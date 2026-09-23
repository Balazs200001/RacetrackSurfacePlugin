#pragma once

#include "CoreMinimal.h"

namespace RacetrackSurfaceMapping
{
	static constexpr float SaturationConstant = 3.0f;

	static constexpr float ExposurePerUnitSlip = 0.002f;

	inline float ExposureToCoverage(float Exposure)
	{
		return 1.0f - FMath::Exp(-SaturationConstant * FMath::Max(Exposure, 0.0f));
	}

	inline float SlipToExposure(float SlipAmount, float DepositionScale)
	{
		return FMath::Clamp(SlipAmount, 0.0f, 1.0f) * ExposurePerUnitSlip * FMath::Max(DepositionScale, 0.0f);
	}

	static constexpr const TCHAR* ParameterCollectionAssetPath =
		TEXT("/RacetrackSurface/Materials/MPC_RacetrackSurface.MPC_RacetrackSurface");

	static constexpr const TCHAR* SaturationConstantParameterName = TEXT("SaturationConstant");

	static constexpr const TCHAR* AccumulationTargetParameterName = TEXT("AccumulationTarget");

	static constexpr const TCHAR* DepositionScaleVariableName = TEXT("r.RacetrackSurface.DepositionScale");
}
