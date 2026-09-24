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

	static constexpr float FallbackDecayHalfLifeSeconds = 900.0f;

	static constexpr float DecayStepsPerHalfLife = 256.0f;

	static constexpr float HalfFloatGuaranteedChangeThreshold = 4.8828125e-4f;

	inline float ElapsedToDecayProgress(float ElapsedSeconds, float RateMultiplier, float HalfLifeSeconds)
	{
		return FMath::Max(RateMultiplier, 0.0f)
			* FMath::Max(ElapsedSeconds, 0.0f)
			/ FMath::Max(HalfLifeSeconds, UE_SMALL_NUMBER);
	}

	inline float DecayRetentionForProgress(float ProgressInHalfLives)
	{
		return ProgressInHalfLives <= 0.0f
			? 1.0f
			: FMath::Clamp(FMath::Exp2(-ProgressInHalfLives), 0.0f, 1.0f);
	}

	static constexpr const TCHAR* ParameterCollectionAssetPath =
		TEXT("/RacetrackSurface/Materials/MPC_RacetrackSurface.MPC_RacetrackSurface");

	static constexpr const TCHAR* SaturationConstantParameterName = TEXT("SaturationConstant");

	static constexpr const TCHAR* AccumulationRowCountParameterName = TEXT("AccumulationRowCount");

	static constexpr const TCHAR* AccumulationTargetParameterName = TEXT("AccumulationTarget");

	static constexpr const TCHAR* DepositionScaleVariableName = TEXT("r.RacetrackSurface.DepositionScale");

	static constexpr const TCHAR* DecayRateVariableName = TEXT("r.RacetrackSurface.DecayRate");

	static constexpr const TCHAR* DecayStepsPerHalfLifeVariableName =
		TEXT("r.RacetrackSurface.DecayStepsPerHalfLife");
}
