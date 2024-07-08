﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Pathfinding/PCGExPathfindingFindContours.h"

#include "Graph/Pathfinding/PCGExPathfinding.h"

#define LOCTEXT_NAMESPACE "PCGExFindContours"
#define PCGEX_NAMESPACE FindContours

TArray<FPCGPinProperties> UPCGExFindContoursSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();
	PCGEX_PIN_POINTS(PCGExGraph::SourceSeedsLabel, "Seeds associated with the main input points", Required, {})
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExFindContoursSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties;
	PCGEX_PIN_POINTS(PCGExGraph::OutputPathsLabel, "Contours", Required, {})
	return PinProperties;
}

PCGExData::EInit UPCGExFindContoursSettings::GetEdgeOutputInitMode() const { return PCGExData::EInit::NoOutput; }
PCGExData::EInit UPCGExFindContoursSettings::GetMainOutputInitMode() const { return PCGExData::EInit::NoOutput; }

bool FPCGExFindContoursContext::TryFindContours(PCGExData::FPointIO* PathIO, const int32 SeedIndex, const PCGExFindContours::FProcessor* ClusterProcessor)
{
	PCGEX_SETTINGS_LOCAL(FindContours)

	PCGExCluster::FCluster* Cluster = ClusterProcessor->Cluster;

	TArray<PCGExCluster::FExpandedNode*>* ExpandedNodes = ClusterProcessor->ExpandedNodes;
	TArray<PCGExCluster::FExpandedEdge*>* ExpandedEdges = ClusterProcessor->ExpandedEdges;

	const TArray<FVector>& Positions = *ClusterProcessor->ProjectedPositions;
	const TArray<PCGExCluster::FNode>& NodesRef = *Cluster->Nodes;

	const FVector Guide = ProjectedSeeds[SeedIndex];
	int32 StartNodeIndex = Cluster->FindClosestNode(Guide, Settings->SeedPicking.PickingMethod, 2);
	int32 NextEdge = Cluster->FindClosestEdge(StartNodeIndex, Guide);

	if (StartNodeIndex == -1 || NextEdge == -1 ||
		(Cluster->Nodes->GetData() + StartNodeIndex)->Adjacency.Num() <= 1)
	{
		// Fail. Either single-node or single-edge cluster, or no connected edge
		return false;
	}

	const FVector SeedPosition = NodesRef[StartNodeIndex].Position;
	if (!Settings->SeedPicking.WithinDistance(SeedPosition, Guide))
	{
		// Fail. Not within radius.
		return false;
	}

	int32 PrevIndex = StartNodeIndex;
	int32 NextIndex = (*(ExpandedEdges->GetData() + NextEdge))->OtherNodeIndex(PrevIndex);


	const FVector A = (*(ExpandedNodes->GetData() + PrevIndex))->Node->Position;
	const FVector B = (*(ExpandedNodes->GetData() + NextIndex))->Node->Position;

	const double SanityAngle = PCGExMath::GetDegreesBetweenVectors((B - A).GetSafeNormal(), (B - Guide).GetSafeNormal());

	if (SanityAngle > 180)
	{
		// Swap search orientation
		PrevIndex = NextIndex;
		NextIndex = StartNodeIndex;
		StartNodeIndex = PrevIndex;
	}

	TArray<int32> Path;
	Path.Add(PrevIndex);
	TSet<int32> Exclusions = {PrevIndex, NextIndex};

	bool bIsConvex = true;
	int32 Sign = 0;

	bool bGracefullyClosed = false;
	while (NextIndex != -1)
	{
		Path.Add(NextIndex);

		PCGExCluster::FExpandedNode* Current = *(ExpandedNodes->GetData() + NextIndex);
		//	if (Current->Neighbors.Num() <= 1) { break; }

		const FVector Origin = Positions[(Cluster->Nodes->GetData() + NextIndex)->PointIndex];
		const FVector GuideDir = (Origin - Positions[(Cluster->Nodes->GetData() + PrevIndex)->PointIndex]).GetSafeNormal();

		double BestAngle = -1;
		int32 NextBest = -1;

		if (Current->Neighbors.Num() > 1) { Exclusions.Add(PrevIndex); }

		for (const PCGExCluster::FExpandedNeighbor& N : Current->Neighbors)
		{
			const int32 NeighborIndex = N.Node->NodeIndex;
			if (Exclusions.Contains(NeighborIndex)) { continue; }
			if (NeighborIndex == StartNodeIndex)
			{
				bGracefullyClosed = true;
				NextBest = -1;
				break;
			}

			const FVector OtherDir = (Origin - Positions[(Cluster->Nodes->GetData() + NeighborIndex)->PointIndex]).GetSafeNormal();
			const double Angle = PCGExMath::GetDegreesBetweenVectors(OtherDir, GuideDir);

			if (Angle > BestAngle)
			{
				BestAngle = Angle;
				NextBest = NeighborIndex;
			}
		}

		Exclusions.Empty();

		if (NextBest != -1)
		{
			if (Settings->OutputType != EPCGExContourShapeTypeOutput::Both && Path.Num() > 2)
			{
				PCGExMath::CheckConvex(
					(Cluster->Nodes->GetData() + Path.Last(2))->Position,
					(Cluster->Nodes->GetData() + Path.Last(1))->Position,
					(Cluster->Nodes->GetData() + Path.Last())->Position,
					bIsConvex, Sign);

				if (!bIsConvex && Settings->OutputType == EPCGExContourShapeTypeOutput::ConvexOnly) { return false; }
			}

			PrevIndex = NextIndex;
			NextIndex = NextBest;
		}
		else
		{
			NextIndex = -1;
		}
	}

	if ((Settings->bKeepOnlyGracefulContours && !bGracefullyClosed) ||
		(bIsConvex && Settings->OutputType == EPCGExContourShapeTypeOutput::ConcaveOnly))
	{
		return false;
	}

	PCGExGraph::CleanupClusterTags(PathIO, true);
	PCGExGraph::CleanupVtxData(PathIO);

	TArray<FPCGPoint>& MutablePoints = PathIO->GetOut()->GetMutablePoints();
	const TArray<FPCGPoint>& OriginPoints = PathIO->GetIn()->GetPoints();
	MutablePoints.SetNumUninitialized(Path.Num());

	const TArray<int32>& VtxPointIndices = Cluster->GetVtxPointIndices();
	for (int i = 0; i < Path.Num(); i++) { MutablePoints[i] = OriginPoints[VtxPointIndices[Path[i]]]; }

	const FPCGExFindContoursContext* TypedContext = static_cast<FPCGExFindContoursContext*>(ClusterProcessor->Context);
	TypedContext->SeedAttributesToPathTags.Tag(SeedIndex, PathIO);

	TypedContext->SeedForwardHandler->Forward(SeedIndex, PathIO);

	if (Sign != 0)
	{
		if (Settings->bTagConcave && !bIsConvex) { PathIO->Tags->RawTags.Add(Settings->ConcaveTag); }
		if (Settings->bTagConvex && bIsConvex) { PathIO->Tags->RawTags.Add(Settings->ConvexTag); }
	}

	return true;
}

PCGEX_INITIALIZE_ELEMENT(FindContours)

FPCGExFindContoursContext::~FPCGExFindContoursContext()
{
	PCGEX_TERMINATE_ASYNC

	if (SeedsDataFacade) { PCGEX_DELETE(SeedsDataFacade->Source) }
	PCGEX_DELETE(SeedsDataFacade)

	PCGEX_DELETE(Paths)

	SeedAttributesToPathTags.Cleanup();
	PCGEX_DELETE(SeedForwardHandler)

	ProjectedSeeds.Empty();
}


bool FPCGExFindContoursElement::Boot(FPCGContext* InContext) const
{
	if (!FPCGExEdgesProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(FindContours)

	PCGEX_FWD(ProjectionDetails)

	PCGExData::FPointIO* SeedsPoints = Context->TryGetSingleInput(PCGExGraph::SourceSeedsLabel, true);
	if (!SeedsPoints) { return false; }
	Context->SeedsDataFacade = new PCGExData::FFacade(SeedsPoints);

	if (!Context->ProjectionDetails.Init(Context, Context->SeedsDataFacade)) { return false; }

	PCGEX_FWD(SeedAttributesToPathTags)
	if (!Context->SeedAttributesToPathTags.Init(Context, Context->SeedsDataFacade)) { return false; }
	Context->SeedForwardHandler = new PCGExData::FDataForwardHandler(Settings->SeedForwardAttributes, SeedsPoints);

	Context->Paths = new PCGExData::FPointIOCollection();
	Context->Paths->DefaultOutputLabel = PCGExGraph::OutputPathsLabel;

	return true;
}

bool FPCGExFindContoursElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExFindContoursElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(FindContours)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }
		Context->SetState(PCGExMT::State_ProcessingTargets);
	}

	if (Context->IsState(PCGExMT::State_ProcessingTargets))
	{
		const TArray<FPCGPoint>& Seeds = Context->SeedsDataFacade->GetIn()->GetPoints();

		auto Initialize = [&]()
		{
			Context->ProjectedSeeds.SetNumUninitialized(Seeds.Num());
		};

		auto ProjectSeed = [&](const int32 Index)
		{
			Context->ProjectedSeeds[Index] = Context->ProjectionDetails.Project(Seeds[Index].Transform.GetLocation(), Index);
		};

		if (!Context->Process(Initialize, ProjectSeed, Seeds.Num())) { return false; }

		if (!Context->StartProcessingClusters<PCGExFindContours::FBatch>(
			[](PCGExData::FPointIOTaggedEntries* Entries) { return true; },
			[&](PCGExFindContours::FBatch* NewBatch)
			{
			},
			PCGExMT::State_Done))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Could not build any clusters."));
			return true;
		}
	}

	if (!Context->ProcessClusters()) { return false; }

	if (Context->IsDone())
	{
		Context->Paths->OutputTo(Context);
	}

	return Context->TryComplete();
}


namespace PCGExFindContours
{
	FProcessor::~FProcessor()
	{
		if (bBuildExpandedNodes) { PCGEX_DELETE_TARRAY_FULL(ExpandedNodes) }
	}

	bool FProcessor::Process(PCGExMT::FTaskManager* AsyncManager)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(FindContours)

		if (!FClusterProcessor::Process(AsyncManager)) { return false; }

		if (Settings->bUseOctreeSearch) { Cluster->RebuildOctree(Settings->SeedPicking.PickingMethod); }
		Cluster->RebuildOctree(EPCGExClusterClosestSearchMode::Edge); // We need edge octree anyway

		ExpandedNodes = Cluster->ExpandedNodes;
		ExpandedEdges = Cluster->GetExpandedEdges(true);

		if (!ExpandedNodes)
		{
			ExpandedNodes = Cluster->GetExpandedNodes(false);
			bBuildExpandedNodes = true;
			StartParallelLoopForRange(NumNodes);
		}

		return true;
	}

	void FProcessor::ProcessSingleRangeIteration(const int32 Iteration)
	{
		(*ExpandedNodes)[Iteration] = new PCGExCluster::FExpandedNode(Cluster, Iteration);
	}

	void FProcessor::CompleteWork()
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(FindContours)

		if (IsTrivial())
		{
			for (int i = 0; i < TypedContext->ProjectedSeeds.Num(); i++)
			{
				TypedContext->TryFindContours(TypedContext->Paths->Emplace_GetRef<UPCGPointData>(VtxIO, PCGExData::EInit::NewOutput), i, this);
			}
		}
		else
		{
			for (int i = 0; i < TypedContext->ProjectedSeeds.Num(); i++)
			{
				AsyncManagerPtr->Start<FPCGExFindContourTask>(i, TypedContext->Paths->Emplace_GetRef<UPCGPointData>(VtxIO, PCGExData::EInit::NewOutput), this);
			}
		}
	}

	void FBatch::Process(PCGExMT::FTaskManager* AsyncManager)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(FindContours)

		// Project positions
		ProjectionDetails = Settings->ProjectionDetails;
		if (!ProjectionDetails.Init(Context, VtxDataFacade)) { return; }

		PCGEX_SET_NUM_UNINITIALIZED(ProjectedPositions, VtxIO->GetNum())

		ProjectionTaskGroup = AsyncManager->CreateGroup();
		ProjectionTaskGroup->StartRanges(
			[&](const int32 Index)
			{
				ProjectedPositions[Index] = ProjectionDetails.ProjectFlat(VtxIO->GetInPoint(Index).Transform.GetLocation(), Index);
			},
			VtxIO->GetNum(), GetDefault<UPCGExGlobalSettings>()->GetPointsBatchIteration());

		TBatch<FProcessor>::Process(AsyncManager);
	}

	bool FBatch::PrepareSingle(FProcessor* ClusterProcessor)
	{
		ClusterProcessor->ProjectedPositions = &ProjectedPositions;
		TBatch<FProcessor>::PrepareSingle(ClusterProcessor);
		return true;
	}

	bool FProjectRangeTask::ExecuteTask()
	{
		for (int i = 0; i < NumIterations; i++)
		{
			const int32 Index = TaskIndex + i;
			Batch->ProjectedPositions[Index] = Batch->ProjectionDetails.ProjectFlat(Batch->VtxIO->GetInPoint(Index).Transform.GetLocation(), Index);
		}
		return true;
	}

	bool FPCGExFindContourTask::ExecuteTask()
	{
		FPCGExFindContoursContext* Context = Manager->GetContext<FPCGExFindContoursContext>();
		return Context->TryFindContours(PointIO, TaskIndex, ClusterProcessor);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
