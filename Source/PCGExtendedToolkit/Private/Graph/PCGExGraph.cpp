﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExGraph.h"

#include "PCGExPointsProcessor.h"
#include "Graph/PCGExCluster.h"

namespace PCGExGraph
{
	void FNode::Add(const int32 EdgeIndex) { Edges.AddUnique(EdgeIndex); }

	void FSubGraph::Add(const FIndexedEdge& Edge, FGraph* InGraph)
	{
		Nodes.Add(Edge.Start);
		Nodes.Add(Edge.End);

		Edges.Add(Edge.EdgeIndex);
		if (Edge.IOIndex != -1) { EdgesInIOIndices.Add(Edge.IOIndex); }
	}

	void FSubGraph::Invalidate(FGraph* InGraph)
	{
		for (const int32 EdgeIndex : Edges) { InGraph->Edges[EdgeIndex].bValid = false; }
		for (const int32 NodeIndex : Nodes) { InGraph->Nodes[NodeIndex].bValid = false; }
	}

	int32 FSubGraph::GetFirstInIOIndex()
	{
		for (const int32 InIOIndex : EdgesInIOIndices) { return InIOIndex; }
		return -1;
	}

	bool FGraph::InsertEdge(const int32 A, const int32 B, FIndexedEdge& OutEdge)
	{
		const uint64 Hash = PCGEx::H64U(A, B);

		{
			FReadScopeLock ReadLock(GraphLock);
			if (UniqueEdges.Contains(Hash)) { return false; }
		}

		FWriteScopeLock WriteLock(GraphLock);

		UniqueEdges.Add(Hash);

		OutEdge = Edges.Emplace_GetRef(Edges.Num(), A, B);

		Nodes[A].Add(OutEdge.EdgeIndex);
		Nodes[B].Add(OutEdge.EdgeIndex);

		return true;
	}

	bool FGraph::InsertEdge(const FIndexedEdge& Edge)
	{
		const uint64 Hash = Edge.H64U();

		{
			FReadScopeLock ReadLock(GraphLock);
			if (UniqueEdges.Contains(Hash)) { return false; }
		}

		FWriteScopeLock WriteLock(GraphLock);

		UniqueEdges.Add(Hash);

		FIndexedEdge& NewEdge = Edges.Emplace_GetRef(Edge);
		NewEdge.EdgeIndex = Edges.Num() - 1;

		Nodes[Edge.Start].Add(NewEdge.EdgeIndex);
		Nodes[Edge.End].Add(NewEdge.EdgeIndex);

		return true;
	}

	void FGraph::InsertEdges(const TArray<uint64>& InEdges, int32 IOIndex)
	{
		FWriteScopeLock WriteLock(GraphLock);
		uint32 A;
		uint32 B;
		for (const uint64& E : InEdges)
		{
			if (UniqueEdges.Contains(E)) { continue; }
			
			UniqueEdges.Add(E);
			PCGEx::H64(E, A, B);
			const int32 EdgeIndex = Edges.Emplace(Edges.Num(), A, B);
			Nodes[A].Add(EdgeIndex);
			Nodes[B].Add(EdgeIndex);
			Edges[EdgeIndex].IOIndex = IOIndex;
		} 
	}
	
	void FGraph::InsertEdges(const TSet<uint64>& InEdges, int32 IOIndex)
    	{
    		FWriteScopeLock WriteLock(GraphLock);
    		uint32 A;
    		uint32 B;
    		for (const uint64& E : InEdges)
    		{
    			if (UniqueEdges.Contains(E)) { continue; }
    			
    			UniqueEdges.Add(E);
    			PCGEx::H64(E, A, B);
    			const int32 EdgeIndex = Edges.Emplace(Edges.Num(), A, B);
    			Nodes[A].Add(EdgeIndex);
    			Nodes[B].Add(EdgeIndex);
    		} 
    	}

#define PCGEX_EDGE_INSERT\
	if (!E.bValid) { continue; } const uint64 Hash = E.H64U(); if (UniqueEdges.Contains(Hash)) { continue; }\
	UniqueEdges.Add(Hash); const FIndexedEdge& Edge = Edges.Emplace_GetRef(Edges.Num(), E.Start, E.End);\
	Nodes[E.Start].Add(Edge.EdgeIndex);	Nodes[E.End].Add(Edge.EdgeIndex);

	void FGraph::InsertEdges(const TArray<FUnsignedEdge>& InEdges, int32 IOIndex)
	{
		FWriteScopeLock WriteLock(GraphLock);
		for (const FUnsignedEdge& E : InEdges) { PCGEX_EDGE_INSERT }
	}

	void FGraph::InsertEdges(const TArray<FIndexedEdge>& InEdges)
	{
		FWriteScopeLock WriteLock(GraphLock);
		for (const FIndexedEdge& E : InEdges) { PCGEX_EDGE_INSERT }
	}

#undef PCGEX_EDGE_INSERT

	TArrayView<FNode> FGraph::AddNodes(const int32 NumNewNodes)
	{
		const int32 StartIndex = Nodes.Num();
		Nodes.SetNum(StartIndex + NumNewNodes);
		for (int i = 0; i < NumNewNodes; i++)
		{
			FNode& Node = Nodes[StartIndex + i];
			Node.NodeIndex = Node.PointIndex = StartIndex + i;
			Node.Edges.Reserve(NumEdgesReserve);
		}

		return MakeArrayView(Nodes.GetData() + StartIndex, NumNewNodes);
	}


	void FGraph::BuildSubGraphs(const int32 Min, const int32 Max)
	{
		TSet<int32> VisitedNodes;
		VisitedNodes.Reserve(Nodes.Num());

		for (int i = 0; i < Nodes.Num(); i++)
		{
			if (VisitedNodes.Contains(i)) { continue; }

			const FNode& CurrentNode = Nodes[i];
			if (!CurrentNode.bValid || // Points are valid by default, but may be invalidated prior to building the subgraph
				CurrentNode.Edges.IsEmpty())
			{
				VisitedNodes.Add(i);
				continue;
			}

			FSubGraph* SubGraph = new FSubGraph();

			TQueue<int32> Queue;
			Queue.Enqueue(i);

			int32 NextIndex = -1;
			while (Queue.Dequeue(NextIndex))
			{
				if (VisitedNodes.Contains(NextIndex)) { continue; }
				VisitedNodes.Add(NextIndex);

				FNode& Node = Nodes[NextIndex];
				Node.NumExportedEdges = 0;

				for (int32 E : Node.Edges)
				{
					const FIndexedEdge& Edge = Edges[E];
					if (!Edge.bValid) { continue; }

					int32 OtherIndex = Edge.Other(NextIndex);
					if (!Nodes[OtherIndex].bValid) { continue; }

					Node.NumExportedEdges++;
					SubGraph->Add(Edge, this);
					if (!VisitedNodes.Contains(OtherIndex)) { Queue.Enqueue(OtherIndex); }
				}
			}

			if (!FMath::IsWithin(SubGraph->Edges.Num(), Min, Max))
			{
				SubGraph->Invalidate(this); // Will invalidate isolated points
				delete SubGraph;
			}
			else
			{
				SubGraphs.Add(SubGraph);
			}
		}
	}

	void FGraphBuilder::Compile(FPCGExPointsProcessorContext* InContext,
	                            FGraphMetadataSettings* MetadataSettings) const
	{
		InContext->GetAsyncManager()->Start<FPCGExCompileGraphTask>(
			-1, PointIO, const_cast<FGraphBuilder*>(this),
			OutputSettings->GetMinClusterSize(), OutputSettings->GetMaxClusterSize(), MetadataSettings);
	}

	void FGraphBuilder::Write(FPCGExPointsProcessorContext* InContext) const
	{
		EdgesIO->OutputTo(InContext);
	}


	bool FLooseNode::Add(FLooseNode* OtherNode)
	{
		if (OtherNode->Index == Index) { return false; }
		if (Neighbors.Contains(OtherNode->Index)) { return true; }

		Neighbors.Add(OtherNode->Index);
		OtherNode->Add(this);
		return true;
	}

	void FLooseNode::AddPointH(const uint64 PointH)
	{
		FusedPoints.AddUnique(PointH);
	}

	void FLooseNode::AddEdgeH(const uint64 EdgeH)
	{
		FusedEdges.AddUnique(EdgeH);
	}

	FVector FLooseNode::UpdateCenter(PCGExData::FPointIOGroup* IOGroup)
	{
		Center = FVector::ZeroVector;
		double Divider = 0;

		uint32 IOIndex = 0;
		uint32 PointIndex = 0;

		for (const uint64 FuseHash : FusedPoints)
		{
			Divider++;
			PCGEx::H64(FuseHash, IOIndex, PointIndex);
			Center += IOGroup->Pairs[IOIndex]->GetInPoint(PointIndex).Transform.GetLocation();
		}

		Center /= Divider;
		return Center;
	}

	FLooseNode* FLooseGraph::GetOrCreateNode(const FPCGPoint& Point, const int32 IOIndex, const int32 PointIndex)
	{
		const FVector Origin = Point.Transform.GetLocation();

		if (FuseSettings.bComponentWiseTolerance)
		{
			for (FLooseNode* Node : Nodes)
			{
				if (FuseSettings.IsWithinToleranceComponentWise(Point, Node->Point)) { return Node; }
			}
		}
		else
		{
			for (FLooseNode* Node : Nodes)
			{
				if (FuseSettings.IsWithinTolerance(Point, Node->Point)) { return Node; }
			}
		}

		FLooseNode* NewNode = new FLooseNode(Point, Origin, Nodes.Num());
		NewNode->AddPointH(PCGEx::H64(IOIndex, PointIndex));
		Nodes.Add_GetRef(NewNode);
		return NewNode;
	}

	void FLooseGraph::CreateBridge(const FPCGPoint& From, const int32 FromIOIndex, const int32 FromPointIndex, const FPCGPoint& To, const int32 ToIOIndex, const int32 ToPointIndex)
	{
		FLooseNode* StartVtx = GetOrCreateNode(From, FromIOIndex, FromPointIndex);
		FLooseNode* EndVtx = GetOrCreateNode(To, ToIOIndex, ToPointIndex);
		StartVtx->Add(EndVtx);
		EndVtx->Add(StartVtx);
	}

	void FLooseGraph::GetUniqueEdges(TArray<FUnsignedEdge>& OutEdges)
	{
		OutEdges.Empty(Nodes.Num() * 4);
		TSet<uint64> UniqueEdges;
		for (const FLooseNode* Node : Nodes)
		{
			for (const int32 OtherNodeIndex : Node->Neighbors)
			{
				const uint64 Hash = PCGEx::H64U(Node->Index, OtherNodeIndex);
				if (UniqueEdges.Contains(Hash)) { continue; }
				UniqueEdges.Add(Hash);
				OutEdges.Emplace(Node->Index, OtherNodeIndex);
			}
		}
		UniqueEdges.Empty();
	}

	void FLooseGraph::WriteMetadata(TMap<int32, FGraphNodeMetadata*>& OutMetadata)
	{
		for (const FLooseNode* Node : Nodes)
		{
			FGraphNodeMetadata* NodeMeta = FGraphNodeMetadata::GetOrCreate(Node->Index, OutMetadata);
			NodeMeta->CompoundSize = Node->Neighbors.Num();
			NodeMeta->bCompounded = NodeMeta->CompoundSize > 1;
		}
	}

	bool FPointEdgeProxy::FindSplit(const FVector& Position, FPESplit& OutSplit) const
	{
		const FVector ClosestPoint = FMath::ClosestPointOnSegment(Position, Start, End);

		if ((ClosestPoint - Start).IsNearlyZero() || (ClosestPoint - End).IsNearlyZero()) { return false; } // Overlap endpoint
		if (FVector::DistSquared(ClosestPoint, Position) >= ToleranceSquared) { return false; }             // Too far

		OutSplit.ClosestPoint = ClosestPoint;
		OutSplit.Time = (FVector::DistSquared(Start, ClosestPoint) / LengthSquared);
		return true;
	}

	FPointEdgeIntersections::FPointEdgeIntersections(
		FGraph* InGraph,
		PCGExData::FPointIO* InPointIO,
		const FPCGExPointEdgeIntersectionSettings& InSettings)
		: PointIO(InPointIO), Graph(InGraph), Settings(InSettings)
	{
		const TArray<FPCGPoint>& Points = InPointIO->GetOutIn()->GetPoints();

		const int32 NumEdges = InGraph->Edges.Num();
		Edges.SetNum(NumEdges);

		for (const FIndexedEdge& Edge : InGraph->Edges)
		{
			if (!Edge.bValid) { continue; }
			Edges[Edge.EdgeIndex].Init(
				Edge.EdgeIndex,
				Points[Edge.Start].Transform.GetLocation(),
				Points[Edge.End].Transform.GetLocation(),
				Settings.FuseSettings.Tolerance);
		}
	}

	void FPointEdgeIntersections::FindIntersections(FPCGExPointsProcessorContext* InContext)
	{
		for (const FIndexedEdge& Edge : Graph->Edges)
		{
			if (!Edge.bValid) { continue; }
			InContext->GetAsyncManager()->Start<FPCGExFindPointEdgeIntersectionsTask>(Edge.EdgeIndex, PointIO, this);
		}
	}

	void FPointEdgeIntersections::Add(const int32 EdgeIndex, const FPESplit& Split)
	{
		FWriteScopeLock WriteLock(InsertionLock);
		Edges[EdgeIndex].CollinearPoints.AddUnique(Split);
	}

	void FPointEdgeIntersections::Insert()
	{
		FIndexedEdge NewEdge = FIndexedEdge{};

		for (FPointEdgeProxy& PointEdgeProxy : Edges)
		{
			if (PointEdgeProxy.CollinearPoints.IsEmpty()) { continue; }

			FIndexedEdge& SplitEdge = Graph->Edges[PointEdgeProxy.EdgeIndex];
			SplitEdge.bValid = false; // Invalidate existing edge
			PointEdgeProxy.CollinearPoints.Sort([](const FPESplit& A, const FPESplit& B) { return A.Time < B.Time; });

			const int32 FirstIndex = SplitEdge.Start;
			const int32 LastIndex = SplitEdge.End;

			int32 NodeIndex = -1;

			int32 PrevIndex = FirstIndex;
			for (const FPESplit Split : PointEdgeProxy.CollinearPoints)
			{
				NodeIndex = Split.NodeIndex;

				FGraphNodeMetadata* NodeMetadata = FGraphNodeMetadata::GetOrCreate(NodeIndex, Graph->NodeMetadata);
				NodeMetadata->bIntersector = true;

				Graph->InsertEdge(PrevIndex, NodeIndex, NewEdge);
				PrevIndex = NodeIndex;

				if (Settings.bSnapOnEdge)
				{
					PointIO->GetMutablePoint(Graph->Nodes[Split.NodeIndex].PointIndex).Transform.SetLocation(Split.ClosestPoint);
				}
			}

			Graph->InsertEdge(NodeIndex, LastIndex, NewEdge); // Insert last edge
		}
	}

	void PCGExGraph::FindCollinearNodes(
		FPointEdgeIntersections* InIntersections,
		const int32 EdgeIndex,
		const TArray<FPCGPoint>& Points)
	{
		const FPointEdgeProxy& Edge = InIntersections->Edges[EdgeIndex];
		const FIndexedEdge& IEdge = InIntersections->Graph->Edges[EdgeIndex];
		FPESplit Split = FPESplit{};

		for (const FNode& Node : InIntersections->Graph->Nodes)
		{
			if (!Node.bValid) { continue; }

			FVector Position = Points[Node.PointIndex].Transform.GetLocation();

			if (!Edge.Box.IsInside(Position)) { continue; }
			if (IEdge.Start == Node.PointIndex || IEdge.End == Node.PointIndex) { continue; }
			if (Edge.FindSplit(Position, Split))
			{
				Split.NodeIndex = Node.NodeIndex;
				InIntersections->Add(EdgeIndex, Split);
			}
		}
	}

	bool FEdgeEdgeProxy::FindSplit(const FEdgeEdgeProxy& OtherEdge, FEESplit& OutSplit) const
	{
		if (!Box.Intersect(OtherEdge.Box) || Start == OtherEdge.Start || Start == OtherEdge.End ||
			End == OtherEdge.End || End == OtherEdge.Start) { return false; }

		// TODO: Check directions/dot

		FVector A;
		FVector B;
		FMath::SegmentDistToSegment(
			Start, End,
			OtherEdge.Start, OtherEdge.End,
			A, B);

		if (FVector::DistSquared(A, B) >= ToleranceSquared) { return false; }

		OutSplit.Center = FMath::Lerp(A, B, 0.5);
		OutSplit.TimeA = FVector::DistSquared(Start, A) / LengthSquared;
		OutSplit.TimeB = FVector::DistSquared(OtherEdge.Start, B) / OtherEdge.LengthSquared;

		return true;
	}

	FEdgeEdgeIntersections::FEdgeEdgeIntersections(
		FGraph* InGraph,
		PCGExData::FPointIO* InPointIO,
		const FPCGExEdgeEdgeIntersectionSettings& InSettings)
		: PointIO(InPointIO), Graph(InGraph), Settings(InSettings)
	{
		const TArray<FPCGPoint>& Points = InPointIO->GetOutIn()->GetPoints();

		const int32 NumEdges = InGraph->Edges.Num();
		Edges.SetNum(NumEdges);

		for (const FIndexedEdge& Edge : InGraph->Edges)
		{
			if (!Edge.bValid) { continue; }
			Edges[Edge.EdgeIndex].Init(
				Edge.EdgeIndex,
				Points[Edge.Start].Transform.GetLocation(),
				Points[Edge.End].Transform.GetLocation(),
				Settings.Tolerance);
		}
	}

	void FEdgeEdgeIntersections::FindIntersections(FPCGExPointsProcessorContext* InContext)
	{
		for (const FIndexedEdge& Edge : Graph->Edges)
		{
			if (!Edge.bValid) { continue; }
			InContext->GetAsyncManager()->Start<FPCGExFindEdgeEdgeIntersectionsTask>(Edge.EdgeIndex, PointIO, this);
		}
	}

	void FEdgeEdgeIntersections::Add(const int32 EdgeIndex, const int32 OtherEdgeIndex, const FEESplit& Split)
	{
		FWriteScopeLock WriteLock(InsertionLock);

		CheckedPairs.Add(PCGEx::H64U(EdgeIndex, OtherEdgeIndex));

		FEECrossing* OutSplit = new FEECrossing(Split);

		OutSplit->NodeIndex = Crossings.Add(OutSplit) + Graph->Nodes.Num();
		OutSplit->EdgeA = FMath::Min(EdgeIndex, OtherEdgeIndex);
		OutSplit->EdgeB = FMath::Max(EdgeIndex, OtherEdgeIndex);

		Edges[EdgeIndex].Intersections.AddUnique(OutSplit);
		Edges[OtherEdgeIndex].Intersections.AddUnique(OutSplit);
	}

	void FEdgeEdgeIntersections::Insert()
	{
		FIndexedEdge NewEdge = FIndexedEdge{};

		// Insert new nodes
		const TArrayView<FNode> NewNodes = Graph->AddNodes(Crossings.Num());

		TArray<FPCGPoint>& MutablePoints = PointIO->GetOut()->GetMutablePoints();
		MutablePoints.SetNum(Graph->Nodes.Num());
		for (int i = 0; i < NewNodes.Num(); i++)
		{
			MutablePoints[NewNodes[i].NodeIndex].Transform.SetLocation(Crossings[i]->Split.Center);
		}

		for (FEdgeEdgeProxy& EdgeProxy : Edges)
		{
			if (EdgeProxy.Intersections.IsEmpty()) { continue; }

			FIndexedEdge& SplitEdge = Graph->Edges[EdgeProxy.EdgeIndex];
			SplitEdge.bValid = false; // Invalidate existing edge

			EdgeProxy.Intersections.Sort(
				[&](const FEECrossing& A, const FEECrossing& B)
				{
					return A.GetTime(EdgeProxy.EdgeIndex) > B.GetTime(EdgeProxy.EdgeIndex);
				});

			const int32 FirstIndex = SplitEdge.Start;
			const int32 LastIndex = SplitEdge.End;

			int32 NodeIndex = -1;

			int32 PrevIndex = FirstIndex;
			for (const FEECrossing* Crossing : EdgeProxy.Intersections)
			{
				NodeIndex = Crossing->NodeIndex;

				FGraphNodeMetadata* NodeMetadata = FGraphNodeMetadata::GetOrCreate(NodeIndex, Graph->NodeMetadata);
				NodeMetadata->bCrossing = true;

				Graph->InsertEdge(PrevIndex, NodeIndex, NewEdge);
				PrevIndex = NodeIndex;
			}

			Graph->InsertEdge(NodeIndex, LastIndex, NewEdge); // Insert last edge
		}
	}

	void FindOverlappingEdges(
		FEdgeEdgeIntersections* InIntersections,
		const int32 EdgeIndex)
	{
		const FEdgeEdgeProxy& Edge = InIntersections->Edges[EdgeIndex];
		FEESplit Split = FEESplit{};

		for (const FEdgeEdgeProxy& OtherEdge : InIntersections->Edges)
		{
			if (OtherEdge.EdgeIndex == -1 || &Edge == &OtherEdge) { continue; }
			if (!Edge.Box.Intersect(OtherEdge.Box)) { continue; }

			{
				FReadScopeLock ReadLock(InIntersections->InsertionLock);
				if (InIntersections->CheckedPairs.Contains(PCGEx::H64U(EdgeIndex, OtherEdge.EdgeIndex))) { continue; }
			}

			if (Edge.FindSplit(OtherEdge, Split))
			{
				InIntersections->Add(EdgeIndex, OtherEdge.EdgeIndex, Split);
			}
		}
	}
}

bool FPCGExFindPointEdgeIntersectionsTask::ExecuteTask()
{
	FindCollinearNodes(IntersectionList, TaskIndex, PointIO->GetOutIn()->GetPoints());
	return true;
}

bool FPCGExInsertPointEdgeIntersectionsTask::ExecuteTask()
{
	IntersectionList->Insert();
	return true;
}

bool FPCGExFindEdgeEdgeIntersectionsTask::ExecuteTask()
{
	FindOverlappingEdges(IntersectionList, TaskIndex);
	return true;
}

bool FPCGExInsertEdgeEdgeIntersectionsTask::ExecuteTask()
{
	IntersectionList->Insert();
	return true;
}

bool FPCGExWriteSubGraphEdgesTask::ExecuteTask()
{
	PCGExData::FPointIO& EdgeIO = *SubGraph->PointIO;

	TArray<FPCGPoint>& MutablePoints = EdgeIO.GetOut()->GetMutablePoints();
	MutablePoints.SetNum(SubGraph->Edges.Num());

	int32 PointIndex = 0;
	if (EdgeIO.GetIn())
	{
		// Copy any existing point properties first
		const TArray<FPCGPoint>& InPoints = EdgeIO.GetIn()->GetPoints();
		for (const int32 EdgeIndex : SubGraph->Edges)
		{
			const int32 EdgePtIndex = Graph->Edges[EdgeIndex].PointIndex;
			if (InPoints.IsValidIndex(EdgePtIndex)) { MutablePoints[PointIndex] = InPoints[EdgePtIndex]; }
			PointIndex++;
		}
	}

	EdgeIO.CreateOutKeys();

	PCGEx::TFAttributeWriter<int32>* EdgeStart = new PCGEx::TFAttributeWriter<int32>(PCGExGraph::Tag_EdgeStart, -1, false);
	PCGEx::TFAttributeWriter<int32>* EdgeEnd = new PCGEx::TFAttributeWriter<int32>(PCGExGraph::Tag_EdgeEnd, -1, false);

	EdgeStart->BindAndGet(EdgeIO);
	EdgeEnd->BindAndGet(EdgeIO);


	const TArray<FPCGPoint> Vertices = PointIO->GetOut()->GetPoints();

	PointIndex = 0;
	for (const int32 EdgeIndex : SubGraph->Edges)
	{
		const PCGExGraph::FIndexedEdge& Edge = Graph->Edges[EdgeIndex];
		FPCGPoint& Point = MutablePoints[PointIndex];

		EdgeStart->Values[PointIndex] = Graph->Nodes[Edge.Start].PointIndex;
		EdgeEnd->Values[PointIndex] = Graph->Nodes[Edge.End].PointIndex;

		if (Point.Seed == 0) { PCGExMath::RandomizeSeed(Point); }
		PointIndex++;
	}

	if (Graph->bWriteEdgePosition)
	{
		for (int i = 0; i < SubGraph->Edges.Num(); i++)
		{
			MutablePoints[i].Transform.SetLocation(
				FMath::Lerp(
					Vertices[EdgeStart->Values[i]].Transform.GetLocation(),
					Vertices[EdgeEnd->Values[i]].Transform.GetLocation(),
					Graph->EdgePosition));
		}
	}

	if (Graph->bRefreshEdgeSeed)
	{
		const FVector SeedOffset = FVector(EdgeIO.IOIndex);
		for (FPCGPoint& Point : MutablePoints) { PCGExMath::RandomizeSeed(Point, SeedOffset); }
	}

	EdgeStart->Write();
	EdgeEnd->Write();

	PCGEX_DELETE(EdgeStart)
	PCGEX_DELETE(EdgeEnd)

	return true;
}

bool FPCGExCompileGraphTask::ExecuteTask()
{
	Builder->Graph->BuildSubGraphs(Min, Max);

	if (Builder->Graph->SubGraphs.IsEmpty())
	{
		Builder->bCompiledSuccessfully = false;
		return false;
	}

	PointIO->Cleanup(); //Ensure fresh keys later on

	TArray<PCGExGraph::FNode>& Nodes = Builder->Graph->Nodes;
	TArray<int32> ValidNodes;
	ValidNodes.Reserve(Builder->Graph->Nodes.Num());

	if (Builder->bPrunePoints)
	{
		// Rebuild point list with only the one used
		// to know which are used, we need to prune subgraphs first
		TArray<FPCGPoint>& MutablePoints = PointIO->GetOut()->GetMutablePoints();

		if (!MutablePoints.IsEmpty())
		{
			//Assume points were filled before, and remove them from the current array
			TArray<FPCGPoint> PrunedPoints;
			PrunedPoints.Reserve(MutablePoints.Num());

			for (PCGExGraph::FNode& Node : Nodes)
			{
				if (!Node.bValid) { continue; }
				Node.PointIndex = PrunedPoints.Add(MutablePoints[Node.PointIndex]);
				ValidNodes.Add(Node.NodeIndex);
			}

			PointIO->GetOut()->SetPoints(PrunedPoints);
		}
		else
		{
			const int32 NumMaxNodes = Nodes.Num();
			MutablePoints.Reserve(NumMaxNodes);

			for (PCGExGraph::FNode& Node : Nodes)
			{
				if (!Node.bValid) { continue; }
				Node.PointIndex = MutablePoints.Add(PointIO->GetInPoint(Node.PointIndex));
				ValidNodes.Add(Node.NodeIndex);
			}
		}
	}
	else
	{
		for (const PCGExGraph::FNode& Node : Nodes) { if (Node.bValid) { ValidNodes.Add(Node.NodeIndex); } }
	}

	///

	PCGEx::TFAttributeWriter<int32>* IndexWriter = new PCGEx::TFAttributeWriter<int32>(PCGExGraph::Tag_EdgeIndex, -1, false);
	PCGEx::TFAttributeWriter<int32>* NumEdgesWriter = new PCGEx::TFAttributeWriter<int32>(PCGExGraph::Tag_EdgesNum, 0, false);

	IndexWriter->BindAndGet(*PointIO);
	NumEdgesWriter->BindAndGet(*PointIO);

	for (int i = 0; i < IndexWriter->Values.Num(); i++) { IndexWriter->Values[i] = i; }
	for (const int32 NodeIndex : ValidNodes)
	{
		const PCGExGraph::FNode& Node = Nodes[NodeIndex];
		NumEdgesWriter->Values[Node.PointIndex] = Node.NumExportedEdges;
	}

	IndexWriter->Write();
	NumEdgesWriter->Write();

	PCGEX_DELETE(IndexWriter)
	PCGEX_DELETE(NumEdgesWriter)

	if (MetadataSettings && !Builder->Graph->NodeMetadata.IsEmpty())
	{
#define PCGEX_METADATA(_NAME, _TYPE, _DEFAULT, _ACCESSOR)\
{if(MetadataSettings->bWrite##_NAME){\
PCGEx::TFAttributeWriter<_TYPE>* Writer = MetadataSettings->bWrite##_NAME ? new PCGEx::TFAttributeWriter<_TYPE>(MetadataSettings->_NAME##AttributeName, _DEFAULT, false) : nullptr;\
Writer->BindAndGet(*PointIO);\
		for(const int32 NodeIndex : ValidNodes){\
		PCGExGraph::FGraphNodeMetadata** NodeMeta = Builder->Graph->NodeMetadata.Find(NodeIndex);\
		if(NodeMeta){Writer->Values[Nodes[NodeIndex].PointIndex] = (*NodeMeta)->_ACCESSOR; }}\
		Writer->Write(); delete Writer; }}

		PCGEX_METADATA(Compounded, bool, false, bCompounded)
		PCGEX_METADATA(CompoundSize, int32, 0, CompoundSize)
		PCGEX_METADATA(Intersector, bool, false, bIntersector)
		PCGEX_METADATA(Crossing, bool, false, bCrossing)

#undef PCGEX_METADATA
	}

	Builder->bCompiledSuccessfully = true;

	int32 SubGraphIndex = 0;
	for (PCGExGraph::FSubGraph* SubGraph : Builder->Graph->SubGraphs)
	{
		PCGExData::FPointIO* EdgeIO;

		if (const int32 IOIndex = SubGraph->GetFirstInIOIndex();
			Builder->SourceEdgesIO && Builder->SourceEdgesIO->Pairs.IsValidIndex(IOIndex))
		{
			EdgeIO = &Builder->EdgesIO->Emplace_GetRef(*Builder->SourceEdgesIO->Pairs[IOIndex], PCGExData::EInit::NewOutput);
		}
		else
		{
			EdgeIO = &Builder->EdgesIO->Emplace_GetRef(PCGExData::EInit::NewOutput);
		}

		SubGraph->PointIO = EdgeIO;
		EdgeIO->Tags->Set(PCGExGraph::Tag_Cluster, Builder->EdgeTagValue);

		Manager->Start<FPCGExWriteSubGraphEdgesTask>(SubGraphIndex++, PointIO, Builder->Graph, SubGraph);
	}

	return true;
}

bool FPCGExInsertLooseNodesTask::ExecuteTask()
{
	TArray<PCGExGraph::FIndexedEdge> IndexedEdges;
	if (!BuildIndexedEdges(*EdgeIO, *NodeIndicesMap, IndexedEdges, true) ||
		IndexedEdges.IsEmpty()) { return false; }
	/*
		IndexedEdges.Sort([&](const PCGExGraph::FIndexedEdge& A, const PCGExGraph::FIndexedEdge& B)
		{
			return A.Start == B.Start ? A.End < B.End : A.Start < B.Start; 
		});	
	*/
	const TArray<FPCGPoint>& InPoints = PointIO->GetIn()->GetPoints();
	for (const PCGExGraph::FIndexedEdge& Edge : IndexedEdges)
	{
		Graph->CreateBridge(
			InPoints[Edge.Start], TaskIndex, Edge.Start,
			InPoints[Edge.End], TaskIndex, Edge.End);
	}

	return false;
}
