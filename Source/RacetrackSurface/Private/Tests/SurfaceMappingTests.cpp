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

#endif
