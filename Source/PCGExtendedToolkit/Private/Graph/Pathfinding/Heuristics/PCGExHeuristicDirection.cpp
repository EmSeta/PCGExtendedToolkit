﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/Pathfinding/Heuristics/PCGExHeuristicDirection.h"

void UPCGExHeuristicDirection::PrepareForCluster(PCGExCluster::FCluster* InCluster)
{
	if (bInvert)
	{
		OutMin = 1;
		OutMax = 0;
	}
	else
	{
		OutMin = 0;
		OutMax = 1;
	}
	Super::PrepareForCluster(InCluster);
}

double UPCGExHeuristicDirection::GetGlobalScore(
	const PCGExCluster::FNode& From,
	const PCGExCluster::FNode& Seed,
	const PCGExCluster::FNode& Goal) const
{
	const FVector Dir = (Seed.Position - Goal.Position).GetSafeNormal();
	const double Dot = FVector::DotProduct(Dir, (From.Position - Goal.Position).GetSafeNormal()) * -1;
	return FMath::Max(0, ScoreCurveObj->GetFloatValue(PCGExMath::Remap(Dot, -1, 1, OutMin, OutMax))) * ReferenceWeight;
}

double UPCGExHeuristicDirection::GetEdgeScore(
	const PCGExCluster::FNode& From,
	const PCGExCluster::FNode& To,
	const PCGExGraph::FIndexedEdge& Edge,
	const PCGExCluster::FNode& Seed,
	const PCGExCluster::FNode& Goal) const
{
	const double Dot = (FVector::DotProduct((From.Position - To.Position).GetSafeNormal(), (From.Position - Goal.Position).GetSafeNormal()) * -1);
	return FMath::Max(0, ScoreCurveObj->GetFloatValue(PCGExMath::Remap(Dot, -1, 1, OutMin, OutMax))) * ReferenceWeight;
}

void UPCGExHeuristicDirection::ApplyOverrides()
{
	Super::ApplyOverrides();
	PCGEX_OVERRIDE_OP_PROPERTY(bInvert, FName(TEXT("Heuristics/Invert")), EPCGMetadataTypes::Boolean);
}

UPCGExHeuristicOperation* UPCGHeuristicsFactoryDirection::CreateOperation() const
{
	UPCGExHeuristicDirection* NewOperation = NewObject<UPCGExHeuristicDirection>();
	NewOperation->bInvert = Descriptor.bInvert;
	return NewOperation;
}

UPCGExParamFactoryBase* UPCGExHeuristicsDirectionProviderSettings::CreateFactory(FPCGContext* InContext, UPCGExParamFactoryBase* InFactory) const
{
	UPCGHeuristicsFactoryDirection* NewHeuristics = NewObject<UPCGHeuristicsFactoryDirection>();
	NewHeuristics->WeightFactor = Descriptor.WeightFactor;
	return Super::CreateFactory(InContext, NewHeuristics);
}
