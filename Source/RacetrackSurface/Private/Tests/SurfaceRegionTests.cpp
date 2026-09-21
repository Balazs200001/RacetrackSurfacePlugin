#include "Misc/AutomationTest.h"
#include "SurfaceRegion.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::MakeTestRegion;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceRegionCentreMapsToHalf,
	"RacetrackSurface.Region.CentreMapsToHalf",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceRegionCentreMapsToHalf::RunTest(const FString& Parameters)
{
	const FSurfaceRegion Region = MakeTestRegion();
	const FVector2D Normalised = Region.WorldToNormalised(FVector::ZeroVector);

	TestTrue(TEXT("Centre maps to 0.5 in X"), FMath::IsNearlyEqual(Normalised.X, 0.5, UE_DOUBLE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("Centre maps to 0.5 in Y"), FMath::IsNearlyEqual(Normalised.Y, 0.5, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceRegionCornersMapToUnitRange,
	"RacetrackSurface.Region.CornersMapToUnitRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceRegionCornersMapToUnitRange::RunTest(const FString& Parameters)
{
	const FSurfaceRegion Region = MakeTestRegion();

	const FVector2D MinCorner = Region.WorldToNormalised(FVector(-10000.0, -10000.0, 0.0));
	const FVector2D MaxCorner = Region.WorldToNormalised(FVector(10000.0, 10000.0, 0.0));

	TestTrue(TEXT("Min corner maps to zero"), MinCorner.Equals(FVector2D::ZeroVector, UE_DOUBLE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("Max corner maps to one"), MaxCorner.Equals(FVector2D(1.0, 1.0), UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceRegionRejectsPositionsOutside,
	"RacetrackSurface.Region.RejectsPositionsOutside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceRegionRejectsPositionsOutside::RunTest(const FString& Parameters)
{
	const FSurfaceRegion Region = MakeTestRegion();

	TestTrue(TEXT("Centre is inside"), Region.ContainsWorldPosition(FVector::ZeroVector));
	TestFalse(TEXT("Beyond positive edge is outside"), Region.ContainsWorldPosition(FVector(10001.0, 0.0, 0.0)));
	TestFalse(TEXT("Beyond negative edge is outside"), Region.ContainsWorldPosition(FVector(0.0, -10001.0, 0.0)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceRegionTexelDensity,
	"RacetrackSurface.Region.TexelDensity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceRegionTexelDensity::RunTest(const FString& Parameters)
{
	const FSurfaceRegion Region = MakeTestRegion();

	const double ExpectedCentimetresPerTexel = 20000.0 / 2048.0;
	TestTrue(
		TEXT("Centimetres per texel follows size over resolution"),
		FMath::IsNearlyEqual(Region.CentimetresPerTexel(), ExpectedCentimetresPerTexel, UE_DOUBLE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("A contact patch converts to the matching texel count"),
		FMath::IsNearlyEqual(Region.WorldDistanceToTexels(ExpectedCentimetresPerTexel), 1.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceRegionHonoursNonZeroCentre,
	"RacetrackSurface.Region.HonoursNonZeroCentre",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceRegionHonoursNonZeroCentre::RunTest(const FString& Parameters)
{
	FSurfaceRegion Region = MakeTestRegion();
	Region.WorldCentre = FVector2D(50000.0, -30000.0);

	const FVector2D Normalised = Region.WorldToNormalised(FVector(50000.0, -30000.0, 0.0));

	TestTrue(TEXT("Offset centre still maps to the middle"), Normalised.Equals(FVector2D(0.5, 0.5), UE_DOUBLE_KINDA_SMALL_NUMBER));
	TestFalse(TEXT("World origin is now outside the region"), Region.ContainsWorldPosition(FVector::ZeroVector));

	return true;
}

#endif
