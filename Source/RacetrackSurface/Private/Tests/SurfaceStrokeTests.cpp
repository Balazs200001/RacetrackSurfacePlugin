#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr double FrameSeconds = 1.0 / 30.0;
	constexpr double RaceSpeedCentimetresPerSecond = 5500.0;

	TStrongObjectPtr<USurfaceAccumulationSubsystem> MakeSubsystem()
	{
		TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem(NewObject<USurfaceAccumulationSubsystem>());
		Subsystem->ConfigureRegion(RacetrackSurfaceTest::MakeTestRegion());
		return Subsystem;
	}

	FSurfaceContact MakeContact(const FVector& Position, const FVector& Velocity)
	{
		FSurfaceContact Contact;
		Contact.WorldPosition = Position;
		Contact.Velocity = Velocity;
		Contact.SlipAmount = 0.5f;
		Contact.ContactWidth = 20.0f;
		Contact.Channel = EAccumulationChannel::SkidMark;
		return Contact;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeSweepsBackAlongVelocity,
	"RacetrackSurface.Stroke.SweepsBackAlongVelocity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeSweepsBackAlongVelocity::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();
	const FSurfaceRegion& Region = Subsystem->GetRegion();

	const FVector Velocity(RaceSpeedCentimetresPerSecond, 0.0, 0.0);
	const FSurfaceContact Contact = MakeContact(FVector::ZeroVector, Velocity);

	FSurfaceBrushStroke Stroke;
	TestTrue(TEXT("Contact inside the region produces a stroke"), Subsystem->BuildStroke(Contact, FrameSeconds, Stroke));

	const double ExpectedTravel = RaceSpeedCentimetresPerSecond * FrameSeconds;
	const double ActualTravel = (Stroke.RegionEnd - Stroke.RegionStart).Size() * Region.WorldSize;

	TestTrue(
		TEXT("Stroke spans the distance travelled during the frame"),
		FMath::IsNearlyEqual(ActualTravel, ExpectedTravel, 1.0));

	TestTrue(
		TEXT("Stroke ends at the reported contact position"),
		Stroke.RegionEnd.Equals(Region.WorldToNormalised(FVector::ZeroVector), UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeLeavesNoGapBetweenFrames,
	"RacetrackSurface.Stroke.LeavesNoGapBetweenFrames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeLeavesNoGapBetweenFrames::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();

	const FVector Velocity(RaceSpeedCentimetresPerSecond, 0.0, 0.0);
	const FVector FirstPosition = FVector::ZeroVector;
	const FVector SecondPosition = FirstPosition + Velocity * FrameSeconds;

	FSurfaceBrushStroke FirstStroke;
	FSurfaceBrushStroke SecondStroke;

	TestTrue(TEXT("First frame produces a stroke"), Subsystem->BuildStroke(MakeContact(FirstPosition, Velocity), FrameSeconds, FirstStroke));
	TestTrue(TEXT("Second frame produces a stroke"), Subsystem->BuildStroke(MakeContact(SecondPosition, Velocity), FrameSeconds, SecondStroke));

	TestTrue(
		TEXT("Consecutive strokes abut exactly, leaving no dotted gap at speed"),
		SecondStroke.RegionStart.Equals(FirstStroke.RegionEnd, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeLengthScalesWithTimeCompression,
	"RacetrackSurface.Stroke.LengthScalesWithTimeCompression",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeLengthScalesWithTimeCompression::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();

	const FVector Velocity(RaceSpeedCentimetresPerSecond, 0.0, 0.0);
	const FSurfaceContact Contact = MakeContact(FVector::ZeroVector, Velocity);

	FSurfaceBrushStroke RealTimeStroke;
	FSurfaceBrushStroke CompressedStroke;

	Subsystem->BuildStroke(Contact, FrameSeconds, RealTimeStroke);
	Subsystem->BuildStroke(Contact, FrameSeconds * 20.0, CompressedStroke);

	const double RealTimeLength = (RealTimeStroke.RegionEnd - RealTimeStroke.RegionStart).Size();
	const double CompressedLength = (CompressedStroke.RegionEnd - CompressedStroke.RegionStart).Size();

	TestTrue(
		TEXT("Twenty times the delta produces twenty times the stroke, so compression stays continuous"),
		FMath::IsNearlyEqual(CompressedLength, RealTimeLength * 20.0, UE_DOUBLE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeRejectsContactsOutsideRegion,
	"RacetrackSurface.Stroke.RejectsContactsOutsideRegion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeRejectsContactsOutsideRegion::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();

	const FSurfaceContact FarAway = MakeContact(FVector(500000.0, 500000.0, 0.0), FVector(1.0, 0.0, 0.0));

	FSurfaceBrushStroke Stroke;
	TestFalse(TEXT("A contact well outside the region is discarded"), Subsystem->BuildStroke(FarAway, FrameSeconds, Stroke));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeKeepsContactsCrossingTheEdge,
	"RacetrackSurface.Stroke.KeepsContactsCrossingTheEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeKeepsContactsCrossingTheEdge::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();

	const FVector Velocity(RaceSpeedCentimetresPerSecond, 0.0, 0.0);
	const FSurfaceContact LeavingRegion = MakeContact(FVector(10050.0, 0.0, 0.0), Velocity);

	FSurfaceBrushStroke Stroke;
	TestTrue(
		TEXT("A stroke starting inside and ending outside is kept"),
		Subsystem->BuildStroke(LeavingRegion, FrameSeconds, Stroke));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStrokeConvertsWidthAndIntensity,
	"RacetrackSurface.Stroke.ConvertsWidthAndIntensity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStrokeConvertsWidthAndIntensity::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();
	const FSurfaceRegion& Region = Subsystem->GetRegion();

	FSurfaceContact Contact = MakeContact(FVector::ZeroVector, FVector(1.0, 0.0, 0.0));
	Contact.ContactWidth = 20.0f;
	Contact.SlipAmount = 1.7f;

	FSurfaceBrushStroke Stroke;
	Subsystem->BuildStroke(Contact, FrameSeconds, Stroke);

	const float ExpectedWidthInTexels = static_cast<float>(20.0 / Region.CentimetresPerTexel());

	TestTrue(
		TEXT("Contact width converts from centimetres to texels"),
		FMath::IsNearlyEqual(Stroke.WidthInTexels, ExpectedWidthInTexels, UE_KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("Slip amount above one is clamped"),
		FMath::IsNearlyEqual(Stroke.Intensity, 1.0f, UE_KINDA_SMALL_NUMBER));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueueDrainsOnFlush,
	"RacetrackSurface.Queue.DrainsOnFlush",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueueDrainsOnFlush::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem = MakeSubsystem();

	Subsystem->RegisterSurfaceContact(FVector::ZeroVector, FVector(1.0, 0.0, 0.0), 0.5f, 20.0f, EAccumulationChannel::SkidMark);
	Subsystem->RegisterSurfaceContact(FVector(100.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0), 0.5f, 20.0f, EAccumulationChannel::RubberBuildup);

	TestEqual(TEXT("Both contacts are queued"), Subsystem->GetPendingContactCount(), 2);

	Subsystem->ResetSurface();

	TestEqual(TEXT("Reset empties the queue"), Subsystem->GetPendingContactCount(), 0);

	return true;
}

#endif
