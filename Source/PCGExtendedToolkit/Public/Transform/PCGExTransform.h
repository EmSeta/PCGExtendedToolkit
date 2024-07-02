﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#pragma once
#include "PCGExSettings.h"
#include "Data/PCGExData.h"

#include "PCGExTransform.generated.h"

UENUM(BlueprintType, meta=(DisplayName="[PCGEx] Point Bounds Source"))
enum class EPCGExPointBoundsSource : uint8
{
	DensityBounds UMETA(DisplayName = "Density Bounds", ToolTip="TBD"),
	ScaledExtents UMETA(DisplayName = "Scaled Extents", ToolTip="TBD"),
	Extents UMETA(DisplayName = "Extents", ToolTip="TBD")
};

namespace PCGExTransform
{
	FORCEINLINE static void GetBounds(const FPCGPoint& Point, const EPCGExPointBoundsSource Source, FBox& OutBounds)
	{
		FVector S = Point.Transform.GetScale3D();
		switch (Source)
		{
		case EPCGExPointBoundsSource::DensityBounds:
			OutBounds = Point.GetLocalDensityBounds();
			break;
		case EPCGExPointBoundsSource::ScaledExtents:
			OutBounds = FBox(Point.BoundsMin * S, Point.BoundsMax * S);
			break;
		default: ;
		case EPCGExPointBoundsSource::Extents:
			OutBounds = FBox(Point.BoundsMin, Point.BoundsMax);
			break;
		}
	}
}

USTRUCT(BlueprintType)
struct PCGEXTENDEDTOOLKIT_API FPCGExUVW
{
	GENERATED_BODY()

	FPCGExUVW()
	{
	}

	explicit FPCGExUVW(double DefaultW)
		: WConstant(DefaultW)
	{
	}

	/** Overlap overlap test mode */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable))
	EPCGExPointBoundsSource BoundsReference = EPCGExPointBoundsSource::ScaledExtents;

	/** U Source */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExFetchType USource = EPCGExFetchType::Constant;

	/** U Constant */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="USource==EPCGExFetchType::Constant", EditConditionHides, DisplayName="U"))
	double UConstant = 0.5;

	/** U Attribute */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="USource==EPCGExFetchType::Attribute", EditConditionHides, DisplayName="U"))
	FPCGAttributePropertyInputSelector UAttribute;

	/** V Source */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExFetchType VSource = EPCGExFetchType::Constant;

	/** V Constant */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="VSource==EPCGExFetchType::Constant", EditConditionHides, DisplayName="V"))
	double VConstant = 0.5;

	/** V Attribute */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="USource==EPCGExFetchType::Attribute", EditConditionHides, DisplayName="V"))
	FPCGAttributePropertyInputSelector VAttribute;

	/** W Source */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_NotOverridable))
	EPCGExFetchType WSource = EPCGExFetchType::Constant;

	/** W Constant */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="WSource==EPCGExFetchType::Constant", EditConditionHides, DisplayName="W"))
	double WConstant = 0.5;

	/** W Attribute */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = Settings, meta=(PCG_Overridable, EditCondition="USource==EPCGExFetchType::Attribute", EditConditionHides, DisplayName="W"))
	FPCGAttributePropertyInputSelector WAttribute;

	PCGExData::FCache<double>* UGetter = nullptr;
	PCGExData::FCache<double>* VGetter = nullptr;
	PCGExData::FCache<double>* WGetter = nullptr;

	bool Init(const FPCGContext* InContext, PCGExData::FFacade* InDataFacade)
	{
		if (USource == EPCGExFetchType::Attribute)
		{
			UGetter = InDataFacade->GetOrCreateGetter<double>(UAttribute);
			if (!UGetter)
			{
				PCGE_LOG_C(Error, GraphAndLog, InContext, FText::FromString(TEXT("Invalid attribute for U.")));
				return false;
			}
		}

		if (VSource == EPCGExFetchType::Attribute)
		{
			VGetter = InDataFacade->GetOrCreateGetter<double>(VAttribute);
			if (!VGetter)
			{
				PCGE_LOG_C(Error, GraphAndLog, InContext, FText::FromString(TEXT("Invalid attribute for V.")));
				return false;
			}
		}

		if (WSource == EPCGExFetchType::Attribute)
		{
			WGetter = InDataFacade->GetOrCreateGetter<double>(WAttribute);
			if (!WGetter)
			{
				PCGE_LOG_C(Error, GraphAndLog, InContext, FText::FromString(TEXT("Invalid attribute for W.")));
				return false;
			}
		}

		return true;
	}

	// Without axis

	FVector GetUVW(const int32 PointIndex) const
	{
		return FVector(
			UGetter ? UGetter->Values[PointIndex] : UConstant,
			VGetter ? VGetter->Values[PointIndex] : VConstant,
			WGetter ? WGetter->Values[PointIndex] : WConstant);
	}

	FVector GetPosition(const PCGEx::FPointRef& PointRef) const
	{
		FBox Bounds;
		PCGExTransform::GetBounds(*PointRef.Point, BoundsReference, Bounds);
		const FVector LocalPosition = Bounds.Min + ((Bounds.GetExtent() * 2) * GetUVW(PointRef.Index));
		return PointRef.Point->Transform.TransformPositionNoScale(LocalPosition);
	}

	FVector GetPosition(const PCGEx::FPointRef& PointRef, FVector& OutOffset) const
	{
		FBox Bounds;
		PCGExTransform::GetBounds(*PointRef.Point, BoundsReference, Bounds);
		const FVector LocalPosition = Bounds.Min + ((Bounds.GetExtent() * 2) * GetUVW(PointRef.Index));
		OutOffset = PointRef.Point->Transform.TransformVectorNoScale(LocalPosition - Bounds.GetCenter());
		return PointRef.Point->Transform.TransformPositionNoScale(LocalPosition);
	}

	// With axis

	FVector GetUVW(const int32 PointIndex, const EPCGExMinimalAxis Axis) const
	{
		switch (Axis)
		{
		default: ;
		case EPCGExMinimalAxis::None:
		case EPCGExMinimalAxis::Z:
			return FVector(
				UGetter ? UGetter->Values[PointIndex] : UConstant,
				VGetter ? VGetter->Values[PointIndex] : VConstant,
				WGetter ? WGetter->Values[PointIndex] : WConstant);
		case EPCGExMinimalAxis::X:
			return FVector(
				WGetter ? WGetter->Values[PointIndex] : WConstant,
				UGetter ? UGetter->Values[PointIndex] : UConstant,
				VGetter ? VGetter->Values[PointIndex] : VConstant);
		case EPCGExMinimalAxis::Y:
			return FVector(
				UGetter ? UGetter->Values[PointIndex] : UConstant,
				WGetter ? WGetter->Values[PointIndex] : WConstant,
				VGetter ? VGetter->Values[PointIndex] : VConstant);
		}
	}

	FVector GetPosition(const PCGEx::FPointRef& PointRef, const EPCGExMinimalAxis Axis) const
	{
		FBox Bounds;
		PCGExTransform::GetBounds(*PointRef.Point, BoundsReference, Bounds);
		const FVector LocalPosition = Bounds.Min + ((Bounds.GetExtent() * 2) * GetUVW(PointRef.Index, Axis));
		return PointRef.Point->Transform.TransformPositionNoScale(LocalPosition);
	}

	FVector GetPosition(const PCGEx::FPointRef& PointRef, FVector& OutOffset, const EPCGExMinimalAxis Axis) const
	{
		FBox Bounds;
		PCGExTransform::GetBounds(*PointRef.Point, BoundsReference, Bounds);
		const FVector LocalPosition = Bounds.Min + ((Bounds.GetExtent() * 2) * GetUVW(PointRef.Index, Axis));
		OutOffset = PointRef.Point->Transform.TransformVectorNoScale(LocalPosition - Bounds.GetCenter());
		return PointRef.Point->Transform.TransformPositionNoScale(LocalPosition);
	}
};
