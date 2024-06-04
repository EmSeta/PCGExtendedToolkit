﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Sampling/Neighbors/PCGExNeighborSampleProperties.h"

#define LOCTEXT_NAMESPACE "PCGExCreateNeighborSample"
#define PCGEX_NAMESPACE PCGExCreateNeighborSample

void UPCGExNeighborSampleProperties::PrepareForCluster(const FPCGContext* InContext, PCGExCluster::FCluster* InCluster)
{
	PCGEX_DELETE(Blender)
	Blender = new PCGExDataBlending::FPropertiesBlender(BlendingSettings);
	Super::PrepareForCluster(InContext, InCluster);
}

void UPCGExNeighborSampleProperties::PrepareNode(PCGExCluster::FNode& TargetNode) const
{
	FPCGPoint& A = Cluster->PointsIO->GetMutablePoint(TargetNode.PointIndex);
	Blender->PrepareBlending(A, A);
}

void UPCGExNeighborSampleProperties::BlendNodePoint(PCGExCluster::FNode& TargetNode, const PCGExCluster::FNode& OtherNode, const double Weight) const
{
	FPCGPoint& A = Cluster->PointsIO->GetMutablePoint(TargetNode.PointIndex);
	const FPCGPoint& B = Cluster->PointsIO->GetInPoint(OtherNode.PointIndex);
	Blender->Blend(A, B, A, Weight);
}

void UPCGExNeighborSampleProperties::BlendNodeEdge(PCGExCluster::FNode& TargetNode, const int32 InEdgeIndex, const double Weight) const
{
	FPCGPoint& A = Cluster->PointsIO->GetMutablePoint(TargetNode.PointIndex);
	const FPCGPoint& B = Cluster->EdgesIO->GetInPoint(InEdgeIndex);
	Blender->Blend(A, B, A, Weight);
}

void UPCGExNeighborSampleProperties::FinalizeNode(PCGExCluster::FNode& TargetNode, const int32 Count, const double TotalWeight) const
{
	FPCGPoint& A = Cluster->PointsIO->GetMutablePoint(TargetNode.PointIndex);
	Blender->CompleteBlending(A, Count, TotalWeight);
}

void UPCGExNeighborSampleProperties::Cleanup()
{
	PCGEX_DELETE(Blender)
	Super::Cleanup();
}

#if WITH_EDITOR
FString UPCGExNeighborSamplePropertiesSettings::GetDisplayName() const
{
	return TEXT("");
}
#endif

UPCGExNeighborSampleOperation* UPCGNeighborSamplerFactoryProperties::CreateOperation() const
{
	UPCGExNeighborSampleProperties* NewOperation = NewObject<UPCGExNeighborSampleProperties>();

	PCGEX_SAMPLER_CREATE

	NewOperation->BlendingSettings = Descriptor.Blending;

	return NewOperation;
}

UPCGExParamFactoryBase* UPCGExNeighborSamplePropertiesSettings::CreateFactory(FPCGContext* InContext, UPCGExParamFactoryBase* InFactory) const
{
	UPCGNeighborSamplerFactoryProperties* SamplerFactory = NewObject<UPCGNeighborSamplerFactoryProperties>();
	SamplerFactory->Descriptor = Descriptor;

	return Super::CreateFactory(InContext, SamplerFactory);
}


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
