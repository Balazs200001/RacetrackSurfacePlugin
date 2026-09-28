#include "Misc/AutomationTest.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "SurfaceTrackSpace.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::HarnessTrackWidth;
using RacetrackSurfaceTest::MakeTestCircuit;
using RacetrackSurfaceTest::MakeTestRibbon;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;

namespace
{
	constexpr float FrameSeconds = 1.0f / 30.0f;

	struct FHarness
	{
		TStrongObjectPtr<USplineComponent> Circuit;
		TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem;
		FSurfaceTrackRibbon Ribbon;

		double Length() const
		{
			return Ribbon.TrackLength;
		}
	};

	FHarness MakeHarness()
	{
		FHarness Harness;
		Harness.Circuit = MakeTestCircuit();
		Harness.Ribbon = MakeTestRibbon(*Harness.Circuit);
		Harness.Subsystem = TStrongObjectPtr<USurfaceAccumulationSubsystem>(
			NewObject<USurfaceAccumulationSubsystem>());
		Harness.Subsystem->ConfigureTrackRibbon(Harness.Circuit.Get(), Harness.Ribbon);
		return Harness;
	}

	FHarness MakeFoldedHarness()
	{
		FHarness Harness;
		Harness.Circuit = MakeTestCircuit();
		Harness.Ribbon.TrackLength = 180000.0;
		Harness.Ribbon.TrackWidth = 800.0;
		Harness.Ribbon.CentimetresPerTexel = 3.125;
		Harness.Ribbon.bClosedLoop = true;
		Harness.Subsystem = TStrongObjectPtr<USurfaceAccumulationSubsystem>(
			NewObject<USurfaceAccumulationSubsystem>());
		Harness.Subsystem->ConfigureTrackRibbon(Harness.Circuit.Get(), Harness.Ribbon);
		return Harness;
	}

	FSurfaceTrackContact MakeTrackContact(double Distance, double Alpha, double Speed)
	{
		FSurfaceTrackContact Contact;
		Contact.Coordinate.DistanceAlongCentreline = Distance;
		Contact.Coordinate.LateralOffsetAlpha = Alpha;
		Contact.CentimetresPerSecondAlongCentreline = Speed;
		Contact.SlipAmount = 0.5f;
		Contact.ContactWidth = 20.0f;
		return Contact;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeSweepsBackAlongTravel,
	"RacetrackSurface.Stroke.SweepsBackAlongTravel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeSweepsBackAlongTravel::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();
	const double Distance = Harness.Length() * 0.5;

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(Distance, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, Strokes);

	TestEqual(TEXT("A contact away from the seam produces exactly one stroke"), Strokes.Num(), 1);

	if (Strokes.Num() != 1)
	{
		return false;
	}

	const double ExpectedTravel = RaceSpeedCentimetresPerSecond * FrameSeconds;
	const double ActualTravel = (Strokes[0].RegionEnd.X - Strokes[0].RegionStart.X) * Harness.Length();

	TestTrue(
		FString::Printf(TEXT("Stroke spans the distance travelled during the frame, got %.2f cm"), ActualTravel),
		FMath::IsNearlyEqual(ActualTravel, ExpectedTravel, 1.0));

	TestTrue(
		TEXT("Stroke ends at the reported contact position"),
		FMath::IsNearlyEqual(Strokes[0].RegionEnd.X, Distance / Harness.Length(), UE_DOUBLE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("A contact on the centreline sits half way across the ribbon"),
		FMath::IsNearlyEqual(Strokes[0].RegionEnd.Y, 0.5, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeLeavesNoGapBetweenFrames,
	"RacetrackSurface.Stroke.LeavesNoGapBetweenFrames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeLeavesNoGapBetweenFrames::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();

	const double Travel = RaceSpeedCentimetresPerSecond * FrameSeconds;
	const double FirstDistance = Harness.Length() * 0.25;

	TArray<FSurfaceBrushStroke> FirstStrokes;
	TArray<FSurfaceBrushStroke> SecondStrokes;

	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(FirstDistance, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, FirstStrokes);
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(FirstDistance + Travel, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, SecondStrokes);

	TestEqual(TEXT("First frame produces one stroke"), FirstStrokes.Num(), 1);
	TestEqual(TEXT("Second frame produces one stroke"), SecondStrokes.Num(), 1);

	if (FirstStrokes.Num() != 1 || SecondStrokes.Num() != 1)
	{
		return false;
	}

	TestTrue(
		TEXT("Consecutive strokes abut exactly, leaving no dotted gap at speed"),
		SecondStrokes[0].RegionStart.Equals(FirstStrokes[0].RegionEnd, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeLengthScalesWithTimeCompression,
	"RacetrackSurface.Stroke.LengthScalesWithTimeCompression",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeLengthScalesWithTimeCompression::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();
	const FSurfaceTrackContact Contact =
		MakeTrackContact(Harness.Length() * 0.5, 0.0, RaceSpeedCentimetresPerSecond);

	TArray<FSurfaceBrushStroke> RealTime;
	TArray<FSurfaceBrushStroke> Compressed;

	Harness.Subsystem->BuildStrokes(Contact, FrameSeconds, RealTime);
	Harness.Subsystem->BuildStrokes(Contact, FrameSeconds * 20.0f, Compressed);

	if (RealTime.Num() != 1 || Compressed.Num() != 1)
	{
		AddError(TEXT("Expected a single stroke from each delta."));
		return false;
	}

	const double RealTimeLength = RealTime[0].RegionEnd.X - RealTime[0].RegionStart.X;
	const double CompressedLength = Compressed[0].RegionEnd.X - Compressed[0].RegionStart.X;

	TestTrue(
		TEXT("Twenty times the delta produces twenty times the stroke, so compression stays continuous"),
		FMath::IsNearlyEqual(CompressedLength, RealTimeLength * 20.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeSplitsAtTheLapSeam,
	"RacetrackSurface.Stroke.SplitsAtTheLapSeam",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeSplitsAtTheLapSeam::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();
	const double Length = Harness.Length();

	const double Travel = RaceSpeedCentimetresPerSecond * FrameSeconds;
	const double PastTheSeam = Travel * 0.25;

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(PastTheSeam, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, Strokes);

	TestEqual(TEXT("A contact straddling the lap seam produces two strokes, not one"), Strokes.Num(), 2);

	if (Strokes.Num() != 2)
	{
		return false;
	}

	TestTrue(
		TEXT("The first stroke runs up to the far end of the ribbon"),
		FMath::IsNearlyEqual(Strokes[0].RegionEnd.X, 1.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("The second stroke starts at the near end of the ribbon"),
		FMath::IsNearlyEqual(Strokes[1].RegionStart.X, 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	const double FirstLength = (Strokes[0].RegionEnd.X - Strokes[0].RegionStart.X) * Length;
	const double SecondLength = (Strokes[1].RegionEnd.X - Strokes[1].RegionStart.X) * Length;

	TestTrue(
		FString::Printf(TEXT("The two pieces together span the frame's travel, got %.2f + %.2f cm against %.2f"),
			FirstLength, SecondLength, Travel),
		FMath::IsNearlyEqual(FirstLength + SecondLength, Travel, 1.0));

	TestTrue(
		FString::Printf(TEXT("Neither piece smears across the ribbon, longest is %.2f cm of a %.0f cm lap"),
			FMath::Max(FirstLength, SecondLength), Length),
		FMath::Max(FirstLength, SecondLength) < Travel + 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeSplitsAtRowBoundaries,
	"RacetrackSurface.Stroke.SplitsAtRowBoundaries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeSplitsAtRowBoundaries::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeFoldedHarness();
	const double RowLength = Harness.Ribbon.RowLength();

	TestEqual(TEXT("The folded harness has four rows"), Harness.Ribbon.RowCount(), 4);

	const double Travel = RaceSpeedCentimetresPerSecond * FrameSeconds;
	const double PastTheBoundary = RowLength + Travel * 0.25;

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(PastTheBoundary, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, Strokes);

	TestEqual(TEXT("A contact straddling a row boundary produces two strokes, not one"), Strokes.Num(), 2);

	if (Strokes.Num() != 2)
	{
		return false;
	}

	TestEqual(TEXT("The first piece stays in the row it started in"), Strokes[0].RowIndex, 0);
	TestEqual(TEXT("The second piece lands in the next row down"), Strokes[1].RowIndex, 1);

	TestTrue(
		TEXT("The first piece runs to the right edge of its row"),
		FMath::IsNearlyEqual(Strokes[0].RegionEnd.X, 1.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("The second piece starts at the left edge of its row"),
		FMath::IsNearlyEqual(Strokes[1].RegionStart.X, 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	const double FirstLength = (Strokes[0].RegionEnd.X - Strokes[0].RegionStart.X) * RowLength;
	const double SecondLength = (Strokes[1].RegionEnd.X - Strokes[1].RegionStart.X) * RowLength;

	TestTrue(
		FString::Printf(TEXT("The two pieces together span the frame's travel, got %.2f + %.2f cm against %.2f"),
			FirstLength, SecondLength, Travel),
		FMath::IsNearlyEqual(FirstLength + SecondLength, Travel, 1.0));

	TestTrue(
		TEXT("Neither piece leaves its own band of the atlas"),
		Strokes[0].RegionStart.Y < 0.25 && Strokes[1].RegionStart.Y >= 0.25 && Strokes[1].RegionStart.Y < 0.5);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeSplitsWhereTheLastRowMeetsTheFirst,
	"RacetrackSurface.Stroke.SplitsWhereTheLastRowMeetsTheFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeSplitsWhereTheLastRowMeetsTheFirst::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeFoldedHarness();
	const double RowLength = Harness.Ribbon.RowLength();
	const int32 Rows = Harness.Ribbon.RowCount();

	const double Travel = RaceSpeedCentimetresPerSecond * FrameSeconds;

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(Travel * 0.25, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, Strokes);

	TestEqual(TEXT("The lap seam is a row boundary and splits the same way"), Strokes.Num(), 2);

	if (Strokes.Num() != 2)
	{
		return false;
	}

	TestEqual(TEXT("The piece before the seam belongs to the last row"), Strokes[0].RowIndex, Rows - 1);
	TestEqual(TEXT("The piece after the seam belongs to the first row"), Strokes[1].RowIndex, 0);

	TestTrue(
		TEXT("The last row runs out at its right edge rather than wrapping inside itself"),
		FMath::IsNearlyEqual(Strokes[0].RegionEnd.X, 1.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("The first row picks up at its left edge"),
		FMath::IsNearlyEqual(Strokes[1].RegionStart.X, 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	const double FirstLength = (Strokes[0].RegionEnd.X - Strokes[0].RegionStart.X) * RowLength;
	const double SecondLength = (Strokes[1].RegionEnd.X - Strokes[1].RegionStart.X) * RowLength;

	TestTrue(
		FString::Printf(TEXT("The two pieces together span the frame's travel, got %.2f + %.2f cm against %.2f"),
			FirstLength, SecondLength, Travel),
		FMath::IsNearlyEqual(FirstLength + SecondLength, Travel, 1.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeStaysWholeInsideARow,
	"RacetrackSurface.Stroke.StaysWholeInsideARow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeStaysWholeInsideARow::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeFoldedHarness();
	const double RowLength = Harness.Ribbon.RowLength();

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(RowLength * 2.5, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, Strokes);

	TestEqual(TEXT("A contact well inside a row is not split"), Strokes.Num(), 1);

	if (Strokes.Num() != 1)
	{
		return false;
	}

	TestEqual(TEXT("A contact half way along the third row reports the third row"), Strokes[0].RowIndex, 2);

	const double Length = (Strokes[0].RegionEnd.X - Strokes[0].RegionStart.X) * RowLength;
	const double Travel = RaceSpeedCentimetresPerSecond * FrameSeconds;

	TestTrue(
		FString::Printf(TEXT("The unsplit stroke spans the frame's travel, got %.2f cm against %.2f"), Length, Travel),
		FMath::IsNearlyEqual(Length, Travel, 1.0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeDoesNotSplitAnOpenTrack,
	"RacetrackSurface.Stroke.DoesNotSplitAnOpenTrack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeDoesNotSplitAnOpenTrack::RunTest(const FString& Parameters)
{
	FHarness Harness = MakeHarness();
	Harness.Ribbon.bClosedLoop = false;
	Harness.Subsystem->ConfigureTrackRibbon(Harness.Circuit.Get(), Harness.Ribbon);

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(10.0, 0.0, RaceSpeedCentimetresPerSecond), FrameSeconds, Strokes);

	TestEqual(TEXT("An open track clamps at its start rather than wrapping into two strokes"), Strokes.Num(), 1);

	if (Strokes.Num() == 1)
	{
		TestTrue(
			TEXT("The clamped stroke starts at the beginning of the ribbon"),
			FMath::IsNearlyEqual(Strokes[0].RegionStart.X, 0.0, UE_DOUBLE_KINDA_SMALL_NUMBER));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeRejectsContactsOffTheTrack,
	"RacetrackSurface.Stroke.RejectsContactsOffTheTrack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeRejectsContactsOffTheTrack::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();

	TestFalse(
		TEXT("A track-space contact beyond the track edge is rejected"),
		Harness.Subsystem->RegisterSurfaceContactInTrackSpace(
			Harness.Length() * 0.5, 1.5, RaceSpeedCentimetresPerSecond, 0.5f, 20.0f));

	TestFalse(
		TEXT("A world-space contact far from the track is rejected"),
		Harness.Subsystem->RegisterSurfaceContact(
			FVector(500000.0, 500000.0, 0.0), FVector(1.0, 0.0, 0.0), 0.5f, 20.0f));

	TestEqual(TEXT("Both rejections are counted"), Harness.Subsystem->GetContactsDiscardedOutsideRibbon(), 2);
	TestEqual(TEXT("Nothing was queued"), Harness.Subsystem->GetPendingContactCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeKeepsContactsCrossingTheLateralEdge,
	"RacetrackSurface.Stroke.KeepsContactsCrossingTheLateralEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeKeepsContactsCrossingTheLateralEdge::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();

	FSurfaceTrackContact Contact =
		MakeTrackContact(Harness.Length() * 0.5, 0.95, RaceSpeedCentimetresPerSecond);
	Contact.LateralOffsetAlphaPerSecond = -3.0;

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(Contact, FrameSeconds, Strokes);

	TestEqual(TEXT("A stroke that starts off the edge and ends on the track is kept"), Strokes.Num(), 1);

	if (Strokes.Num() == 1)
	{
		TestTrue(
			FString::Printf(TEXT("The off-track end is left outside the buffer for the rasteriser to clip, got %.4f"),
				Strokes[0].RegionStart.Y),
			Strokes[0].RegionStart.Y > 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeConvertsWidthAndIntensity,
	"RacetrackSurface.Stroke.ConvertsWidthAndIntensity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeConvertsWidthAndIntensity::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();

	FSurfaceTrackContact Contact = MakeTrackContact(Harness.Length() * 0.5, 0.0, 1.0);
	Contact.ContactWidth = 20.0f;
	Contact.SlipAmount = 1.7f;

	TArray<FSurfaceBrushStroke> Strokes;
	Harness.Subsystem->BuildStrokes(Contact, FrameSeconds, Strokes);

	if (Strokes.Num() != 1)
	{
		AddError(TEXT("Expected a single stroke."));
		return false;
	}

	const float ExpectedWidthInTexels =
		static_cast<float>(20.0 / Harness.Ribbon.CentimetresPerTexel);

	TestTrue(
		FString::Printf(TEXT("Contact width converts from centimetres to texels, got %.4f"), Strokes[0].WidthInTexels),
		FMath::IsNearlyEqual(Strokes[0].WidthInTexels, ExpectedWidthInTexels, UE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("Slip amount above one is clamped before it becomes exposure"),
		FMath::IsNearlyEqual(
			Strokes[0].Intensity, RacetrackSurfaceMapping::ExposurePerUnitSlip, UE_KINDA_SMALL_NUMBER));

	const int32 ContactsToSaturation = FMath::CeilToInt32(1.0f / RacetrackSurfaceMapping::ExposurePerUnitSlip);
	const float CoverageAtSaturation =
		RacetrackSurfaceMapping::ExposureToCoverage(ContactsToSaturation * RacetrackSurfaceMapping::ExposurePerUnitSlip);

	AddInfo(FString::Printf(
		TEXT("One full-slip contact deposits %.5f exposure, which is %.4f coverage. %d contacts reach %.4f."),
		Strokes[0].Intensity,
		RacetrackSurfaceMapping::ExposureToCoverage(Strokes[0].Intensity),
		ContactsToSaturation,
		CoverageAtSaturation));

	TestTrue(
		TEXT("One contact leaves a faint mark rather than a saturated one, so buildup takes a session"),
		RacetrackSurfaceMapping::ExposureToCoverage(Strokes[0].Intensity) < 0.01f);

	TestTrue(
		TEXT("Roughly five hundred full-slip contacts approach the ceiling, as the precision floor derived"),
		CoverageAtSaturation > 0.9f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeWorldAndTrackSpacePathsAgree,
	"RacetrackSurface.Stroke.WorldAndTrackSpacePathsAgree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeWorldAndTrackSpacePathsAgree::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();

	const double Distance = Harness.Length() * 0.4;
	const double Alpha = 0.3;

	FSurfaceTrackCoordinate Coordinate;
	Coordinate.DistanceAlongCentreline = Distance;
	Coordinate.LateralOffsetAlpha = Alpha;

	const FVector WorldPosition =
		RacetrackSurfaceTrackSpace::TrackToWorld(*Harness.Circuit, HarnessTrackWidth, Coordinate);
	const FVector WorldDirection = Harness.Circuit->GetDirectionAtDistanceAlongSpline(
		static_cast<float>(Distance), ESplineCoordinateSpace::World);

	TestTrue(
		TEXT("The world-space contact is accepted"),
		Harness.Subsystem->RegisterSurfaceContact(
			WorldPosition,
			WorldDirection * RaceSpeedCentimetresPerSecond,
			0.5f,
			20.0f));

	if (Harness.Subsystem->GetPendingContactCount() != 1)
	{
		AddError(TEXT("The world-space path queued nothing."));
		return false;
	}

	const FSurfaceTrackContact Converted = Harness.Subsystem->GetPendingContacts()[0];

	AddInfo(FString::Printf(
		TEXT("World path resolved to distance %.2f cm, alpha %.5f, %.2f cm/s along, %.6f alpha/s across"),
		Converted.Coordinate.DistanceAlongCentreline,
		Converted.Coordinate.LateralOffsetAlpha,
		Converted.CentimetresPerSecondAlongCentreline,
		Converted.LateralOffsetAlphaPerSecond));

	TestTrue(
		FString::Printf(TEXT("The world path recovers the distance, got %.2f against %.2f"),
			Converted.Coordinate.DistanceAlongCentreline, Distance),
		FMath::Abs(Converted.Coordinate.DistanceAlongCentreline - Distance) < 1.0);

	TestTrue(
		FString::Printf(TEXT("The world path recovers the lateral offset, got %.5f against %.5f"),
			Converted.Coordinate.LateralOffsetAlpha, Alpha),
		FMath::Abs(Converted.Coordinate.LateralOffsetAlpha - Alpha) < 0.001);

	TestTrue(
		FString::Printf(TEXT("The world path recovers the speed along the centreline, got %.2f against %.2f"),
			Converted.CentimetresPerSecondAlongCentreline, RaceSpeedCentimetresPerSecond),
		FMath::Abs(Converted.CentimetresPerSecondAlongCentreline - RaceSpeedCentimetresPerSecond) < 1.0);

	TArray<FSurfaceBrushStroke> FromWorld;
	TArray<FSurfaceBrushStroke> FromTrack;

	Harness.Subsystem->BuildStrokes(Converted, FrameSeconds, FromWorld);
	Harness.Subsystem->BuildStrokes(
		MakeTrackContact(Distance, Alpha, RaceSpeedCentimetresPerSecond), FrameSeconds, FromTrack);

	if (FromWorld.Num() != 1 || FromTrack.Num() != 1)
	{
		AddError(TEXT("Expected one stroke from each path."));
		return false;
	}

	TestTrue(
		TEXT("Both write paths place the stroke in the same place on the ribbon"),
		FromWorld[0].RegionStart.Equals(FromTrack[0].RegionStart, 1.0e-4)
		&& FromWorld[0].RegionEnd.Equals(FromTrack[0].RegionEnd, 1.0e-4));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueueDrainsOnFlush,
	"RacetrackSurface.Queue.DrainsOnFlush",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueueDrainsOnFlush::RunTest(const FString& Parameters)
{
	const FHarness Harness = MakeHarness();

	Harness.Subsystem->RegisterSurfaceContactInTrackSpace(
		0.0, 0.0, RaceSpeedCentimetresPerSecond, 0.5f, 20.0f);
	Harness.Subsystem->RegisterSurfaceContactInTrackSpace(
		100.0, 0.5, RaceSpeedCentimetresPerSecond, 0.5f, 20.0f);

	TestEqual(TEXT("Both contacts are queued"), Harness.Subsystem->GetPendingContactCount(), 2);

	Harness.Subsystem->ResetSurface();

	TestEqual(TEXT("Reset empties the queue"), Harness.Subsystem->GetPendingContactCount(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceContactsNeedACentreline,
	"RacetrackSurface.Queue.WorldContactsNeedACentreline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceContactsNeedACentreline::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem(
		NewObject<USurfaceAccumulationSubsystem>());

	TestFalse(
		TEXT("A world-space contact without a centreline is rejected rather than queued"),
		Subsystem->RegisterSurfaceContact(
			FVector::ZeroVector, FVector(1.0, 0.0, 0.0), 0.5f, 20.0f));

	TestEqual(TEXT("The rejection is counted separately from an off-track one"),
		Subsystem->GetContactsDiscardedWithoutCentreline(), 1);
	TestEqual(TEXT("Nothing was queued"), Subsystem->GetPendingContactCount(), 0);

	TestTrue(
		TEXT("A track-space contact still works without a centreline, because it needs no conversion"),
		Subsystem->RegisterSurfaceContactInTrackSpace(
			100.0, 0.0, RaceSpeedCentimetresPerSecond, 0.5f, 20.0f));

	return true;
}

#endif
