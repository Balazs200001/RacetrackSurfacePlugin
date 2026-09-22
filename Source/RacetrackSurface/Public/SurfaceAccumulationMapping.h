#pragma once

#include "CoreMinimal.h"

namespace RacetrackSurfaceMapping
{
	static constexpr float SaturationConstant = 3.0f;

	inline float ExposureToCoverage(float Exposure)
	{
		return 1.0f - FMath::Exp(-SaturationConstant * FMath::Max(Exposure, 0.0f));
	}

	static constexpr const TCHAR* ParameterCollectionAssetPath =
		TEXT("/RacetrackSurface/Materials/MPC_RacetrackSurface.MPC_RacetrackSurface");

	static constexpr const TCHAR* SaturationConstantParameterName = TEXT("SaturationConstant");

	static constexpr const TCHAR* AccumulationTargetParameterName = TEXT("AccumulationTarget");
}
