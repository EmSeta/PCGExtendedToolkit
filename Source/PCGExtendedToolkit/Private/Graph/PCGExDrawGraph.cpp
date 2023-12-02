﻿// Copyright Timothé Lapetite 2023
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExDrawGraph.h"
#include "Graph/Solvers/PCGExGraphSolver.h"

#define LOCTEXT_NAMESPACE "PCGExDrawGraph"

PCGExIO::EInitMode UPCGExDrawGraphSettings::GetPointOutputInitMode() const { return PCGExIO::EInitMode::NoOutput; }

FPCGElementPtr UPCGExDrawGraphSettings::CreateElement() const
{
	return MakeShared<FPCGExDrawGraphElement>();
}

UPCGExDrawGraphSettings::UPCGExDrawGraphSettings(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DebugSettings.PointScale = 0.0f;
	GraphSolver = NewObject<UPCGExGraphSolver>();
}

TArray<FPCGPinProperties> UPCGExDrawGraphSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Empty;
	Empty.Empty();
	return Empty;
}

#if WITH_EDITOR
void UPCGExDrawGraphSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	//if (const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World()) { FlushPersistentDebugLines(EditorWorld); }
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

bool FPCGExDrawGraphElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGExDrawGraphElement::Execute);

#if WITH_EDITOR

	FPCGExGraphProcessorContext* Context = static_cast<FPCGExGraphProcessorContext*>(InContext);

	UPCGExDrawGraphSettings* Settings = const_cast<UPCGExDrawGraphSettings*>(Context->GetInputSettings<UPCGExDrawGraphSettings>());
	check(Settings);

	if (Context->IsSetup())
	{
		if (!Settings->bDebug) { return true; }
		if (!Validate(Context)) { return true; }

		if (!PCGExDebug::NotifyExecute(InContext))
		{
			PCGE_LOG(Error, GraphAndLog, LOCTEXT("MissingDebugManager", "Could not find a PCGEx Debug Manager node in your graph."));
			return true;
		}

		Context->SetState(PCGExMT::State_ReadyForNextPoints);
	}

	if (Context->IsState(PCGExMT::State_ReadyForNextPoints))
	{
		if (!Context->AdvancePointsIO(true)) { Context->Done(); }
		else { Context->SetState(PCGExGraph::State_ReadyForNextGraph); }
	}

	if (Context->IsState(PCGExGraph::State_ReadyForNextGraph))
	{
		if (!Context->AdvanceGraph())
		{
			Context->SetState(PCGExMT::State_ReadyForNextPoints);
		}
		else
		{
			Context->SetState(PCGExGraph::State_ProcessingGraph);
		}
	}

	if (Context->IsState(PCGExGraph::State_ProcessingGraph))
	{
		auto Initialize = [&](const UPCGExPointIO* PointIO)
		{
			Context->PrepareCurrentGraphForPoints(PointIO->In, false);
		};

		auto ProcessPoint = [&](const int32 PointIndex, const UPCGExPointIO* PointIO)
		{
			const FPCGPoint& Point = PointIO->GetInPoint(PointIndex);
			const FVector Start = Point.Transform.GetLocation();

			TArray<PCGExGraph::FSocketProbe> Probes;
			if (Settings->bDrawSocketCones) { Settings->GraphSolver->PrepareProbesForPoint(Context->SocketInfos, Point, Probes); }

			for (const PCGExGraph::FSocketInfos& SocketInfos : Context->SocketInfos)
			{
				const PCGExGraph::FSocketMetadata SocketMetadata = SocketInfos.Socket->GetData(Point.MetadataEntry);

				if (Settings->bDrawSocketCones)
				{
					for (PCGExGraph::FSocketProbe Probe : Probes)
					{
						const double AngleWidth = FMath::Acos(FMath::Max(-1.0, FMath::Min(1.0, Probe.DotThreshold)));
						DrawDebugCone(
							Context->World,
							Probe.Origin,
							Probe.Direction,
							FMath::Sqrt(Probe.MaxDistance),
							AngleWidth, AngleWidth, 12,
							Probe.SocketInfos->Socket->Descriptor.DebugColor,
							true, -1, 0, .5f);
					}
				}

				if (Settings->bDrawSocketBox)
				{
					for (PCGExGraph::FSocketProbe Probe : Probes)
					{
						DrawDebugBox(
							Context->World,
							Probe.LooseBounds.GetCenter(),
							Probe.LooseBounds.GetExtent(),
							Probe.SocketInfos->Socket->Descriptor.DebugColor,
							true, -1, 0, .5f);
					}
				}

				if (Settings->bDrawGraph)
				{
					if (SocketMetadata.Index == -1) { continue; }
					if (static_cast<uint8>((SocketMetadata.EdgeType & static_cast<EPCGExEdgeType>(Settings->EdgeType))) == 0) { continue; }

					FPCGPoint PtB = PointIO->GetInPoint(SocketMetadata.Index);
					FVector End = PtB.Transform.GetLocation();
					float Thickness = 1.0f;
					float ArrowSize = 0.0f;
					float Lerp = 1.0f;

					switch (SocketMetadata.EdgeType)
					{
					case EPCGExEdgeType::Unknown:
						Lerp = 0.8f;
						Thickness = 0.5f;
						ArrowSize = 1.0f;
						break;
					case EPCGExEdgeType::Roaming:
						Lerp = 0.8f;
						ArrowSize = 1.0f;
						break;
					case EPCGExEdgeType::Shared:
						Lerp = 0.4f;
						ArrowSize = 2.0f;
						break;
					case EPCGExEdgeType::Match:
						Lerp = 0.5f;
						Thickness = 2.0f;
						break;
					case EPCGExEdgeType::Complete:
						Lerp = 0.5f;
						Thickness = 2.0f;
						break;
					case EPCGExEdgeType::Mirror:
						ArrowSize = 2.0f;
						Lerp = 0.5f;
						break;
					default:
						//Lerp = 0.0f;
						//Alpha = 0.0f;
						;
					}

					if (ArrowSize > 0.0f)
					{
						DrawDebugDirectionalArrow(Context->World, Start, FMath::Lerp(Start, End, Lerp), 3.0f, SocketInfos.Socket->Descriptor.DebugColor, true, -1, 0, Thickness);
					}
					else
					{
						DrawDebugLine(Context->World, Start, FMath::Lerp(Start, End, Lerp), SocketInfos.Socket->Descriptor.DebugColor, true, -1, 0, Thickness);
					}
				}
			}
		};

		if (Context->ProcessCurrentPoints(Initialize, ProcessPoint, true))
		{
			Context->SetState(PCGExGraph::State_ReadyForNextGraph);
		}
	}

	if (Context->IsDone())
	{
		//Context->OutputPointsAndParams();
		return true;
	}

	return false;

#elif
	return  true;
#endif
}

#undef LOCTEXT_NAMESPACE
