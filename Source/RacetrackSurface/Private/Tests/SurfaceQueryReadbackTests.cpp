#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationSubsystem.h"
#include "SurfaceQueryGather.h"
#include "SurfaceQueryReadback.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeTestCircuit;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;

namespace
{
	constexpr float ReadbackFrameSeconds = 1.0f / 30.0f;

	constexpr double ReadbackPaintedLapFraction = 0.75;

	FSurfaceTrackRibbon MakeReadbackRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	struct FReadbackFixture
	{
		TStrongObjectPtr<USplineComponent> Circuit;
		TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem;
		TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer;
		FSurfaceTrackRibbon Ribbon;
		FSurfaceTrackCoordinate OnTheMark;
	};

	bool BuildPaintedFixture(FAutomationTestBase& Test, UWorld* World, FReadbackFixture& Out)
	{
		Out.Ribbon = MakeReadbackRibbon();
		Out.Circuit = MakeTestCircuit();
		Out.Subsystem = TStrongObjectPtr<USurfaceAccumulationSubsystem>(NewObject<USurfaceAccumulationSubsystem>());
		Out.Subsystem->ConfigureTrackRibbon(Out.Circuit.Get(), Out.Ribbon);

		Out.OnTheMark.DistanceAlongCentreline = Out.Ribbon.TrackLength * ReadbackPaintedLapFraction;
		Out.OnTheMark.LateralOffsetAlpha = 0.0;

		FSurfaceTrackContact Contact;
		Contact.Coordinate = Out.OnTheMark;
		Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
		Contact.SlipAmount = 1.0f;
		Contact.ContactWidth = 20.0f;
		Contact.Channel = EAccumulationChannel::SkidMark;

		TArray<FSurfaceBrushStroke> Strokes;
		Out.Subsystem->BuildStrokes(Contact, ReadbackFrameSeconds, Strokes);

		if (Strokes.Num() == 0)
		{
			Test.AddError(TEXT("The fixture painted no strokes, so there is nothing to read back."));
			return false;
		}

		Out.Buffer = TStrongObjectPtr<USurfaceAccumulationBuffer>(NewObject<USurfaceAccumulationBuffer>());

		if (!Out.Buffer->Allocate(World, Out.Ribbon, RTF_RGBA16f))
		{
			Test.AddError(TEXT("Failed to allocate the accumulation buffer."));
			return false;
		}

		Out.Buffer->PaintStrokes(World, Strokes);
		FlushRenderingCommands();

		TArray<FSurfaceQueryTap> Taps;
		RacetrackSurfaceQuery::BuildTaps(Out.Ribbon, 0, Out.OnTheMark, 0.0, Taps);

		Out.Buffer->GatherTaps(World, Taps, 1);

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReadbackDoesNotResolveInTheCallThatSubmitsIt,
	"RacetrackSurface.Query.ReadbackDoesNotResolveInTheCallThatSubmitsIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReadbackDoesNotResolveInTheCallThatSubmitsIt::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	FReadbackFixture Fixture;

	if (!BuildPaintedFixture(*this, World, Fixture))
	{
		return false;
	}

	const TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Readback =
		MakeShared<FSurfaceQueryReadback, ESPMode::ThreadSafe>(4);

	Readback->Submit(Fixture.Buffer->GetGatherTarget(), 1, 100);

	TArray<FLinearColor> Slots;
	uint64 ResolvedFrame = 0;

	const bool bResolvedImmediately = Readback->CopyResolved(Slots, ResolvedFrame);

	TestFalse(
		TEXT("Submitting does not block until the GPU has the answer, which is the whole point of "
			"the ring: a synchronous read would already have values here"),
		bResolvedImmediately);

	Readback->ReleaseOnRenderThread();
	FlushRenderingCommands();
	Fixture.Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReadbackResolvesTheFrameItSubmitted,
	"RacetrackSurface.Query.ReadbackResolvesTheFrameItSubmitted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReadbackResolvesTheFrameItSubmitted::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	FReadbackFixture Fixture;

	if (!BuildPaintedFixture(*this, World, Fixture))
	{
		return false;
	}

	const TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Readback =
		MakeShared<FSurfaceQueryReadback, ESPMode::ThreadSafe>(4);

	constexpr uint64 SubmittedOnFrame = 4242;

	Readback->Submit(Fixture.Buffer->GetGatherTarget(), 1, SubmittedOnFrame);

	TArray<FLinearColor> Slots;
	uint64 ResolvedFrame = 0;

	constexpr int32 MaximumAttempts = 400;

	int32 Attempts = 0;

	while (Attempts < MaximumAttempts && !Readback->CopyResolved(Slots, ResolvedFrame))
	{
		FPlatformProcess::Sleep(0.001f);
		Readback->Drain();
		FlushRenderingCommands();
		++Attempts;
	}

	AddInfo(FString::Printf(
		TEXT("The gather resolved after %d drain attempts at roughly a millisecond each. A render "
			"thread flush alone never resolves it, because the flush waits for the render thread and "
			"the fence waits for the GPU, so this test polls where production code waits for the next "
			"frame instead"),
		Attempts));

	if (!Readback->CopyResolved(Slots, ResolvedFrame))
	{
		AddError(FString::Printf(
			TEXT("The readback never resolved across %d drain attempts."), MaximumAttempts));
		Readback->ReleaseOnRenderThread();
		FlushRenderingCommands();
		Fixture.Buffer->Release();
		return false;
	}

	TestEqual(TEXT("One slot was submitted and one came back"), Slots.Num(), 1);
	TestEqual(TEXT("The resolved values are stamped with the frame that submitted them"), ResolvedFrame, SubmittedOnFrame);

	const float Average = Slots.Num() > 0 ? Slots[0].G / RacetrackSurfaceQuery::TotalTapWeight : 0.0f;

	const TArray<FLinearColor> Direct = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Fixture.Buffer->GetGatherTarget(), 0, 0, 1, 1, false);

	const float Expected = Direct.Num() == 1 ? Direct[0].G / RacetrackSurfaceQuery::TotalTapWeight : -1.0f;

	AddInfo(FString::Printf(
		TEXT("The ring resolved slot 0 of frame %llu to %.6f in the skid channel, against %.6f read "
			"synchronously off the same gather target"),
		ResolvedFrame,
		Average,
		Expected));

	TestTrue(TEXT("The mark reached the gather target at all"), Average > 0.0f);

	TestTrue(
		FString::Printf(
			TEXT("The asynchronous ring returns the same value a synchronous read does, %.6f against %.6f"),
			Average,
			Expected),
		FMath::Abs(Average - Expected) < 1.0e-6f);

	Readback->ReleaseOnRenderThread();
	FlushRenderingCommands();
	Fixture.Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReadbackLosesNoSubmissionWhenTheRingFills,
	"RacetrackSurface.Query.ReadbackLosesNoSubmissionWhenTheRingFills",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReadbackLosesNoSubmissionWhenTheRingFills::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	FReadbackFixture Fixture;

	if (!BuildPaintedFixture(*this, World, Fixture))
	{
		return false;
	}

	constexpr int32 ShallowRing = 2;
	constexpr int32 Submissions = 16;

	const TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Readback =
		MakeShared<FSurfaceQueryReadback, ESPMode::ThreadSafe>(ShallowRing);

	TestEqual(TEXT("The ring is the depth it was asked for"), Readback->GetRingDepth(), ShallowRing);

	for (int32 Frame = 0; Frame < Submissions; ++Frame)
	{
		Readback->Submit(Fixture.Buffer->GetGatherTarget(), 1, Frame);
	}

	FlushRenderingCommands();

	const int32 Accepted = Readback->GetSubmissionsAccepted();
	const int32 Dropped = Readback->GetSubmissionsDroppedWhileFull();

	AddInfo(FString::Printf(
		TEXT("%d submissions into a ring of %d: %d accepted, %d dropped while full, %d resolves. "
			"The drop rate here is not a production figure: this submits as fast as the render thread "
			"will take them with no frame in between, so the GPU retires nothing and the ring fills "
			"immediately. What it demonstrates is that a full ring refuses loudly, as Nanite's "
			"readback manager does, rather than overwriting work in flight"),
		Submissions,
		ShallowRing,
		Accepted,
		Dropped,
		Readback->GetResolveCount()));

	TestEqual(
		TEXT("Every submission is either accepted or counted as dropped, so a ring that cannot keep up "
			"reports it rather than losing work quietly"),
		Accepted + Dropped,
		Submissions);

	Readback->Drain();
	FlushRenderingCommands();

	TArray<FLinearColor> Slots;
	uint64 ResolvedFrame = 0;

	if (Readback->CopyResolved(Slots, ResolvedFrame))
	{
		TestTrue(
			FString::Printf(
				TEXT("The resolved frame is one that was actually submitted, %llu of %d"),
				ResolvedFrame,
				Submissions),
			ResolvedFrame < static_cast<uint64>(Submissions));
	}

	Readback->ReleaseOnRenderThread();
	FlushRenderingCommands();
	Fixture.Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReadbackResolvedFrameNeverGoesBackwards,
	"RacetrackSurface.Query.ReadbackResolvedFrameNeverGoesBackwards",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReadbackResolvedFrameNeverGoesBackwards::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	FReadbackFixture Fixture;

	if (!BuildPaintedFixture(*this, World, Fixture))
	{
		return false;
	}

	const TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Readback =
		MakeShared<FSurfaceQueryReadback, ESPMode::ThreadSafe>(FSurfaceQueryReadback::GetConfiguredRingDepth());

	uint64 Highest = 0;
	int32 Observations = 0;

	for (int32 Frame = 1; Frame <= 24; ++Frame)
	{
		Readback->Submit(Fixture.Buffer->GetGatherTarget(), 1, Frame);
		FlushRenderingCommands();

		TArray<FLinearColor> Slots;
		uint64 ResolvedFrame = 0;

		if (!Readback->CopyResolved(Slots, ResolvedFrame))
		{
			continue;
		}

		if (ResolvedFrame < Highest)
		{
			AddError(FString::Printf(
				TEXT("The cache went backwards in time, from frame %llu to frame %llu"),
				Highest,
				ResolvedFrame));

			Readback->ReleaseOnRenderThread();
			FlushRenderingCommands();
			Fixture.Buffer->Release();
			return false;
		}

		Highest = ResolvedFrame;
		++Observations;
	}

	AddInfo(FString::Printf(
		TEXT("Twenty four submissions gave %d readable caches, the newest stamped frame %llu, "
			"with %d resolves and %d dropped"),
		Observations,
		Highest,
		Readback->GetResolveCount(),
		Readback->GetSubmissionsDroppedWhileFull()));

	TestTrue(TEXT("The cache became readable at some point during the run"), Observations > 0);

	Readback->ReleaseOnRenderThread();
	FlushRenderingCommands();
	Fixture.Buffer->Release();

	return true;
}

#endif
