﻿// Copyright Timothé Lapetite 2024
// Released under the MIT license https://opensource.org/license/MIT/

#include "Misc/Filters/PCGExStringCompareFilter.h"

PCGExPointFilter::TFilter* UPCGExStringCompareFilterFactory::CreateFilter() const
{
	return new PCGExPointsFilter::TStringCompareFilter(this);
}

bool PCGExPointsFilter::TStringCompareFilter::Init(const FPCGContext* InContext, PCGExData::FFacade* InPointDataFacade)
{
	if (!TFilter::Init(InContext, InPointDataFacade)) { return false; }

	OperandA = new PCGEx::TFAttributeReader<FString>(TypedFilterFactory->Config.OperandA.GetName());
	if (!OperandA->Bind(InPointDataFacade->Source))
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, FText::Format(FTEXT("Invalid Operand A attribute: {0}."), FText::FromName(TypedFilterFactory->Config.OperandA.GetName())));
		PCGEX_DELETE(OperandA)
		return false;
	}

	if (TypedFilterFactory->Config.CompareAgainst == EPCGExFetchType::Attribute)
	{
		OperandB = new PCGEx::TFAttributeReader<FString>(TypedFilterFactory->Config.OperandB.GetName());
		if (!OperandB->Bind(InPointDataFacade->Source))
		{
			PCGE_LOG_C(Error, GraphAndLog, InContext, FText::Format(FTEXT("Invalid Operand B attribute: {0}."), FText::FromName(TypedFilterFactory->Config.OperandB.GetName())));
			PCGEX_DELETE(OperandA)
			PCGEX_DELETE(OperandB)
			return false;
		}
	}

	return true;
}

namespace PCGExCompareFilter
{
}

#define LOCTEXT_NAMESPACE "PCGExCompareFilterDefinition"
#define PCGEX_NAMESPACE CompareFilterDefinition

PCGEX_CREATE_FILTER_FACTORY(StringCompare)

#if WITH_EDITOR
FString UPCGExStringCompareFilterProviderSettings::GetDisplayName() const
{
	FString DisplayName = Config.OperandA.GetName().ToString();

	switch (Config.Comparison)
	{
	case EPCGExStringComparison::StrictlyEqual:
		DisplayName += " == ";
		break;
	case EPCGExStringComparison::StrictlyNotEqual:
		DisplayName += " != ";
		break;
	case EPCGExStringComparison::LengthStrictlyEqual:
		DisplayName += " L == L ";
		break;
	case EPCGExStringComparison::LengthStrictlyUnequal:
		DisplayName += " L != L ";
		break;
	case EPCGExStringComparison::LengthEqualOrGreater:
		DisplayName += " L >= L ";
		break;
	case EPCGExStringComparison::LengthEqualOrSmaller:
		DisplayName += " L <= L ";
		break;
	case EPCGExStringComparison::StrictlyGreater:
		DisplayName += " L > L ";
		break;
	case EPCGExStringComparison::StrictlySmaller:
		DisplayName += " L < L ";
		break;
	case EPCGExStringComparison::LocaleStrictlyGreater:
		DisplayName += " > ";
		break;
	case EPCGExStringComparison::LocaleStrictlySmaller:
		DisplayName += " < ";
		break;
	default: ;
	}

	DisplayName += Config.OperandB.GetName().ToString();
	return DisplayName;
}
#endif


#undef LOCTEXT_NAMESPACE
#undef PCGEX_NAMESPACE
