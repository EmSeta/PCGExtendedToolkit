﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExEdgesProcessor.h"

#include "Data/PCGExGraphDefinition.h"

#define LOCTEXT_NAMESPACE "PCGExGraphSettings"

#pragma region UPCGSettings interface


PCGExData::EInit UPCGExEdgesProcessorSettings::GetMainOutputInitMode() const { return PCGExData::EInit::Forward; }

FName UPCGExEdgesProcessorSettings::GetMainInputLabel() const { return PCGExGraph::SourceVerticesLabel; }
FName UPCGExEdgesProcessorSettings::GetMainOutputLabel() const { return PCGExGraph::OutputVerticesLabel; }

FName UPCGExEdgesProcessorSettings::GetVtxFilterLabel() const { return NAME_None; }
FName UPCGExEdgesProcessorSettings::GetEdgesFilterLabel() const { return NAME_None; }

bool UPCGExEdgesProcessorSettings::SupportsVtxFilters() const { return !GetVtxFilterLabel().IsNone(); }
bool UPCGExEdgesProcessorSettings::SupportsEdgesFilters() const { return !GetEdgesFilterLabel().IsNone(); }

PCGExData::EInit UPCGExEdgesProcessorSettings::GetEdgeOutputInitMode() const { return PCGExData::EInit::Forward; }

bool UPCGExEdgesProcessorSettings::RequiresDeterministicClusters() const { return false; }

bool UPCGExEdgesProcessorSettings::GetMainAcceptMultipleData() const { return true; }

TArray<FPCGPinProperties> UPCGExEdgesProcessorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINTS(PCGExGraph::SourceEdgesLabel, "Edges associated with the main input points", Required, {})
	if (SupportsVtxFilters()) { PCGEX_PIN_PARAMS(GetVtxFilterLabel(), "Vtx filters", Advanced, {}) }
	if (SupportsEdgesFilters()) { PCGEX_PIN_PARAMS(GetEdgesFilterLabel(), "Edges filters", Advanced, {}) }
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExEdgesProcessorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExGraph::OutputEdgesLabel, "Edges associated with the main output points", Required, {})
	return PinProperties;
}

#pragma endregion

FPCGExEdgesProcessorContext::~FPCGExEdgesProcessorContext()
{
	PCGEX_TERMINATE_ASYNC

	PCGEX_DELETE(InputDictionary)
	PCGEX_DELETE(MainEdges)
	PCGEX_DELETE(CurrentCluster)
	PCGEX_DELETE(ClusterProjection)

	VtxIndices.Empty();

	PCGEX_DELETE(VtxFiltersData)
	PCGEX_DELETE(VtxFiltersHandler)
	VtxFilterResults.Empty();

	PCGEX_DELETE(EdgesFiltersData)
	PCGEX_DELETE(EdgesFiltersHandler)
	EdgeFilterResults.Empty();

	EndpointsLookup.Empty();
	ProjectionSettings.Cleanup();
}


bool FPCGExEdgesProcessorContext::ProcessorAutomation()
{
	if (!FPCGExPointsProcessorContext::ProcessorAutomation()) { return false; }
	if (!ProcessFilters()) { return false; }
	if (!ProjectCluster()) { return false; }
	return true;
}

bool FPCGExEdgesProcessorContext::AdvancePointsIO()
{
	PCGEX_DELETE(CurrentCluster)
	PCGEX_DELETE(ClusterProjection)

	CurrentEdgesIndex = -1;
	EndpointsLookup.Empty();
	EndpointsAdjacency.Empty();

	if (!FPCGExPointsProcessorContext::AdvancePointsIO()) { return false; }

	if (FString CurrentPairId;
		CurrentIO->Tags->GetValue(PCGExGraph::TagStr_ClusterPair, CurrentPairId))
	{
		FString OutId;
		CurrentIO->Tags->Set(PCGExGraph::TagStr_ClusterPair, CurrentIO->GetOutIn()->UID, OutId);

		TaggedEdges = InputDictionary->GetEntries(CurrentPairId);
		if (TaggedEdges && !TaggedEdges->Entries.IsEmpty())
		{
			for (const PCGExData::FPointIO* EdgeIO : TaggedEdges->Entries) { EdgeIO->Tags->Set(PCGExGraph::TagStr_ClusterPair, OutId); }
		}
		else { TaggedEdges = nullptr; }
	}
	else { TaggedEdges = nullptr; }

	if (TaggedEdges)
	{
		CurrentIO->CreateInKeys();
		ProjectionSettings.Init(CurrentIO);
		PCGExGraph::BuildEndpointsLookup(*CurrentIO, EndpointsLookup, EndpointsAdjacency);
	}
	else
	{
		PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Some input vtx have no associated edges."));
	}

	return true;
}

bool FPCGExEdgesProcessorContext::AdvanceEdges(const bool bBuildCluster)
{
	PCGEX_DELETE(CurrentCluster)
	PCGEX_DELETE(ClusterProjection)

	PCGEX_DELETE(VtxFiltersHandler)
	PCGEX_DELETE(EdgesFiltersHandler)

	if (bBuildCluster && CurrentEdges) { CurrentEdges->CleanupKeys(); }

	if (TaggedEdges && TaggedEdges->Entries.IsValidIndex(++CurrentEdgesIndex))
	{
		CurrentEdges = TaggedEdges->Entries[CurrentEdgesIndex];

		if (!bBuildCluster) { return true; }

		CurrentEdges->CreateInKeys();
		CurrentCluster = new PCGExCluster::FCluster();

		if (!CurrentCluster->BuildFrom(
			*CurrentEdges, GetCurrentIn()->GetPoints(),
			EndpointsLookup, &EndpointsAdjacency))
		{
			PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Some clusters are corrupted and will not be processed. \n If you modified vtx/edges manually, make sure to use Sanitize Clusters first."));
			PCGEX_DELETE(CurrentCluster)
		}
		else
		{
			CurrentCluster->PointsIO = CurrentIO;
			CurrentCluster->EdgesIO = CurrentEdges;

			bWaitingOnFilterWork = false;
			bRequireVtxFilterPreparation = false;

			PCGEX_SETTINGS_LOCAL(EdgesProcessor)

			const bool DefaultResult = DefaultVtxFilterResult();

			if (VtxFiltersData)
			{
				VtxFilterResults.SetNumUninitialized(CurrentCluster->Nodes.Num());
				VtxIndices.SetNumUninitialized(CurrentCluster->Nodes.Num());

				for (int i = 0; i < VtxIndices.Num(); i++)
				{
					VtxIndices[i] = CurrentCluster->Nodes[i].PointIndex;
					VtxFilterResults[i] = DefaultResult;
				}

				VtxFiltersHandler = static_cast<PCGExCluster::FNodeStateHandler*>(VtxFiltersData->CreateFilter());
				VtxFiltersHandler->bCacheResults = false;
				VtxFiltersHandler->CaptureCluster(this, CurrentCluster);

				bRequireVtxFilterPreparation = VtxFiltersHandler->PrepareForTesting(CurrentIO, VtxIndices);
				bWaitingOnFilterWork = true;
			}
			else if (Settings->SupportsVtxFilters())
			{
				VtxFilterResults.SetNumUninitialized(CurrentCluster->Nodes.Num());
				for (int i = 0; i < VtxFilterResults.Num(); i++) { VtxFilterResults[i] = DefaultResult; }
			}

			bRequireEdgesFilterPreparation = false;
			if (EdgesFiltersData)
			{
				EdgeFilterResults.SetNumUninitialized(CurrentEdges->GetNum());

				// TODO: Implement 
				//VtxFiltersHandler = static_cast<PCGExCluster::FNodeStateHandler*>(VtxFiltersData->CreateFilter());
				//EdgesFiltersHandler->CaptureCluster(this, CurrentCluster);
			}
		}

		return true;
	}

	CurrentEdges = nullptr;
	return false;
}

bool FPCGExEdgesProcessorContext::ProcessFilters()
{
	if (!bWaitingOnFilterWork) { return true; }

	if (bRequireVtxFilterPreparation)
	{
		auto PrepareVtx = [&](const int32 Index) { VtxFiltersHandler->PrepareSingle(CurrentCluster->Nodes[Index].PointIndex); };
		if (!Process(PrepareVtx, CurrentCluster->Nodes.Num())) { return false; }
		bRequireVtxFilterPreparation = false;
	}

	if (bRequireEdgesFilterPreparation)
	{
		auto PrepareEdge = [&](const int32 Index) { EdgesFiltersHandler->PrepareSingle(Index); };
		if (!Process(PrepareEdge, CurrentCluster->Edges.Num())) { return false; }
		bRequireVtxFilterPreparation = false;
	}

	if (VtxFiltersHandler)
	{
		auto FilterVtx = [&](const int32 Index) { VtxFilterResults[Index] = VtxFiltersHandler->Test(Index); };
		if (!Process(FilterVtx, CurrentCluster->Nodes.Num())) { return false; }
		PCGEX_DELETE(VtxFiltersHandler)
	}

	if (EdgesFiltersHandler)
	{
		auto FilterEdge = [&](const int32 Index) { EdgeFilterResults[Index] = EdgesFiltersHandler->Test(Index); };
		if (!Process(FilterEdge, CurrentCluster->Edges.Num())) { return false; }
		PCGEX_DELETE(EdgesFiltersHandler)
	}

	bWaitingOnFilterWork = false;

	return true;
}

bool FPCGExEdgesProcessorContext::DefaultVtxFilterResult() const { return true; }

bool FPCGExEdgesProcessorContext::ProjectCluster()
{
	if (!bWaitingOnClusterProjection) { return true; }

	auto Initialize = [&]()
	{
		PCGEX_DELETE(ClusterProjection)
		ClusterProjection = new PCGExCluster::FClusterProjection(CurrentCluster, &ProjectionSettings);
	};

	auto ProjectSinglePoint = [&](const int32 Index) { ClusterProjection->Nodes[Index].Project(CurrentCluster, &ProjectionSettings); };

	if (!Process(Initialize, ProjectSinglePoint, CurrentCluster->Nodes.Num())) { return false; }

	bWaitingOnClusterProjection = false;

	return true;
}

void FPCGExEdgesProcessorContext::OutputPointsAndEdges()
{
	MainPoints->OutputTo(this);
	MainEdges->OutputTo(this);
}

PCGEX_INITIALIZE_CONTEXT(EdgesProcessor)

void FPCGExEdgesProcessorElement::DisabledPassThroughData(FPCGContext* Context) const
{
	FPCGExPointsProcessorElementBase::DisabledPassThroughData(Context);

	//Forward main edges
	TArray<FPCGTaggedData> EdgesSources = Context->InputData.GetInputsByPin(PCGExGraph::SourceEdgesLabel);
	for (const FPCGTaggedData& TaggedData : EdgesSources)
	{
		FPCGTaggedData& TaggedDataCopy = Context->OutputData.TaggedData.Emplace_GetRef();
		TaggedDataCopy.Data = TaggedData.Data;
		TaggedDataCopy.Tags.Append(TaggedData.Tags);
		TaggedDataCopy.Pin = PCGExGraph::OutputEdgesLabel;
	}
}

bool FPCGExEdgesProcessorElement::Boot(FPCGContext* InContext) const
{
	if (!FPCGExPointsProcessorElementBase::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(EdgesProcessor)

	Context->bDeterministicClusters = Settings->RequiresDeterministicClusters();

	if (Context->MainEdges->IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Missing Edges."));
		return false;
	}

	if (Settings->SupportsVtxFilters())
	{
		TArray<UPCGExFilterFactoryBase*> FilterFactories;
		if (GetInputFactories(InContext, Settings->GetVtxFilterLabel(), FilterFactories, PCGExFactories::ClusterFilters, false))
		{
			Context->VtxFiltersData = NewObject<UPCGExNodeStateFactory>();
			Context->VtxFiltersData->FilterFactories.Append(FilterFactories);
		}
	}

	if (Settings->SupportsEdgesFilters())
	{
		TArray<UPCGExFilterFactoryBase*> FilterFactories;
		if (GetInputFactories(InContext, Settings->GetEdgesFilterLabel(), FilterFactories, PCGExFactories::ClusterFilters, false))
		{
			Context->EdgesFiltersData = NewObject<UPCGExNodeStateFactory>();
			Context->EdgesFiltersData->FilterFactories.Append(FilterFactories);
		}
	}

	return true;
}

FPCGContext* FPCGExEdgesProcessorElement::InitializeContext(
	FPCGExPointsProcessorContext* InContext,
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node) const
{
	FPCGExPointsProcessorElementBase::InitializeContext(InContext, InputData, SourceComponent, Node);

	PCGEX_CONTEXT_AND_SETTINGS(EdgesProcessor)

	if (!Settings->bEnabled) { return Context; }

	Context->InputDictionary = new PCGExData::FPointIOTaggedDictionary(PCGExGraph::TagStr_ClusterPair);
	Context->MainPoints->ForEach(
		[&](PCGExData::FPointIO& PointIO, int32)
		{
			if (!PCGExGraph::IsPointDataVtxReady(PointIO.GetIn()))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("A Vtx input has no metadata and will be discarded."));
				PointIO.Disable();
				return;
			}
			if (!Context->InputDictionary->CreateKey(PointIO))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("At least two Vtx inputs share the same PCGEx/Cluster tag. Only one will be processed."));
				PointIO.Disable();
			}
		});

	Context->MainEdges = new PCGExData::FPointIOCollection();
	Context->MainEdges->DefaultOutputLabel = PCGExGraph::OutputEdgesLabel;
	TArray<FPCGTaggedData> Sources = Context->InputData.GetInputsByPin(PCGExGraph::SourceEdgesLabel);
	Context->MainEdges->Initialize(Context, Sources, Settings->GetEdgeOutputInitMode());

	Context->MainEdges->ForEach(
		[&](PCGExData::FPointIO& PointIO, int32)
		{
			if (!PCGExGraph::IsPointDataEdgeReady(PointIO.GetIn()))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("An Edges input has no edge metadata and will be discarded."));
				PointIO.Disable();
				return;
			}
			Context->InputDictionary->TryAddEntry(PointIO);
		});

	return Context;
}

#undef LOCTEXT_NAMESPACE
