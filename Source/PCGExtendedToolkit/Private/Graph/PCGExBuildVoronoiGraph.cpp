﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExBuildVoronoiGraph.h"

#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Geometry/PCGExGeoDelaunay.h"
#include "Geometry/PCGExGeoVoronoi.h"
#include "Graph/PCGExConsolidateCustomGraph.h"
#include "Graph/PCGExCluster.h"

#define LOCTEXT_NAMESPACE "PCGExGraph"
#define PCGEX_NAMESPACE BuildVoronoiGraph

int32 UPCGExBuildVoronoiGraphSettings::GetPreferredChunkSize() const { return 32; }

PCGExData::EInit UPCGExBuildVoronoiGraphSettings::GetMainOutputInitMode() const { return PCGExData::EInit::NewOutput; }

FPCGExBuildVoronoiGraphContext::~FPCGExBuildVoronoiGraphContext()
{
	PCGEX_TERMINATE_ASYNC

	PCGEX_DELETE(GraphBuilder)

	HullIndices.Empty();
}

TArray<FPCGPinProperties> UPCGExBuildVoronoiGraphSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	FPCGPinProperties& PinClustersOutput = PinProperties.Emplace_GetRef(PCGExGraph::OutputEdgesLabel, EPCGDataType::Point);

#if WITH_EDITOR
	PinClustersOutput.Tooltip = FTEXT("Point data representing edges.");
#endif


	return PinProperties;
}

FName UPCGExBuildVoronoiGraphSettings::GetMainOutputLabel() const { return PCGExGraph::OutputVerticesLabel; }

PCGEX_INITIALIZE_ELEMENT(BuildVoronoiGraph)

bool FPCGExBuildVoronoiGraphElement::Boot(FPCGContext* InContext) const
{
	if (!FPCGExPointsProcessorElementBase::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(BuildVoronoiGraph)

	PCGEX_VALIDATE_NAME(Settings->HullAttributeName)
	PCGEX_FWD(GraphBuilderSettings)

	return true;
}

bool FPCGExBuildVoronoiGraphElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExBuildVoronoiGraphElement::Execute);

	PCGEX_CONTEXT_AND_SETTINGS(BuildVoronoiGraph)

	if (Context->IsSetup())
	{
		if (!Boot(Context)) { return true; }
		Context->SetState(PCGExMT::State_ReadyForNextPoints);
	}

	if (Context->IsState(PCGExMT::State_ReadyForNextPoints))
	{
		PCGEX_DELETE(Context->GraphBuilder)
		Context->HullIndices.Empty();

		if (!Context->AdvancePointsIO()) { Context->Done(); }
		else
		{
			if (Context->CurrentIO->GetNum() <= 4)
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT(" (0) Some inputs have too few points to be processed (<= 4)."));
				return false;
			}

			//Context->GraphBuilder = new PCGExGraph::FGraphBuilder(*Context->CurrentIO, &Context->GraphBuilderSettings, 6);
			Context->GetAsyncManager()->Start<FPCGExVoronoi3Task>(Context->CurrentIO->IOIndex, Context->CurrentIO);
			Context->SetAsyncState(PCGExGeo::State_ProcessingVoronoi);
		}
	}

	if (Context->IsState(PCGExGeo::State_ProcessingVoronoi))
	{
		if (!Context->IsAsyncWorkComplete()) { return false; }

		if(Context->GraphBuilder->Graph->Edges.IsEmpty())
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("(1) Some inputs generates no results. Are points coplanar? If so, use Convex Hull 2D instead."));
			Context->SetState(PCGExMT::State_ReadyForNextPoints);
			return false;
		}
		
		Context->GraphBuilder->Compile(Context);
		Context->SetAsyncState(PCGExGraph::State_WritingClusters);
	}

	if (Context->IsState(PCGExGraph::State_WritingClusters))
	{
		if (!Context->IsAsyncWorkComplete()) { return false; }

		if (Context->GraphBuilder->bCompiledSuccessfully) { Context->GraphBuilder->Write(Context); }
		Context->SetState(PCGExMT::State_ReadyForNextPoints);
	}

	if (Context->IsDone())
	{
		Context->OutputPoints();
	}

	return Context->IsDone();
}

bool FPCGExVoronoi3Task::ExecuteTask()
{
	FPCGExBuildVoronoiGraphContext* Context = static_cast<FPCGExBuildVoronoiGraphContext*>(Manager->Context);
	PCGEX_SETTINGS(BuildVoronoiGraph)
	
	PCGExGeo::TVoronoi3* Voronoi = new PCGExGeo::TVoronoi3();

	const TArray<FPCGPoint>& Points = PointIO->GetIn()->GetPoints();
	const int32 NumPoints = Points.Num();

	TArray<FVector> Positions;
	Positions.SetNum(NumPoints);
	for (int i = 0; i < NumPoints; i++) { Positions[i] = Points[i].Transform.GetLocation(); }

	const TArrayView<FVector> View = MakeArrayView(Positions);
	if (!Voronoi->Process(View))
	{
		PCGEX_DELETE(Voronoi)
		return false;
	}

	TArray<FPCGPoint>& Centroids = PointIO->GetOut()->GetMutablePoints();

	const int32 NumSites = Voronoi->Centroids.Num();
	Centroids.SetNum(NumSites);
	
	for (int i = 0; i < NumSites; i++) { Centroids[i].Transform.SetLocation(Voronoi->Centroids[i]); }

	//if (Settings->bMarkHull) { Context->HullIndices.Append(Voronoi->DelaunayHull); }

	Context->GraphBuilder = new PCGExGraph::FGraphBuilder(*PointIO, &Context->GraphBuilderSettings, 6);
	Context->GraphBuilder->Graph->InsertEdges(Voronoi->VoronoiEdges, -1);

	PCGEX_DELETE(Voronoi)
	return true;
}

#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
