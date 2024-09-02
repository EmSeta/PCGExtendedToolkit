﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Misc/PCGExAssetStaging.h"
#include "AssetSelectors/PCGExInternalCollection.h"

#define LOCTEXT_NAMESPACE "PCGExAssetStagingElement"
#define PCGEX_NAMESPACE AssetStaging

PCGExData::EInit UPCGExAssetStagingSettings::GetMainOutputInitMode() const { return PCGExData::EInit::DuplicateInput; }

PCGEX_INITIALIZE_ELEMENT(AssetStaging)

TArray<FPCGPinProperties> UPCGExAssetStagingSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	if (CollectionSource == EPCGExCollectionSource::AttributeSet) { PCGEX_PIN_PARAM(PCGExAssetCollection::SourceAssetCollection, "Attribute set to be used as collection.", Required, {}) }

	return PinProperties;
}

FPCGExAssetStagingContext::~FPCGExAssetStagingContext()
{
	PCGEX_TERMINATE_ASYNC
	UPCGExInternalCollection* InternalCollection = Cast<UPCGExInternalCollection>(MainCollection);
	PCGEX_DELETE_UOBJECT(InternalCollection)
}

bool FPCGExAssetStagingElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(AssetStaging)

	if (Settings->CollectionSource == EPCGExCollectionSource::Asset)
	{
		Context->MainCollection = Settings->AssetCollection.LoadSynchronous();
	}
	else
	{
		Context->MainCollection = GetDefault<UPCGExInternalCollection>()->GetCollectionFromAttributeSet(
			Context,
			PCGExAssetCollection::SourceAssetCollection,
			Settings->AttributeSetDetails);
	}

	if (!Context->MainCollection)
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Missing asset collection."));
		return false;
	}

	Context->MainCollection->LoadCache(); // Make sure to load the stuff

	PCGEX_VALIDATE_NAME(Settings->AssetPathAttributeName)

	if (Settings->WeightToAttribute == EPCGExWeightOutputMode::Raw || Settings->WeightToAttribute == EPCGExWeightOutputMode::Normalized)
	{
		PCGEX_VALIDATE_NAME(Settings->WeightAttributeName)
	}

	return true;
}

bool FPCGExAssetStagingElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExAssetStagingElement::Execute);

	PCGEX_CONTEXT(AssetStaging)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }

		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExAssetStaging::FProcessor>>(
			[&](PCGExData::FPointIO* Entry) { return true; },
			[&](PCGExPointsMT::TBatch<PCGExAssetStaging::FProcessor>* NewBatch)
			{
			},
			PCGExMT::State_Done))
		{
			PCGE_LOG(Error, GraphAndLog, FTEXT("Could not find any points to process."));
			return true;
		}
	}

	if (!Context->ProcessPointsBatch()) { return false; }

	Context->MainPoints->OutputToContext();

	return Context->TryComplete();
}

namespace PCGExAssetStaging
{
	bool FProcessor::Process(PCGExMT::FTaskManager* AsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExAssetStaging::Process);
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(AssetStaging)

		if (!FPointsProcessor::Process(AsyncManager)) { return false; }

		LocalSettings = Settings;
		LocalTypedContext = TypedContext;

		Justification = Settings->Justification;
		Justification.Init(Context, PointDataFacade);

		NumPoints = PointIO->GetNum();
		PointDataFacade->bSupportsDynamic = true;

		Helper = new PCGExAssetCollection::FDistributionHelper(LocalTypedContext->MainCollection, Settings->DistributionSettings);
		if (!Helper->Init(Context, PointDataFacade)) { return false; }

		bOutputWeight = Settings->WeightToAttribute != EPCGExWeightOutputMode::NoOutput;
		bNormalizedWeight = Settings->WeightToAttribute != EPCGExWeightOutputMode::Raw;
		bOneMinusWeight = Settings->WeightToAttribute == EPCGExWeightOutputMode::NormalizedInverted || Settings->WeightToAttribute == EPCGExWeightOutputMode::NormalizedInvertedToDensity;

		if (Settings->WeightToAttribute == EPCGExWeightOutputMode::Raw)
		{
			WeightWriter = PointDataFacade->GetWriter<int32>(Settings->WeightAttributeName, true);
		}
		else if (Settings->WeightToAttribute == EPCGExWeightOutputMode::Normalized)
		{
			NormalizedWeightWriter = PointDataFacade->GetWriter<double>(Settings->WeightAttributeName, true);
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION > 3
		PathWriter = PointDataFacade->GetWriter<FSoftObjectPath>(Settings->AssetPathAttributeName, false);
#else
		PathWriter = PointDataFacade->GetWriter<FString>(Settings->AssetPathAttributeName, true);
#endif

		StartParallelLoopForPoints();

		return true;
	}

	void FProcessor::PrepareSingleLoopScopeForPoints(const uint32 StartIndex, const int32 Count)
	{
		PointDataFacade->Fetch(StartIndex, Count);
	}

	void FProcessor::ProcessSinglePoint(const int32 Index, FPCGPoint& Point, const int32 LoopIdx, const int32 Count)
	{
		// Note : Prototype implementation

		const FPCGExAssetStagingData* StagingData = nullptr;

		const int32 Seed = PCGExRandom::GetSeedFromPoint(
			Helper->Details.SeedComponents, Point,
			Helper->Details.LocalSeed, LocalSettings, LocalTypedContext->SourceComponent.Get());

		Helper->GetStaging(StagingData, Index, Seed);

		if (!StagingData)
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION > 3
			PathWriter->Values[Index] = FSoftObjectPath{};
#else
			PathWriter->Values[Index] = TEXT("");
#endif

			Point.Density = 0;

			Point.BoundsMin = FVector::ZeroVector;
			Point.BoundsMax = FVector::ZeroVector;

			Point.Transform.SetScale3D(FVector::ZeroVector);

			if (bOutputWeight)
			{
				if (WeightWriter) { WeightWriter->Values[Index] = -1; }
				else if (NormalizedWeightWriter) { NormalizedWeightWriter->Values[Index] = -1; }
				else { Point.Density = -1; }
			}

			return;
		}

		if (bOutputWeight)
		{
			double Weight = bNormalizedWeight ? static_cast<double>(StagingData->Weight) / static_cast<double>(LocalTypedContext->MainCollection->LoadCache()->WeightSum) : StagingData->Weight;
			if (bOneMinusWeight) { Weight = 1 - Weight; }
			if (WeightWriter) { WeightWriter->Values[Index] = Weight; }
			else if (NormalizedWeightWriter) { NormalizedWeightWriter->Values[Index] = Weight; }
			else { Point.Density = Weight; }
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION > 3
		PathWriter->Values[Index] = StagingData->Path;
#else
		PathWriter->Values[Index] = StagingData->Path.ToString();
#endif


		const FBox& StBox = StagingData->Bounds;
		FVector OutScale = Point.Transform.GetScale3D();
		const FBox InBounds = FBox(Point.BoundsMin * OutScale, Point.BoundsMax * OutScale);
		FBox OutBounds = StBox;

		LocalSettings->ScaleToFit.Process(Point, StagingData->Bounds, OutScale, OutBounds);

		Point.BoundsMin = OutBounds.Min;
		Point.BoundsMax = OutBounds.Max;

		FVector OutTranslation = FVector::ZeroVector;
		OutBounds = FBox(OutBounds.Min * OutScale, OutBounds.Max * OutScale);

		Justification.Process(Index, InBounds, OutBounds, OutTranslation);

		Point.Transform.AddToTranslation(Point.Transform.GetRotation().RotateVector(OutTranslation));
		Point.Transform.SetScale3D(OutScale);
	}

	void FProcessor::CompleteWork()
	{
		PointDataFacade->Write(AsyncManagerPtr, true);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
