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
	const TArray<PCGExCluster::FNode>& NodesRef = *Cluster->Nodes;

	PCGExCluster::FClusterProjection* Projection = ClusterProcessor->ClusterProjection;

	const FVector Guide = ProjectedSeeds[SeedIndex];
	const int32 StartNodeIndex = Cluster->FindClosestNode(Guide, Settings->SeedPicking.PickingMethod, 2);

	if (StartNodeIndex == -1)
	{
		// Fail. Either single-node or single-edge cluster
		return false;
	}

	const FVector SeedPosition = NodesRef[StartNodeIndex].Position;
	if (!Settings->SeedPicking.WithinDistance(SeedPosition, Guide))
	{
		// Fail. Not within radius.
		return false;
	}

	const FVector InitialDir = PCGExMath::GetNormal(SeedPosition, Guide, Guide + FVector::UpVector);
	const int32 NextToStartIndex = Cluster->FindClosestNeighborInDirection(StartNodeIndex, InitialDir, 2);

	if (NextToStartIndex == -1)
	{
		// Fail. Either single-node or single-edge cluster
		return false;
	}

	TArray<int32> Path;
	TSet<int32> Visited;

	int32 PreviousIndex = StartNodeIndex;
	int32 NextIndex = NextToStartIndex;

	Path.Add(StartNodeIndex);
	Path.Add(NextToStartIndex);
	Visited.Add(NextToStartIndex);

	TSet<int32> Exclusion = {PreviousIndex, NextIndex};
	PreviousIndex = NextToStartIndex;
	NextIndex = Projection->FindNextAdjacentNode(Settings->OrientationMode, NextToStartIndex, StartNodeIndex, Exclusion, 2);

	while (NextIndex != -1)
	{
		if (NextIndex == StartNodeIndex) { break; } // Contour closed gracefully

		const PCGExCluster::FNode& CurrentNode = NodesRef[NextIndex];

		Path.Add(NextIndex);

		if (CurrentNode.IsAdjacentTo(StartNodeIndex)) { break; } // End is in the immediate vicinity

		Exclusion.Empty();
		if (CurrentNode.Adjacency.Num() > 1) { Exclusion.Add(PreviousIndex); }

		const int32 FromIndex = PreviousIndex;
		PreviousIndex = NextIndex;
		NextIndex = Projection->FindNextAdjacentNode(Settings->OrientationMode, NextIndex, FromIndex, Exclusion, 1);
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

	PCGExData::FPointIO* SeedsPoints = Context->TryGetSingleInput(PCGExGraph::SourceSeedsLabel, true);
	if (!SeedsPoints) { return false; }
	Context->SeedsDataFacade = new PCGExData::FFacade(SeedsPoints);

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
			Context->ProjectedSeeds[Index] = Settings->ProjectionDetails.Project(Seeds[Index].Transform.GetLocation());
		};

		if (!Context->Process(Initialize, ProjectSeed, Seeds.Num())) { return false; }

		if (!Context->StartProcessingClusters<PCGExClusterMT::TBatch<PCGExFindContours::FProcessor>>(
			[](PCGExData::FPointIOTaggedEntries* Entries) { return true; },
			[&](PCGExClusterMT::TBatch<PCGExFindContours::FProcessor>* NewBatch)
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
		PCGEX_DELETE(ClusterProjection)
	}

	bool FProcessor::Process(PCGExMT::FTaskManager* AsyncManager)
	{
		PCGEX_TYPED_CONTEXT_AND_SETTINGS(FindContours)

		if (!FClusterProcessor::Process(AsyncManager)) { return false; }

		if (Settings->bUseOctreeSearch) { Cluster->RebuildOctree(Settings->SeedPicking.PickingMethod); }

		ProjectionDetails = Settings->ProjectionDetails;
		ProjectionDetails.Init(Context, VtxDataFacade);
		ClusterProjection = new PCGExCluster::FClusterProjection(Cluster, &ProjectionDetails);

		StartParallelLoopForNodes();

		return true;
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

	void FProcessor::ProcessSingleNode(const int32 Index, PCGExCluster::FNode& Node)
	{
		ClusterProjection->Nodes[Node.NodeIndex].Project(Cluster, &ProjectionDetails);
	}

	bool FPCGExFindContourTask::ExecuteTask()
	{
		FPCGExFindContoursContext* Context = Manager->GetContext<FPCGExFindContoursContext>();
		return Context->TryFindContours(PointIO, TaskIndex, ClusterProcessor);
	}
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
