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

	PCGEX_DELETE_UOBJECT(VtxFiltersData)
	PCGEX_DELETE_UOBJECT(EdgesFiltersData)

	EndpointsLookup.Empty();
}

bool FPCGExEdgesProcessorContext::AdvancePointsIO(const bool bCleanupKeys)
{
	PCGEX_DELETE(CurrentCluster)
	PCGEX_DELETE(ClusterProjection)

	CurrentEdgesIndex = -1;
	EndpointsLookup.Empty();
	EndpointsAdjacency.Empty();

	if (!FPCGExPointsProcessorContext::AdvancePointsIO(bCleanupKeys)) { return false; }

	if (FString CurrentPairId;
		CurrentIO->Tags->GetValue(PCGExGraph::TagStr_ClusterPair, CurrentPairId))
	{
		FString OutId;
		PCGExGraph::SetClusterVtx(CurrentIO, OutId);

		TaggedEdges = InputDictionary->GetEntries(CurrentPairId);
		if (TaggedEdges && !TaggedEdges->Entries.IsEmpty()) { PCGExGraph::MarkClusterEdges(TaggedEdges->Entries, OutId); }
		else { TaggedEdges = nullptr; }
	}
	else { TaggedEdges = nullptr; }

	if (TaggedEdges)
	{
		CurrentIO->CreateInKeys();
		//ProjectionSettings.Init(CurrentIO); // TODO : Move to FClusterProcessor?
		if (bBuildEndpointsLookup) { PCGExGraph::BuildEndpointsLookup(*CurrentIO, EndpointsLookup, EndpointsAdjacency); }
	}
	else
	{
		PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Some input vtx have no associated edges."));
	}

	return true;
}

bool FPCGExEdgesProcessorContext::AdvanceEdges(const bool bBuildCluster, const bool bCleanupKeys)
{
	PCGEX_DELETE_TARRAY(Batches)

	PCGEX_DELETE(CurrentCluster)
	PCGEX_DELETE(ClusterProjection)

	if (bCleanupKeys && CurrentEdges) { CurrentEdges->CleanupKeys(); }

	if (TaggedEdges && TaggedEdges->Entries.IsValidIndex(++CurrentEdgesIndex))
	{
		CurrentEdges = TaggedEdges->Entries[CurrentEdgesIndex];

		if (!bBuildCluster) { return true; }

		CurrentEdges->CreateInKeys();
		CurrentCluster = new PCGExCluster::FCluster();

		if (!CurrentCluster->BuildFrom(
			*CurrentEdges, CurrentIO->GetIn()->GetPoints(),
			EndpointsLookup, &EndpointsAdjacency))
		{
			PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Some clusters are corrupted and will not be processed. \n If you modified vtx/edges manually, make sure to use Sanitize Clusters first."));
			PCGEX_DELETE(CurrentCluster)
		}
		else
		{
			CurrentCluster->PointsIO = CurrentIO;
			CurrentCluster->EdgesIO = CurrentEdges;
		}

		return true;
	}

	CurrentEdges = nullptr;
	return false;
}

bool FPCGExEdgesProcessorContext::ProcessClusters()
{
	if (Batches.IsEmpty()) { return true; }

	if (IsState(PCGExClusterMT::State_WaitingOnClusterProcessing))
	{
		if (!IsAsyncWorkComplete()) { return false; }

		CompleteBatches(GetAsyncManager(), Batches);
		SetAsyncState(PCGExClusterMT::State_WaitingOnClusterCompletedWork);
	}

	if (IsState(PCGExClusterMT::State_WaitingOnClusterCompletedWork))
	{
		if (!IsAsyncWorkComplete()) { return false; }

		if (!bClusterUseGraphBuilder) { SetState(State_ClusterProcessingDone); }
		else
		{
			for (const PCGExClusterMT::FClusterProcessorBatchBase* Batch : Batches) { Batch->GraphBuilder->Compile(GetAsyncManager()); }
			SetAsyncState(PCGExGraph::State_Compiling);
		}
	}

	if (IsState(PCGExGraph::State_Compiling))
	{
		if (!IsAsyncWorkComplete()) { return false; }

		for (const PCGExClusterMT::FClusterProcessorBatchBase* Batch : Batches)
		{
			if (Batch->GraphBuilder->bCompiledSuccessfully) { Batch->GraphBuilder->Write(this); }
		}

		SetState(State_ClusterProcessingDone);
	}

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

	TArray<PCGExData::FPointIO*> TaggedVtx;
	TArray<PCGExData::FPointIO*> TaggedEdges;

	Context->MainEdges = new PCGExData::FPointIOCollection();
	Context->MainEdges->DefaultOutputLabel = PCGExGraph::OutputEdgesLabel;
	TArray<FPCGTaggedData> Sources = Context->InputData.GetInputsByPin(PCGExGraph::SourceEdgesLabel);
	Context->MainEdges->Initialize(Context, Sources, Settings->GetEdgeOutputInitMode());

	// Gather Vtx inputs
	for (PCGExData::FPointIO* MainIO : Context->MainPoints->Pairs)
	{
		if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExVtx))
		{
			if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExEdges))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges -- it will be ignored for safety."));
				continue;
			}

			TaggedVtx.Add(MainIO);
			continue;
		}

		if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExEdges))
		{
			if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExVtx))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges. It will be ignored."));
				continue;
			}

			PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, some Edge data made its way to the vtx input. It will be ignored."));
			continue;
		}

		PCGE_LOG(Warning, GraphAndLog, FTEXT("A data pluggued into Vtx is neither tagged Vtx or Edges and will be ignored."));
	}

	// Gather Edge inputs
	for (PCGExData::FPointIO* MainIO : Context->MainEdges->Pairs)
	{
		if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExEdges))
		{
			if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExVtx))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges. It will be ignored."));
				continue;
			}

			TaggedEdges.Add(MainIO);
			continue;
		}

		if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExVtx))
		{
			if (MainIO->Tags->RawTags.Contains(PCGExGraph::TagStr_PCGExEdges))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges. It will be ignored."));
				continue;
			}

			PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, some Edge data made its way to the vtx input. It will be ignored."));
			continue;
		}

		PCGE_LOG(Warning, GraphAndLog, FTEXT("A data pluggued into Edges is neither tagged Edges or Vtx and will be ignored."));
	}


	for (PCGExData::FPointIO* Vtx : TaggedVtx)
	{
		if (!PCGExGraph::IsPointDataVtxReady(Vtx->GetIn()->Metadata))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("A Vtx input has no metadata and will be discarded."));
			Vtx->Disable();
			continue;
		}

		if (!Context->InputDictionary->CreateKey(*Vtx))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("At least two Vtx inputs share the same PCGEx/Cluster tag. Only one will be processed."));
			Vtx->Disable();
		}
	}

	for (PCGExData::FPointIO* Edges : TaggedEdges)
	{
		if (!PCGExGraph::IsPointDataEdgeReady(Edges->GetIn()->Metadata))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("An Edges input has no edge metadata and will be discarded."));
			Edges->Disable();
			continue;
		}

		if (!Context->InputDictionary->TryAddEntry(*Edges))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Some input edges have no associated vtx."));
		}
	}

	return Context;
}

#undef LOCTEXT_NAMESPACE
