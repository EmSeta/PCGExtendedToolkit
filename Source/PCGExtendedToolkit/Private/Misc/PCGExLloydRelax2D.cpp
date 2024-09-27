﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Misc/PCGExLloydRelax2D.h"


#include "Geometry/PCGExGeoDelaunay.h"

#define LOCTEXT_NAMESPACE "PCGExLloydRelax2DElement"
#define PCGEX_NAMESPACE LloydRelax2D

PCGExData::EInit UPCGExLloydRelax2DSettings::GetMainOutputInitMode() const { return PCGExData::EInit::NoOutput; }

PCGEX_INITIALIZE_ELEMENT(LloydRelax2D)

FPCGExLloydRelax2DContext::~FPCGExLloydRelax2DContext()
{
	PCGEX_TERMINATE_ASYNC
}

bool FPCGExLloydRelax2DElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(LloydRelax2D)

	return true;
}

bool FPCGExLloydRelax2DElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExLloydRelax2DElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(LloydRelax2D)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }

		bool bInvalidInputs = false;

		if (!Context->StartBatchProcessingPoints<PCGExPointsMT::TBatch<PCGExLloydRelax2D::FProcessor>>(
			[&](const TSharedPtr<PCGExData::FPointIO>& Entry)
			{
				if (Entry->GetNum() <= 3)
				{
					Entry->InitializeOutput(PCGExData::EInit::Forward);
					bInvalidInputs = true;
					return false;
				}
				return true;
			},
			[&](const TSharedPtr<PCGExPointsMT::TBatch<PCGExLloydRelax2D::FProcessor>>& NewBatch)
			{
			},
			PCGExMT::State_Done))
		{
			PCGE_LOG(Error, GraphAndLog, FTEXT("Could not find any paths to relax."));
			return true;
		}

		if (bInvalidInputs)
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Some inputs have less than 3 points and won't be processed."));
		}
	}

	if (!Context->ProcessPointsBatch()) { return false; }

	Context->MainPoints->OutputToContext();

	return Context->TryComplete();
}

namespace PCGExLloydRelax2D
{
	FProcessor::~FProcessor()
	{
		ActivePositions.Empty();
	}

	bool FProcessor::Process(TSharedPtr<PCGExMT::FTaskManager> InAsyncManager)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGExLloydRelax2D::Process);

		if (!FPointsProcessor::Process(InAsyncManager)) { return false; }

		ProjectionDetails = Settings->ProjectionDetails;
		ProjectionDetails.Init(ExecutionContext, PointDataFacade);

		InfluenceDetails = Settings->InfluenceDetails;
		if (!InfluenceDetails.Init(ExecutionContext, PointDataFacade)) { return false; }

		PointDataFacade->Source->InitializeOutput(PCGExData::EInit::DuplicateInput);
		PCGExGeo::PointsToPositions(PointDataFacade->GetIn()->GetPoints(), ActivePositions);

		AsyncManager->Start<FLloydRelaxTask>(0, PointDataFacade->Source, this, &InfluenceDetails, Settings->Iterations);

		return true;
	}

	void FProcessor::ProcessSinglePoint(const int32 Index, FPCGPoint& Point, const int32 LoopIdx, const int32 Count)
	{
		FVector TargetPosition = Point.Transform.GetLocation();
		TargetPosition.X = ActivePositions[Index].X;
		TargetPosition.Y = ActivePositions[Index].Y;

		Point.Transform.SetLocation(
			InfluenceDetails.bProgressiveInfluence ?
				TargetPosition :
				FMath::Lerp(Point.Transform.GetLocation(), TargetPosition, InfluenceDetails.GetInfluence(Index)));
	}

	void FProcessor::CompleteWork()
	{
		StartParallelLoopForPoints();
	}

	bool FLloydRelaxTask::ExecuteTask(const TSharedPtr<PCGExMT::FTaskManager>& AsyncManager)
	{
		NumIterations--;

		TUniquePtr<PCGExGeo::TDelaunay2> Delaunay = MakeUnique<PCGExGeo::TDelaunay2>();
		TArray<FVector>& Positions = Processor->ActivePositions;

		//FPCGExPointsProcessorContext* Context = static_cast<FPCGExPointsProcessorContext*>(Manager->Context);

		const TArrayView<FVector> View = MakeArrayView(Positions);
		if (!Delaunay->Process(View, Processor->ProjectionDetails)) { return false; }

		const int32 NumPoints = Positions.Num();

		TArray<FVector> Sum;
		TArray<double> Counts;
		Sum.Append(Processor->ActivePositions);
		Counts.SetNum(NumPoints);
		for (int i = 0; i < NumPoints; ++i) { Counts[i] = 1; }

		FVector Centroid;
		for (const PCGExGeo::FDelaunaySite2& Site : Delaunay->Sites)
		{
			PCGExGeo::GetCentroid(Positions, Site.Vtx, Centroid);
			for (const int32 PtIndex : Site.Vtx)
			{
				Counts[PtIndex] += 1;
				Sum[PtIndex] += Centroid;
			}
		}

		if (InfluenceSettings->bProgressiveInfluence)
		{
			for (int i = 0; i < NumPoints; ++i) { Positions[i] = FMath::Lerp(Positions[i], Sum[i] / Counts[i], InfluenceSettings->GetInfluence(i)); }
		}

		Delaunay.Reset();

		if (NumIterations > 0)
		{
			InternalStart<FLloydRelaxTask>(TaskIndex + 1, PointIO, Processor, InfluenceSettings, NumIterations);
		}

		return true;
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
