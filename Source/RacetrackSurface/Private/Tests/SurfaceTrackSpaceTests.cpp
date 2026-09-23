#include "Misc/AutomationTest.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "SurfaceTrackRibbon.h"
#include "SurfaceTrackSpace.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::MakeClosedCircleSpline;
using RacetrackSurfaceTest::MakeSplineFromPoints;
using RacetrackSurfaceTrackSpace::TrackToWorld;
using RacetrackSurfaceTrackSpace::WorldToTrack;

namespace
{
	constexpr double TestTrackWidth = 1200.0;

	TStrongObjectPtr<USplineComponent> MakeClosedCircle()
	{
		return MakeClosedCircleSpline(5000.0, 16);
	}

	TStrongObjectPtr<USplineComponent> MakeSerpentine()
	{
		constexpr double Amplitude = 2000.0;
		constexpr double Wavelength = 20000.0;
		constexpr double Span = 40000.0;
		constexpr int32 PointCount = 21;

		TArray<FVector> Points;
		Points.Reserve(PointCount);

		for (int32 Index = 0; Index < PointCount; ++Index)
		{
			const double X = Span * Index / (PointCount - 1);
			Points.Add(FVector(X, Amplitude * FMath::Sin(2.0 * UE_DOUBLE_PI * X / Wavelength), 0.0));
		}

		return MakeSplineFromPoints(Points, false);
	}

	FSurfaceTrackCoordinate MakeCoordinate(double Distance, double Alpha)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = Distance;
		Coordinate.LateralOffsetAlpha = Alpha;
		return Coordinate;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackSpaceCentrelineMapsToZeroOffset,
	"RacetrackSurface.TrackSpace.CentrelineMapsToZeroOffset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackSpaceCentrelineMapsToZeroOffset::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USplineComponent> Circle = MakeClosedCircle();
	const double Length = Circle->GetSplineLength();

	TestTrue(TEXT("The test circle has positive length"), Length > 0.0);

	for (int32 Step = 1; Step < 8; ++Step)
	{
		const double Distance = Length * Step / 8.0;
		const FVector OnCentreline = TrackToWorld(*Circle, TestTrackWidth, MakeCoordinate(Distance, 0.0));

		FSurfaceTrackCoordinate Recovered;
		const ETrackSpaceStatus Status = WorldToTrack(*Circle, TestTrackWidth, OnCentreline, Recovered);

		TestEqual(TEXT("A point on the centreline is inside the ribbon"), Status, ETrackSpaceStatus::Valid);

		TestTrue(
			FString::Printf(TEXT("Centreline point at %.0f cm maps to offset zero, got %.6f"), Distance, Recovered.LateralOffsetAlpha),
			FMath::Abs(Recovered.LateralOffsetAlpha) < 1.0e-3);

		TestTrue(
			FString::Printf(TEXT("Centreline point at %.0f cm recovers its distance, got %.2f"), Distance, Recovered.DistanceAlongCentreline),
			FMath::Abs(Recovered.DistanceAlongCentreline - Distance) < 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackSpaceOffsetSignSurvivesCurvatureReversal,
	"RacetrackSurface.TrackSpace.OffsetSignSurvivesCurvatureReversal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackSpaceOffsetSignSurvivesCurvatureReversal::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USplineComponent> Serpentine = MakeSerpentine();
	const double Length = Serpentine->GetSplineLength();

	TestTrue(TEXT("The serpentine has positive length"), Length > 0.0);

	int32 CurvatureSignChanges = 0;
	double PreviousCurvatureSign = 0.0;

	for (int32 Step = 1; Step < 40; ++Step)
	{
		const double Distance = Length * Step / 40.0;

		const FVector Before = Serpentine->GetDirectionAtDistanceAlongSpline(
			static_cast<float>(FMath::Max(Distance - 200.0, 0.0)), ESplineCoordinateSpace::World);
		const FVector After = Serpentine->GetDirectionAtDistanceAlongSpline(
			static_cast<float>(FMath::Min(Distance + 200.0, Length)), ESplineCoordinateSpace::World);

		const double Turn = FMath::Sign(FVector::CrossProduct(Before, After).Z);

		if (PreviousCurvatureSign != 0.0 && Turn != 0.0 && Turn != PreviousCurvatureSign)
		{
			++CurvatureSignChanges;
		}

		if (Turn != 0.0)
		{
			PreviousCurvatureSign = Turn;
		}

		const FVector ToTheRight = TrackToWorld(*Serpentine, TestTrackWidth, MakeCoordinate(Distance, 0.5));
		const FVector ToTheLeft = TrackToWorld(*Serpentine, TestTrackWidth, MakeCoordinate(Distance, -0.5));

		FSurfaceTrackCoordinate RecoveredRight;
		FSurfaceTrackCoordinate RecoveredLeft;

		WorldToTrack(*Serpentine, TestTrackWidth, ToTheRight, RecoveredRight);
		WorldToTrack(*Serpentine, TestTrackWidth, ToTheLeft, RecoveredLeft);

		TestTrue(
			FString::Printf(TEXT("Right of the centreline stays positive at %.0f cm, got %.4f"), Distance, RecoveredRight.LateralOffsetAlpha),
			RecoveredRight.LateralOffsetAlpha > 0.4 && RecoveredRight.LateralOffsetAlpha < 0.6);

		TestTrue(
			FString::Printf(TEXT("Left of the centreline stays negative at %.0f cm, got %.4f"), Distance, RecoveredLeft.LateralOffsetAlpha),
			RecoveredLeft.LateralOffsetAlpha < -0.4 && RecoveredLeft.LateralOffsetAlpha > -0.6);
	}

	TestTrue(
		FString::Printf(TEXT("The serpentine reverses curvature so both handednesses are covered, %d reversals"), CurvatureSignChanges),
		CurvatureSignChanges >= 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackSpaceWorldRoundTripsThroughTrackSpace,
	"RacetrackSurface.TrackSpace.WorldRoundTripsThroughTrackSpace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackSpaceWorldRoundTripsThroughTrackSpace::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USplineComponent> Circle = MakeClosedCircle();
	const double Length = Circle->GetSplineLength();

	double WorstError = 0.0;

	for (int32 Step = 0; Step < 24; ++Step)
	{
		const double Distance = Length * Step / 24.0;
		const double Alpha = -0.9 + 1.8 * (Step % 7) / 6.0;

		const FVector Original = TrackToWorld(*Circle, TestTrackWidth, MakeCoordinate(Distance, Alpha));

		FSurfaceTrackCoordinate Recovered;
		WorldToTrack(*Circle, TestTrackWidth, Original, Recovered);

		const FVector RoundTripped = TrackToWorld(*Circle, TestTrackWidth, Recovered);

		WorstError = FMath::Max(WorstError, (RoundTripped - Original).Size());
	}

	AddInfo(FString::Printf(TEXT("Worst world round-trip error over 24 samples: %.4f cm"), WorstError));

	TestTrue(
		FString::Printf(TEXT("World round-trips through track space within a tenth of a texel, worst %.4f cm"), WorstError),
		WorstError < 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackSpaceBeyondTheTrackEdgeIsReportedNotClamped,
	"RacetrackSurface.TrackSpace.BeyondTheTrackEdgeIsReportedNotClamped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackSpaceBeyondTheTrackEdgeIsReportedNotClamped::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USplineComponent> Circle = MakeClosedCircle();
	const double Distance = Circle->GetSplineLength() * 0.3;

	const FVector OnTheEdge = TrackToWorld(*Circle, TestTrackWidth, MakeCoordinate(Distance, 1.0));
	const FVector OffTheTrack = TrackToWorld(*Circle, TestTrackWidth, MakeCoordinate(Distance, 1.5));

	FSurfaceTrackCoordinate EdgeCoordinate;
	FSurfaceTrackCoordinate OutsideCoordinate;

	const ETrackSpaceStatus EdgeStatus = WorldToTrack(*Circle, TestTrackWidth, OnTheEdge, EdgeCoordinate);
	const ETrackSpaceStatus OutsideStatus = WorldToTrack(*Circle, TestTrackWidth, OffTheTrack, OutsideCoordinate);

	TestEqual(TEXT("The track edge itself is still inside"), EdgeStatus, ETrackSpaceStatus::Valid);
	TestEqual(TEXT("Beyond the edge is reported out of bounds"), OutsideStatus, ETrackSpaceStatus::BeyondTrackEdge);

	TestTrue(
		FString::Printf(TEXT("The out-of-bounds offset is reported as it is, not clamped to one, got %.4f"), OutsideCoordinate.LateralOffsetAlpha),
		OutsideCoordinate.LateralOffsetAlpha > 1.4);

	const FSurfaceTrackRibbon Ribbon;

	TestFalse(TEXT("The ribbon agrees the offset is outside"), Ribbon.ContainsLateralOffset(OutsideCoordinate.LateralOffsetAlpha));
	TestTrue(TEXT("The ribbon agrees the edge is inside"), Ribbon.ContainsLateralOffset(EdgeCoordinate.LateralOffsetAlpha));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackRibbonDistanceWrapsOnAClosedLoop,
	"RacetrackSurface.Ribbon.DistanceWrapsOnAClosedLoop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackRibbonDistanceWrapsOnAClosedLoop::RunTest(const FString& Parameters)
{
	FSurfaceTrackRibbon Ribbon;
	Ribbon.TrackLength = 30000.0;
	Ribbon.bClosedLoop = true;

	TestTrue(TEXT("A quarter lap is unchanged"), FMath::IsNearlyEqual(Ribbon.WrapDistance(7500.0), 7500.0));
	TestTrue(TEXT("A full lap wraps back to zero"), FMath::IsNearlyEqual(Ribbon.WrapDistance(30000.0), 0.0));
	TestTrue(TEXT("Beyond a lap wraps forwards"), FMath::IsNearlyEqual(Ribbon.WrapDistance(37500.0), 7500.0));
	TestTrue(TEXT("A negative distance wraps forwards rather than staying negative"), FMath::IsNearlyEqual(Ribbon.WrapDistance(-7500.0), 22500.0));
	TestTrue(TEXT("Several laps back still wraps forwards"), FMath::IsNearlyEqual(Ribbon.WrapDistance(-67500.0), 22500.0));

	Ribbon.bClosedLoop = false;

	TestTrue(TEXT("An open track clamps past the end instead of wrapping"), FMath::IsNearlyEqual(Ribbon.WrapDistance(37500.0), 30000.0));
	TestTrue(TEXT("An open track clamps before the start"), FMath::IsNearlyEqual(Ribbon.WrapDistance(-7500.0), 0.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackRibbonTexelExtentsFollowTrackDimensions,
	"RacetrackSurface.Ribbon.TexelExtentsFollowTrackDimensions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackRibbonTexelExtentsFollowTrackDimensions::RunTest(const FString& Parameters)
{
	FSurfaceTrackRibbon Harness;
	Harness.TrackLength = 30921.0;
	Harness.TrackWidth = 1200.0;

	TestTrue(TEXT("The C8 density is the default"), FMath::IsNearlyEqual(Harness.CentimetresPerTexel, 3.125));
	TestEqual(TEXT("The harness circuit is 9895 texels along"), Harness.AlongTrackTexels(), 9895);
	TestEqual(TEXT("The harness circuit is 384 texels across"), Harness.AcrossTrackTexels(), 384);
	TestEqual(TEXT("The harness circuit is short enough to need one row"), Harness.RowCount(), 1);

	const int64 HarnessBytes = Harness.AtlasTexelCount() * 8;

	AddInfo(FString::Printf(
		TEXT("Harness circuit %.0f cm by %.0f cm at %.4f cm/texel: %d x %d texels in %d row, allocating %d x %d, "
			"%lld bytes (%.2f MiB) at RGBA16f"),
		Harness.TrackLength,
		Harness.TrackWidth,
		Harness.CentimetresPerTexel,
		Harness.AlongTrackTexels(),
		Harness.AcrossTrackTexels(),
		Harness.RowCount(),
		Harness.AtlasWidth(),
		Harness.AtlasHeight(),
		HarnessBytes,
		static_cast<double>(HarnessBytes) / (1024.0 * 1024.0)));

	TestTrue(TEXT("A one-row ribbon allocates exactly its own texel extents"),
		Harness.AtlasWidth() == Harness.AlongTrackTexels() && Harness.AtlasHeight() == Harness.AcrossTrackTexels());

	FSurfaceTrackRibbon Spa;
	Spa.TrackLength = 700400.0;
	Spa.TrackWidth = 1500.0;

	const int64 SpaBytes = Spa.AtlasTexelCount() * 8;

	AddInfo(FString::Printf(
		TEXT("Real Spa %.0f cm by %.0f cm at %.4f cm/texel: %d x %d texels in %d rows of %d, allocating %d x %d, "
			"%lld bytes (%.1f MB) at RGBA16f"),
		Spa.TrackLength,
		Spa.TrackWidth,
		Spa.CentimetresPerTexel,
		Spa.AlongTrackTexels(),
		Spa.AcrossTrackTexels(),
		Spa.RowCount(),
		Spa.TexelsPerRow(),
		Spa.AtlasWidth(),
		Spa.AtlasHeight(),
		SpaBytes,
		static_cast<double>(SpaBytes) / 1.0e6));

	TestEqual(TEXT("Real Spa needs fourteen rows at the production density"), Spa.RowCount(), 14);
	TestTrue(TEXT("Real Spa still fits the texture cap once folded"), Spa.FitsTheTextureCap());

	TestTrue(TEXT("A width of 20 cm is 6.4 texels at the production density"),
		FMath::IsNearlyEqual(Harness.WorldDistanceToTexels(20.0), 6.4, 0.001));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackRibbonFoldsIntoRowsBeyondTheUnfoldedLimit,
	"RacetrackSurface.Ribbon.FoldsIntoRowsBeyondTheUnfoldedLimit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackRibbonFoldsIntoRowsBeyondTheUnfoldedLimit::RunTest(const FString& Parameters)
{
	FSurfaceTrackRibbon Ribbon;

	TestEqual(TEXT("The maximum row is the hardware 2D texture limit"), FSurfaceTrackRibbon::MaximumTexelsPerRow, 16384);

	TestTrue(
		FString::Printf(TEXT("One row reaches 512 m at the production density, got %.1f cm"), Ribbon.MaximumUnfoldedTrackLength()),
		FMath::IsNearlyEqual(Ribbon.MaximumUnfoldedTrackLength(), 51200.0, 1.0));

	Ribbon.TrackLength = Ribbon.MaximumUnfoldedTrackLength();
	TestTrue(TEXT("A track exactly at the limit still needs only one row"), Ribbon.FitsUnfolded());
	TestEqual(TEXT("A track exactly at the limit uses every texel of the row"), Ribbon.AlongTrackTexels(), 16384);
	TestEqual(TEXT("A one-row ribbon spends the whole lap on that row"), Ribbon.TexelsPerRow(), 16384);

	Ribbon.TrackLength = Ribbon.MaximumUnfoldedTrackLength() + Ribbon.CentimetresPerTexel;
	TestEqual(TEXT("One texel past the limit folds into two rows"), Ribbon.RowCount(), 2);
	TestEqual(TEXT("Two rows split the lap evenly, rounding up"), Ribbon.TexelsPerRow(), 8193);
	TestTrue(
		TEXT("Folding never loses along-track texels, it only rounds the last row up"),
		Ribbon.RowCount() * Ribbon.TexelsPerRow() >= Ribbon.AlongTrackTexels());

	Ribbon.TrackLength = 180000.0;
	TestEqual(TEXT("The demo lap folds into four rows"), Ribbon.RowCount(), 4);
	TestEqual(TEXT("The demo lap divides exactly, so no row carries slack"), Ribbon.TexelsPerRow(), 14400);
	TestEqual(TEXT("The demo lap allocates 14400 texels wide"), Ribbon.AtlasWidth(), 14400);
	TestEqual(TEXT("The demo lap allocates 1024 texels tall"), Ribbon.AtlasHeight(), 1024);
	TestEqual(TEXT("The demo lap allocates 117964800 bytes at RGBA16f"), Ribbon.AtlasTexelCount() * 8, 117964800ll);

	Ribbon.TrackLength = 700400.0;
	TestFalse(TEXT("Real Spa does not fit in one row"), Ribbon.FitsUnfolded());
	TestEqual(TEXT("Real Spa folds into fourteen rows"), Ribbon.RowCount(), 14);
	TestTrue(TEXT("Real Spa still fits the texture cap once folded"), Ribbon.FitsTheTextureCap());

	AddInfo(FString::Printf(
		TEXT("Real Spa at %.4f cm per texel: %d along, %d rows of %d, allocating %d x %d"),
		Ribbon.CentimetresPerTexel,
		Ribbon.AlongTrackTexels(),
		Ribbon.RowCount(),
		Ribbon.TexelsPerRow(),
		Ribbon.AtlasWidth(),
		Ribbon.AtlasHeight()));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackRibbonAtlasMappingRoundTrips,
	"RacetrackSurface.Ribbon.AtlasMappingRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackRibbonAtlasMappingRoundTrips::RunTest(const FString& Parameters)
{
	FSurfaceTrackRibbon Ribbon;
	Ribbon.TrackLength = 180000.0;

	const int32 Rows = Ribbon.RowCount();

	TestEqual(TEXT("The demo lap folds into four rows"), Rows, 4);

	const FVector2D LapStart = Ribbon.RibbonToAtlas(0.0, 0.5);

	TestTrue(
		TEXT("The start of the lap is the start of the first row"),
		LapStart.Equals(FVector2D(0.0, 0.5 / Rows), UE_DOUBLE_KINDA_SMALL_NUMBER));

	const FVector2D LapEnd = Ribbon.RibbonToAtlas(1.0, 0.5);

	TestTrue(
		TEXT("The end of the lap is the end of the last row, not the start of a row that does not exist"),
		LapEnd.Equals(FVector2D(1.0, (Rows - 1 + 0.5) / Rows), UE_DOUBLE_KINDA_SMALL_NUMBER));

	for (int32 Row = 0; Row < Rows; ++Row)
	{
		const double RowStart = static_cast<double>(Row) / Rows;

		TestEqual(
			FString::Printf(TEXT("A coordinate just inside row %d reports row %d"), Row, Row),
			Ribbon.RowIndexAt(RowStart + 0.5 / Rows),
			Row);

		const FVector2D AtRowStart = Ribbon.RibbonToAtlasInRow(Row, RowStart, 0.0);
		const FVector2D AtRowEnd = Ribbon.RibbonToAtlasInRow(Row, RowStart + 1.0 / Rows, 1.0);

		TestTrue(
			FString::Printf(TEXT("Row %d starts at the left edge of the atlas"), Row),
			FMath::IsNearlyZero(AtRowStart.X, UE_DOUBLE_KINDA_SMALL_NUMBER));

		TestTrue(
			FString::Printf(TEXT("Row %d ends at the right edge of the atlas"), Row),
			FMath::IsNearlyEqual(AtRowEnd.X, 1.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

		TestTrue(
			FString::Printf(TEXT("Row %d occupies its own band and no other"), Row),
			AtRowStart.Y >= static_cast<double>(Row) / Rows - UE_DOUBLE_KINDA_SMALL_NUMBER
				&& AtRowEnd.Y <= static_cast<double>(Row + 1) / Rows + UE_DOUBLE_KINDA_SMALL_NUMBER);
	}

	FSurfaceTrackRibbon Unfolded;
	Unfolded.TrackLength = 30000.0;

	TestEqual(TEXT("A short lap needs one row"), Unfolded.RowCount(), 1);

	const FVector2D Midway = Unfolded.RibbonToAtlas(0.25, 0.75);

	TestTrue(
		TEXT("One row makes the fold the identity, so the unfolded path is not a special case"),
		Midway.Equals(FVector2D(0.25, 0.75), UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackRibbonNormalisedMappingSpansTheBuffer,
	"RacetrackSurface.Ribbon.NormalisedMappingSpansTheBuffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackRibbonNormalisedMappingSpansTheBuffer::RunTest(const FString& Parameters)
{
	FSurfaceTrackRibbon Ribbon;
	Ribbon.TrackLength = 30000.0;
	Ribbon.TrackWidth = 1200.0;

	const FVector2D Start = Ribbon.TrackToNormalised(MakeCoordinate(0.0, 0.0));
	const FVector2D HalfWay = Ribbon.TrackToNormalised(MakeCoordinate(15000.0, 0.0));
	const FVector2D RightEdge = Ribbon.TrackToNormalised(MakeCoordinate(0.0, 1.0));
	const FVector2D LeftEdge = Ribbon.TrackToNormalised(MakeCoordinate(0.0, -1.0));

	TestTrue(TEXT("The start of the lap is the start of the buffer"), Start.Equals(FVector2D(0.0, 0.5), UE_DOUBLE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("Half a lap is half way across the buffer"), HalfWay.Equals(FVector2D(0.5, 0.5), UE_DOUBLE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("The right edge of the track is one edge of the buffer"), RightEdge.Equals(FVector2D(0.0, 1.0), UE_DOUBLE_KINDA_SMALL_NUMBER));
	TestTrue(TEXT("The left edge of the track is the other edge of the buffer"), LeftEdge.Equals(FVector2D(0.0, 0.0), UE_DOUBLE_KINDA_SMALL_NUMBER));

	const FVector2D PastTheLap = Ribbon.TrackToNormalised(MakeCoordinate(37500.0, 0.0));

	TestTrue(TEXT("A distance past the lap wraps into the buffer rather than running off it"),
		PastTheLap.Equals(FVector2D(0.25, 0.5), UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

#endif
