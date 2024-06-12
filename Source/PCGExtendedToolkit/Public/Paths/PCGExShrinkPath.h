﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once

#include "CoreMinimal.h"
#include "PCGExPathProcessor.h"

#include "PCGExPointsProcessor.h"
#include "Geometry/PCGExGeo.h"
#include "PCGExShrinkPath.generated.h"

UENUM(BlueprintType, meta=(DisplayName="[PCGEx] Path Shrink Mode"))
enum class EPCGExPathShrinkMode : uint8
{
	Count UMETA(DisplayName = "Count", ToolTip="TBD"),
	Distance UMETA(DisplayName = "Distance", ToolTip="TBD."),
};

UENUM(BlueprintType, meta=(DisplayName="[PCGEx] Path Shrink Distance Cut Type"))
enum class EPCGExPathShrinkDistanceCutType : uint8
{
	NewPoint UMETA(DisplayName = "New Point", ToolTip="TBD"),
	Previous UMETA(DisplayName = "Previous (Floor)", ToolTip="TBD."),
	Next UMETA(DisplayName = "Next (Ceil)", ToolTip="TBD."),
	Closest UMETA(DisplayName = "Closest (Round)", ToolTip="TBD."),
};

/**
 * Calculates the distance between two points (inherently a n*n operation)
 */
UCLASS(BlueprintType, ClassGroup = (Procedural), Category="PCGEx|Path")
class PCGEXTENDEDTOOLKIT_API UPCGExShrinkPathSettings : public UPCGExPathProcessorSettings
{
	GENERATED_BODY()

public:
	//~Begin UPCGSettings interface
#if WITH_EDITOR
	PCGEX_NODE_INFOS(ShrinkPath, "Path : Shrink", "Shrink path from its beginning and end.");
#endif

protected:
	virtual FPCGElementPtr CreateElement() const override;
	//~End UPCGSettings interface

	//~Begin UPCGExPointsProcessorSettings interface
public:
	virtual PCGExData::EInit GetMainOutputInitMode() const override;
	//~End UPCGExPointsProcessorSettings interface

public:
	/** Consider paths to be closed -- processing will wrap between first and last points. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	bool bClosedPath = false;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExPathShrinkMode ShrinkMode = EPCGExPathShrinkMode::Distance;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExFetchType ValueSource = EPCGExFetchType::Constant;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="ShrinkMode==EPCGExPathShrinkMode::Count && ValueSource==EPCGExFetchType::Constant", ClampMin=1))
	double CountConstant = 1;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable, EditCondition="ShrinkMode==EPCGExPathShrinkMode::Distance && ValueSource==EPCGExFetchType::Attribute", ClampMin=0.001))
	double DistanceConstant = 10;

	/** TBD */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta = (PCG_Overridable))
	EPCGExPathShrinkDistanceCutType CutType = EPCGExPathShrinkDistanceCutType::NewPoint;
};

struct PCGEXTENDEDTOOLKIT_API FPCGExShrinkPathContext : public FPCGExPathProcessorContext
{
	friend class FPCGExShrinkPathElement;

	virtual ~FPCGExShrinkPathContext() override;
};

class PCGEXTENDEDTOOLKIT_API FPCGExShrinkPathElement : public FPCGExPathProcessorElement
{
public:
	virtual FPCGContext* Initialize(
		const FPCGDataCollection& InputData,
		TWeakObjectPtr<UPCGComponent> SourceComponent,
		const UPCGNode* Node) override;

protected:
	virtual bool Boot(FPCGContext* InContext) const override;
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};

class PCGEXTENDEDTOOLKIT_API FPCGExShrinkPathTask : public FPCGExNonAbandonableTask
{
public:
	FPCGExShrinkPathTask(FPCGExAsyncManager* InManager, const int32 InTaskIndex, PCGExData::FPointIO* InPointIO) :
		FPCGExNonAbandonableTask(InManager, InTaskIndex, InPointIO)
	{
	}

	virtual bool ExecuteTask() override;
};
