﻿// Copyright Timothé Lapetite 2023
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExFindEdgePatches.h"

#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Graph/PCGExGraphPatch.h"

#define LOCTEXT_NAMESPACE "PCGExFindEdgePatches"

int32 UPCGExFindEdgePatchesSettings::GetPreferredChunkSize() const { return 32; }

PCGExPointIO::EInit UPCGExFindEdgePatchesSettings::GetPointOutputInitMode() const { return PCGExPointIO::EInit::DuplicateInput; }

TArray<FPCGPinProperties> UPCGExFindEdgePatchesSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	FPCGPinProperties& PinPatchesOutput = PinProperties.Emplace_GetRef(PCGExGraph::OutputPatchesLabel, EPCGDataType::Point);

#if WITH_EDITOR
	PinPatchesOutput.Tooltip = LOCTEXT("PCGExOutputParamsTooltip", "Point data representing edges.");
#endif // WITH_EDITOR

	PCGEx::Swap(PinProperties, PinProperties.Num() - 1, PinProperties.Num() - 2);
	return PinProperties;
}

FPCGElementPtr UPCGExFindEdgePatchesSettings::CreateElement() const
{
	return MakeShared<FPCGExFindEdgePatchesElement>();
}

FPCGContext* FPCGExFindEdgePatchesElement::Initialize(
	const FPCGDataCollection& InputData,
	TWeakObjectPtr<UPCGComponent> SourceComponent,
	const UPCGNode* Node)
{
	FPCGExFindEdgePatchesContext* Context = new FPCGExFindEdgePatchesContext();
	InitializeContext(Context, InputData, SourceComponent, Node);

	const UPCGExFindEdgePatchesSettings* Settings = Context->GetInputSettings<UPCGExFindEdgePatchesSettings>();
	check(Settings);

	Context->CrawlEdgeTypes = static_cast<EPCGExEdgeType>(Settings->CrawlEdgeTypes);
	Context->bRemoveSmallPatches = Settings->bRemoveSmallPatches;
	Context->MinPatchSize = Settings->bRemoveSmallPatches ? Settings->MinPatchSize : -1;
	Context->bRemoveBigPatches = Settings->bRemoveBigPatches;
	Context->MaxPatchSize = Settings->bRemoveBigPatches ? Settings->MaxPatchSize : -1;

	Context->PatchIDAttributeName = Settings->PatchIDAttributeName;
	Context->PatchSizeAttributeName = Settings->PatchSizeAttributeName;

	Context->ResolveRoamingMethod = Settings->ResolveRoamingMethod;


	return Context;
}


bool FPCGExFindEdgePatchesElement::Validate(FPCGContext* InContext) const
{
	if (!FPCGExGraphProcessorElement::Validate(InContext)) { return false; }

	const FPCGExFindEdgePatchesContext* Context = static_cast<FPCGExFindEdgePatchesContext*>(InContext);

	if (!FPCGMetadataAttributeBase::IsValidName(Context->PatchIDAttributeName))
	{
		PCGE_LOG(Error, GraphAndLog, LOCTEXT("InvalidName", "Patch ID Attribute name is invalid."));
		return false;
	}

	if (!FPCGMetadataAttributeBase::IsValidName(Context->PatchSizeAttributeName))
	{
		PCGE_LOG(Error, GraphAndLog, LOCTEXT("InvalidName", "Patch size Attribute name is invalid."));
		return false;
	}

	return true;
}

bool FPCGExFindEdgePatchesElement::ExecuteInternal(
	FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExFindEdgePatchesElement::Execute);

	FPCGExFindEdgePatchesContext* Context = static_cast<FPCGExFindEdgePatchesContext*>(InContext);

	if (Context->IsSetup())
	{
		if (!Validate(Context)) { return true; }
		Context->PatchesIO = NewObject<UPCGExPointIOGroup>();
		Context->SetState(PCGExMT::State_ReadyForNextPoints);
	}

	if (Context->IsState(PCGExMT::State_ReadyForNextPoints))
	{
		if (!Context->AdvancePointsIO(true)) { Context->Done(); }
		else
		{
			Context->PreparePatchGroup(); // Prepare patch group for current points
			Context->SetState(PCGExGraph::State_ReadyForNextGraph);
		}
	}

	if (Context->IsState(PCGExGraph::State_ReadyForNextGraph))
	{
		if (!Context->AdvanceGraph())
		{
			// No more graph for the current points, start matching patches.
			Context->SetState(PCGExGraph::State_MergingPatch);
		}
		else
		{
			Context->UpdatePatchGroup();
			Context->SetState(PCGExGraph::State_FindingPatch);
		}
	}

	// -> Process current points with current graph

	if (Context->IsState(PCGExGraph::State_FindingPatch))
	{
		auto Initialize = [&](const UPCGExPointIO* PointIO)
		{
			Context->PrepareCurrentGraphForPoints(PointIO->In, false); // Prepare to read PointIO->In
		};

		auto ProcessPoint = [&](const int32 PointIndex, const UPCGExPointIO* PointIO)
		{
			Context->GetAsyncManager()->StartTask<FDistributeToPatchTask>(PointIndex, PointIO->GetInPoint(PointIndex).MetadataEntry, nullptr);
		};

		if (Context->ProcessCurrentPoints(Initialize, ProcessPoint)) { Context->StartAsyncWait(PCGExGraph::State_WaitingOnFindingPatch); }
	}

	if (Context->IsState(PCGExGraph::State_WaitingOnFindingPatch))
	{
		if (Context->IsAsyncWorkComplete()) { Context->StopAsyncWait(PCGExGraph::State_ReadyForNextGraph); }
	}

	// -> Each graph has been traversed, now merge patches

	if (Context->IsState(PCGExGraph::State_MergingPatch))
	{
		// TODO: Start FConsolidatePatchesTask
		Context->StartAsyncWait(PCGExGraph::State_WaitingOnMergingPatch);
	}

	if (Context->IsState(PCGExGraph::State_WaitingOnMergingPatch))
	{
		if (Context->IsAsyncWorkComplete()) { Context->StopAsyncWait(PCGExGraph::State_WritingPatch); }
	}

	// -> Patches have been merged, now write patches

	if (Context->IsState(PCGExGraph::State_WritingPatch))
	{
		// TODO: Start FWritePatchesTask

		int32 PUID = Context->CurrentIO->GetUniqueID();

		for (UPCGExGraphPatch* Patch : Context->Patches->Patches)
		{
			const int64 OutNumPoints = Patch->IndicesSet.Num();
			if (Context->MinPatchSize >= 0 && OutNumPoints < Context->MinPatchSize) { continue; }
			if (Context->MaxPatchSize >= 0 && OutNumPoints > Context->MaxPatchSize) { continue; }

			// Create and mark patch data
			UPCGPointData* PatchData = PCGExPointIO::NewEmptyOutput(Context, PCGExGraph::OutputPatchesLabel);
			PCGEx::CreateMark(PatchData->Metadata, PCGExGraph::PUIDAttributeName, PUID);

			// Mark point data
			PCGEx::CreateMark(Context->CurrentIO->Out->Metadata, PCGExGraph::PUIDAttributeName, PUID);

			Context->GetAsyncManager()->StartTask<FWritePatchesTask>(Context->PatchUIndex, -1, Context->CurrentIO, Patch, PatchData);

			Context->PatchUIndex++;
		}

		Context->StartAsyncWait(PCGExGraph::State_WaitingOnWritingPatch);
	}

	if (Context->IsState(PCGExGraph::State_WaitingOnWritingPatch))
	{
		if (Context->IsAsyncWorkComplete())
		{
			Context->Patches->Flush();
			Context->StopAsyncWait(PCGExMT::State_ReadyForNextPoints);
		}
	}

	if (Context->IsDone())
	{
		Context->OutputPointsAndGraphParams();
		return true;
	}

	return false;
}

bool FDistributeToPatchTask::ExecuteTask()
{
	const FPCGExFindEdgePatchesContext* Context = Manager->GetContext<FPCGExFindEdgePatchesContext>();
	PCGEX_ASYNC_LIFE_CHECK
	
	Context->Patches->Distribute(TaskInfos.Index);

	return true;
}

bool FConsolidatePatchesTask::ExecuteTask()
{
	if (!CanContinue()) { return false; }

	// TODO : Check if multiple patches overlap, and merge them
	return true;
}

bool FWritePatchesTask::ExecuteTask()
{

	FPCGExFindEdgePatchesContext* Context = Manager->GetContext<FPCGExFindEdgePatchesContext>();
	PCGEX_ASYNC_LIFE_CHECK
	
	const TArray<FPCGPoint>& InPoints = PointIO->In->GetPoints();
	TArray<FPCGPoint>& MutablePoints = PatchData->GetMutablePoints();
	MutablePoints.Reserve(MutablePoints.Num() + Patch->IndicesSet.Num());

	int32 NumPoints = Patch->IndicesSet.Num();

	PCGEx::CreateMark(PointIO->Out->Metadata, Context->Patches->PatchIDAttributeName, TaskInfos.Index);
	PCGEx::CreateMark(PointIO->Out->Metadata, Context->Patches->PatchSizeAttributeName, NumPoints);

	FPCGMetadataAttribute<int32>* StartIndexAttribute = PatchData->Metadata->FindOrCreateAttribute<int32>(FName("StartIndex"), -1);
	FPCGMetadataAttribute<int32>* EndIndexAttribute = PatchData->Metadata->FindOrCreateAttribute<int32>(FName("EndIndex"), -1);

	for (const uint64 Hash : Patch->IndicesSet)
	{
		PCGEX_ASYNC_LIFE_CHECK
		
		FPCGPoint& NewPoint = MutablePoints.Emplace_GetRef();
		PatchData->Metadata->InitializeOnSet(NewPoint.MetadataEntry);

		PCGExGraph::FUnsignedEdge Edge = static_cast<PCGExGraph::FUnsignedEdge>(Hash);
		StartIndexAttribute->SetValue(NewPoint.MetadataEntry, Edge.Start);
		EndIndexAttribute->SetValue(NewPoint.MetadataEntry, Edge.End);
	}


	return true;
}

#undef LOCTEXT_NAMESPACE
