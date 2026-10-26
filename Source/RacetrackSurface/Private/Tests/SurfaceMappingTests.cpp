// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "SurfaceAccumulationMapping.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceExposureToCoverageZeroIsZero,
	"RacetrackSurface.Mapping.ExposureToCoverageZeroIsZero",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceExposureToCoverageZeroIsZero::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Zero exposure maps to zero coverage"),
		RacetrackSurfaceMapping::ExposureToCoverage(0.0f),
		0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceExposureToCoverageClampsNegativeExposure,
	"RacetrackSurface.Mapping.ExposureToCoverageClampsNegativeExposure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceExposureToCoverageClampsNegativeExposure::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Negative exposure is treated as zero, not reflected below zero"),
		RacetrackSurfaceMapping::ExposureToCoverage(-5.0f),
		0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceExposureToCoverageIsMonotonic,
	"RacetrackSurface.Mapping.ExposureToCoverageIsMonotonic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceExposureToCoverageIsMonotonic::RunTest(const FString& Parameters)
{
	const float Low = RacetrackSurfaceMapping::ExposureToCoverage(0.5f);
	const float Mid = RacetrackSurfaceMapping::ExposureToCoverage(1.0f);
	const float High = RacetrackSurfaceMapping::ExposureToCoverage(2.0f);

	TestTrue(TEXT("Coverage increases from 0.5 to 1.0 exposure"), Mid > Low);
	TestTrue(TEXT("Coverage increases from 1.0 to 2.0 exposure"), High > Mid);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceExposureToCoverageNeverOvershootsOne,
	"RacetrackSurface.Mapping.ExposureToCoverageNeverOvershootsOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceExposureToCoverageNeverOvershootsOne::RunTest(const FString& Parameters)
{
	const float ModeratelyHigh = RacetrackSurfaceMapping::ExposureToCoverage(5.0f);
	const float ExtremelyHigh = RacetrackSurfaceMapping::ExposureToCoverage(100.0f);

	TestTrue(TEXT("Coverage is still below one at a moderately high exposure"), ModeratelyHigh < 1.0f);
	TestTrue(TEXT("Coverage is within a tight band of the ceiling there"), ModeratelyHigh > 0.999f);
	TestTrue(TEXT("Coverage never exceeds one even where float precision saturates the curve"), ExtremelyHigh <= 1.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceExposureToCoverageMatchesB6Derivation,
	"RacetrackSurface.Mapping.ExposureToCoverageMatchesB6Derivation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceExposureToCoverageMatchesB6Derivation::RunTest(const FString& Parameters)
{
	const float Coverage = RacetrackSurfaceMapping::ExposureToCoverage(1.0f);
	const float Expected = 1.0f - FMath::Exp(-3.0f);

	TestTrue(
		TEXT("Exposure of 1.0 maps to the documented 0.95 coverage at k=3.0"),
		FMath::IsNearlyEqual(Coverage, Expected, 1.e-4f));

	TestTrue(
		TEXT("Coverage at exposure 1.0 rounds to the 0.95 figure recorded in MEASUREMENTS.md"),
		FMath::IsNearlyEqual(Coverage, 0.95f, 0.001f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayRetentionIsIndependentOfFrameRate,
	"RacetrackSurface.Mapping.DecayRetentionIsIndependentOfFrameRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayRetentionIsIndependentOfFrameRate::RunTest(const FString& Parameters)
{
	constexpr float SpanSeconds = 10.0f;
	constexpr float Rate = 1.0f;
	constexpr float HalfLife = RacetrackSurfaceMapping::FallbackDecayHalfLifeSeconds;

	const float OneStepProgress =
		RacetrackSurfaceMapping::ElapsedToDecayProgress(SpanSeconds, Rate, HalfLife);
	const float OneStepRetention = RacetrackSurfaceMapping::DecayRetentionForProgress(OneStepProgress);

	float AccumulatedProgress = 0.0f;
	float CompoundedRetention = 1.0f;

	constexpr int32 FrameCount = 600;

	for (int32 Frame = 0; Frame < FrameCount; ++Frame)
	{
		const float FrameProgress =
			RacetrackSurfaceMapping::ElapsedToDecayProgress(SpanSeconds / FrameCount, Rate, HalfLife);

		AccumulatedProgress += FrameProgress;
		CompoundedRetention *= RacetrackSurfaceMapping::DecayRetentionForProgress(FrameProgress);
	}

	const float Difference = FMath::Abs(OneStepRetention - CompoundedRetention);
	const float Threshold = RacetrackSurfaceMapping::HalfFloatGuaranteedChangeThreshold;

	AddInfo(FString::Printf(
		TEXT("Over %.0f s at %.2fx the retention is %.9f banked in one step and %.9f compounded over %d "
			"frames, a difference of %.3e, which is %.0f times finer than the %.3e a half float can hold"),
		SpanSeconds,
		Rate,
		OneStepRetention,
		CompoundedRetention,
		FrameCount,
		Difference,
		Threshold / Difference,
		Threshold));

	TestTrue(
		TEXT("Progress banked frame by frame sums to the progress of one long step"),
		FMath::IsNearlyEqual(AccumulatedProgress, OneStepProgress, 1.e-6f));

	TestTrue(
		TEXT("Retention over a span does not depend on how many frames the span was cut into, to well "
			"inside what a half float can represent, the coarsest format decay is still applied to, by the "
			"canvas fallback"),
		Difference < Threshold);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayRateOfZeroRetainsEverything,
	"RacetrackSurface.Mapping.DecayRateOfZeroRetainsEverything",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayRateOfZeroRetainsEverything::RunTest(const FString& Parameters)
{
	const float Spans[] = { 0.0f, 1.0f, 60.0f, 3600.0f, 1.e6f };

	constexpr float HalfLife = RacetrackSurfaceMapping::FallbackDecayHalfLifeSeconds;

	for (const float Span : Spans)
	{
		const float Progress = RacetrackSurfaceMapping::ElapsedToDecayProgress(Span, 0.0f, HalfLife);

		TestEqual(
			*FString::Printf(TEXT("A rate of zero banks no progress over %.0f s"), Span),
			Progress,
			0.0f);

		TestEqual(
			*FString::Printf(TEXT("A rate of zero retains everything over %.0f s"), Span),
			RacetrackSurfaceMapping::DecayRetentionForProgress(Progress),
			1.0f);
	}

	TestEqual(
		TEXT("One whole half-life of progress retains exactly half"),
		RacetrackSurfaceMapping::DecayRetentionForProgress(1.0f),
		0.5f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayStepSurvivesHalfFloatRounding,
	"RacetrackSurface.Mapping.DecayStepSurvivesHalfFloatRounding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayStepSurvivesHalfFloatRounding::RunTest(const FString& Parameters)
{
	const float StepProgress = 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife;
	const float StepRetention = RacetrackSurfaceMapping::DecayRetentionForProgress(StepProgress);
	const float StepChange = 1.0f - StepRetention;
	const float Threshold = RacetrackSurfaceMapping::HalfFloatGuaranteedChangeThreshold;

	const float PerFrameProgress = RacetrackSurfaceMapping::ElapsedToDecayProgress(
		1.0f / 60.0f,
		1.0f,
		RacetrackSurfaceMapping::FallbackDecayHalfLifeSeconds);
	const float PerFrameChange = 1.0f - RacetrackSurfaceMapping::DecayRetentionForProgress(PerFrameProgress);

	AddInfo(FString::Printf(
		TEXT("One step of %.0f per half-life retains %.6f, a change of %.3e against the %.3e that every "
			"half float is guaranteed to notice, so %.1f times the threshold. Decaying every frame instead "
			"would move a texel by %.3e, which is %.0f times too small to change a single bit"),
		RacetrackSurfaceMapping::DecayStepsPerHalfLife,
		StepRetention,
		StepChange,
		Threshold,
		StepChange / Threshold,
		PerFrameChange,
		Threshold / PerFrameChange));

	TestTrue(
		TEXT("One decay step moves a half float by more than the largest half-ulp in the format, so on the "
			"RGBA16f canvas fallback it cannot round back to the value it started at"),
		StepChange > Threshold);

	TestTrue(
		TEXT("Decaying every frame at 60 Hz would be below that threshold, which is why the pass runs on "
			"banked progress rather than on every tick"),
		PerFrameChange < Threshold);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceSkidRetentionIsNeverCloserToOneThanTheSharedRetention,
	"RacetrackSurface.Mapping.SkidRetentionIsNeverCloserToOneThanTheSharedRetention",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceSkidRetentionIsNeverCloserToOneThanTheSharedRetention::RunTest(const FString& Parameters)
{
	const float StepProgress = 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife;
	const float SharedRetention = RacetrackSurfaceMapping::DecayRetentionForProgress(StepProgress);
	const float Threshold = RacetrackSurfaceMapping::HalfFloatGuaranteedChangeThreshold;

	const float RubberExposures[] = { -1.0f, 0.0f, 1.0e-6f, 0.1224f, 1.0f, 4.0f, 32.0f, 64.0f };

	for (const float RubberExposure : RubberExposures)
	{
		const float SkidRetention = RacetrackSurfaceMapping::SkidRetentionForProgress(StepProgress, RubberExposure);

		AddInfo(FString::Printf(
			TEXT("Rubber exposure %.4f: skid decays %.4f times the shared rate and one step retains %.6f "
				"against the shared %.6f, a change of %.3e against the %.3e threshold"),
			RubberExposure,
			RacetrackSurfaceMapping::SkidDecayRateMultiplier(RubberExposure),
			SkidRetention,
			SharedRetention,
			1.0f - SkidRetention,
			Threshold));

		TestTrue(
			*FString::Printf(
				TEXT("At rubber exposure %.4f one skid step retains no more than the shared step, so no texel "
					"can decay slower than the channels whose step already clears the half-float floor"),
				RubberExposure),
			SkidRetention <= SharedRetention);

		TestTrue(
			*FString::Printf(
				TEXT("At rubber exposure %.4f one skid step still moves a half float by more than the largest "
					"half-ulp"),
				RubberExposure),
			1.0f - SkidRetention > Threshold);
	}

	TestEqual(
		TEXT("Bare track decays skid at exactly the shared rate, so the coupling reduces to C10's pass off the "
			"line"),
		RacetrackSurfaceMapping::SkidRetentionForProgress(StepProgress, 0.0f),
		SharedRetention);

	TestEqual(
		TEXT("Negative exposure cannot slow skid below the shared rate"),
		RacetrackSurfaceMapping::SkidRetentionForProgress(StepProgress, -1.0f),
		SharedRetention);

	TestEqual(
		TEXT("At the calibrated apex exposure of 1.0, skid decays 1 + k = 4 times as fast"),
		RacetrackSurfaceMapping::SkidDecayRateMultiplier(1.0f),
		1.0f + RacetrackSurfaceMapping::SaturationConstant);

	TestTrue(
		TEXT("Over one half-life at the calibrated apex, skid falls to 2^-4 of what was laid"),
		FMath::IsNearlyEqual(
			RacetrackSurfaceMapping::SkidRetentionForProgress(1.0f, 1.0f),
			FMath::Exp2(-(1.0f + RacetrackSurfaceMapping::SaturationConstant)),
			1.0e-6f));

	return true;
}

#endif
