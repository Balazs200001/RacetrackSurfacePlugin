#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationSubsystem.h"
#include "SurfaceQueryGather.h"
#include "SurfaceQueryReadback.h"
#include "SurfaceTrackSpace.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"
#include "Tests/SurfaceCpuTiming.h"
#include "Tests/SurfaceGpuTiming.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeClosedCircleSpline;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;
using RacetrackSurfaceTest::SampleCpuWork;
using RacetrackSurfaceTest::SampleGpuWork;
using RacetrackSurfaceTrackSpace::TrackToWorld;
using RacetrackSurfaceTrackSpace::WorldToTrack;

namespace
{
	constexpr double DemoLapCentimetres = 62500.0;

	constexpr double DemoTrackWidth = 800.0;

	constexpr double QueryFrameBudgetMilliseconds = 1000.0 / 60.0;

	constexpr double DecayPassMillisecondsAtTheDemoAtlas = 0.3340;

	constexpr int32 DemoQueryPointCount = 4;

	constexpr int32 StressQueryPointCount = 80;

	const int32 QueryPointCandidates[] = { DemoQueryPointCount, StressQueryPointCount };

	constexpr int32 DemoCircuitSplinePoints = 64;

	FSurfaceTrackRibbon MakeTimingRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = DemoLapCentimetres;
		Ribbon.TrackWidth = DemoTrackWidth;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackCoordinate SpreadCoordinate(const FSurfaceTrackRibbon& Ribbon, int32 Index, int32 Count)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = Ribbon.TrackLength * (Index + 0.5) / Count;
		Coordinate.LateralOffsetAlpha = -0.8 + 1.6 * (Index % 5) / 4.0;
		return Coordinate;
	}

	void BuildSpreadTaps(
		const FSurfaceTrackRibbon& Ribbon,
		int32 PointCount,
		TArray<FSurfaceQueryTap>& OutTaps)
	{
		OutTaps.Reset();

		for (int32 Index = 0; Index < PointCount; ++Index)
		{
			RacetrackSurfaceQuery::BuildTaps(
				Ribbon,
				Index,
				SpreadCoordinate(Ribbon, Index, PointCount),
				RacetrackSurfaceQuery::DefaultContactPatchWidth,
				OutTaps);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryCostAndLatency,
	"RacetrackSurface.Performance.QueryCostAndLatency",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryCostAndLatency::RunTest(const FString& Parameters)
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

	const FSurfaceTrackRibbon Ribbon = MakeTimingRibbon();

	const TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer for the timing run."));
		return false;
	}

	const TStrongObjectPtr<USplineComponent> Circuit =
		MakeClosedCircleSpline(DemoLapCentimetres / (2.0 * UE_DOUBLE_PI), DemoCircuitSplinePoints);

	AddInfo(FString::Printf(
		TEXT("Ribbon %.0f cm lap, %.0f cm wide, %.4f cm per texel, %d rows, atlas %d x %d; circuit of "
			"%d spline points"),
		Ribbon.TrackLength,
		Ribbon.TrackWidth,
		Ribbon.CentimetresPerTexel,
		Ribbon.RowCount(),
		Ribbon.AtlasWidth(),
		Ribbon.AtlasHeight(),
		DemoCircuitSplinePoints));

	AddInfo(TEXT("Points | Gather GPU median | minimum | per point | BuildTaps CPU median | "
		"WorldToTrack CPU median | per point"));

	for (const int32 PointCount : QueryPointCandidates)
	{
		TArray<FSurfaceQueryTap> Taps;
		BuildSpreadTaps(Ribbon, PointCount, Taps);

		TestEqual(
			FString::Printf(TEXT("%d query points build %d taps each"),
				PointCount,
				RacetrackSurfaceQuery::TapsPerQueryPoint),
			Taps.Num(),
			PointCount * RacetrackSurfaceQuery::TapsPerQueryPoint);

		if (Buffer->EnsureGatherTarget(World, PointCount) == nullptr)
		{
			AddError(FString::Printf(TEXT("No gather target for %d slots"), PointCount));
			continue;
		}

		double GatherMedian = 0.0;
		double GatherMinimum = 0.0;

		if (!SampleGpuWork(
			[&]()
			{
				Buffer->GatherTaps(World, Taps, PointCount);
			},
			GatherMedian,
			GatherMinimum))
		{
			AddWarning(FString::Printf(
				TEXT("The GPU timestamps would not resolve at %d points, so no gather cost was taken"),
				PointCount));

			continue;
		}

		TArray<FSurfaceQueryTap> Scratch;
		double BuildMedian = 0.0;
		double BuildMinimum = 0.0;

		SampleCpuWork(
			[&]()
			{
				BuildSpreadTaps(Ribbon, PointCount, Scratch);
			},
			BuildMedian,
			BuildMinimum);

		TArray<FVector> WorldPositions;
		WorldPositions.Reserve(PointCount);

		for (int32 Index = 0; Index < PointCount; ++Index)
		{
			WorldPositions.Add(TrackToWorld(*Circuit, Ribbon.TrackWidth, SpreadCoordinate(Ribbon, Index, PointCount)));
		}

		double ConversionAccumulator = 0.0;
		double ConversionMedian = 0.0;
		double ConversionMinimum = 0.0;

		SampleCpuWork(
			[&]()
			{
				for (const FVector& Position : WorldPositions)
				{
					FSurfaceTrackCoordinate Coordinate;
					WorldToTrack(*Circuit, Ribbon.TrackWidth, Position, Coordinate);
					ConversionAccumulator += Coordinate.DistanceAlongCentreline;
				}
			},
			ConversionMedian,
			ConversionMinimum);

		TestTrue(TEXT("The timed conversion was not optimised away"), ConversionAccumulator != 0.0);

		AddInfo(FString::Printf(
			TEXT("%d | %.4f ms | %.4f ms | %.5f ms | %.4f ms | %.4f ms | %.5f ms"),
			PointCount,
			GatherMedian,
			GatherMinimum,
			GatherMedian / PointCount,
			BuildMedian,
			ConversionMedian,
			ConversionMedian / PointCount));

		if (PointCount == DemoQueryPointCount)
		{
			AddInfo(FString::Printf(
				TEXT("At the demo's four points the whole query costs %.4f ms on the GPU and %.4f ms on "
					"the CPU, against the %.4f ms decay pass already in the frame and %.2f ms of a 60 Hz "
					"frame"),
				GatherMedian,
				BuildMedian + ConversionMedian,
				DecayPassMillisecondsAtTheDemoAtlas,
				QueryFrameBudgetMilliseconds));
		}

		if (PointCount == StressQueryPointCount)
		{
			AddInfo(FString::Printf(
				TEXT("Eighty points is the stress case, not the design target. It is what twenty cars "
					"reading four wheels each would cost, and R2 wrote its segment cache trigger at "
					"exactly that number: %.4f ms of world to track conversion here"),
				ConversionMedian));
		}
	}

	Buffer->Release();

	return true;
}

namespace
{
	const int32 RingDepthCandidates[] = { 1, 2, 3, 4, 8 };

	constexpr int32 FramesPerRingDepth = 30;

	constexpr double RingSweepPaintedLapFraction = 0.70;

	struct FRingDepthScenario
	{
		TStrongObjectPtr<USplineComponent> Circuit;

		TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem;

		TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer;

		FSurfaceTrackRibbon Ribbon;

		TSharedPtr<FSurfaceQueryReadback, ESPMode::ThreadSafe> Ring;

		int32 DepthIndex = 0;

		uint64 Frame = 0;

		int32 FramesAtThisDepth = 0;

		uint64 HighestResolved = 0;

		TArray<uint64> ResolveGaps;

		int32 DropsAtTheShallowestDepth = 0;

		int32 DropsAtTheDeepestDepth = 0;

		void ReleaseRing()
		{
			if (Ring.IsValid())
			{
				Ring->ReleaseOnRenderThread();
				Ring.Reset();
			}
		}

		void Finish()
		{
			ReleaseRing();

			FlushRenderingCommands();

			if (Buffer.IsValid())
			{
				Buffer->Release();
			}

			Buffer.Reset();
			Subsystem.Reset();
			Circuit.Reset();
		}
	};

	bool BuildRingSweepFixture(FAutomationTestBase& Test, UWorld* World, FRingDepthScenario& Out)
	{
		Out.Ribbon = MakeTimingRibbon();
		Out.Circuit = MakeClosedCircleSpline(DemoLapCentimetres / (2.0 * UE_DOUBLE_PI), DemoCircuitSplinePoints);
		Out.Subsystem = TStrongObjectPtr<USurfaceAccumulationSubsystem>(NewObject<USurfaceAccumulationSubsystem>());
		Out.Subsystem->ConfigureTrackRibbon(Out.Circuit.Get(), Out.Ribbon);

		FSurfaceTrackCoordinate Mark;
		Mark.DistanceAlongCentreline = Out.Ribbon.TrackLength * RingSweepPaintedLapFraction;
		Mark.LateralOffsetAlpha = 0.0;

		FSurfaceTrackContact Contact;
		Contact.Coordinate = Mark;
		Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
		Contact.SlipAmount = 1.0f;
		Contact.ContactWidth = 20.0f;

		TArray<FSurfaceBrushStroke> Strokes;
		Out.Subsystem->BuildStrokes(Contact, 1.0f / 60.0f, Strokes);

		Out.Buffer = TStrongObjectPtr<USurfaceAccumulationBuffer>(NewObject<USurfaceAccumulationBuffer>());

		if (!Out.Buffer->Allocate(World, Out.Ribbon))
		{
			Test.AddError(TEXT("Failed to allocate the accumulation buffer for the ring depth sweep."));
			return false;
		}

		Out.Buffer->PaintStrokes(World, Strokes);

		TArray<FSurfaceQueryTap> Taps;
		RacetrackSurfaceQuery::BuildTaps(
			Out.Ribbon, 0, Mark, RacetrackSurfaceQuery::DefaultContactPatchWidth, Taps);

		if (Out.Buffer->EnsureGatherTarget(World, 1) == nullptr)
		{
			Test.AddError(TEXT("No gather target for the ring depth sweep."));
			return false;
		}

		Out.Buffer->GatherTaps(World, Taps, 1);

		return true;
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FSweepReadbackRingDepthLatentCommand,
	FAutomationTestBase*, Test,
	TSharedPtr<FRingDepthScenario>, Scenario);

bool FSweepReadbackRingDepthLatentCommand::Update()
{
	FRingDepthScenario& State = *Scenario;

	if (!State.Buffer.IsValid())
	{
		return true;
	}

	if (!State.Ring.IsValid())
	{
		State.Ring = MakeShared<FSurfaceQueryReadback, ESPMode::ThreadSafe>(
			RingDepthCandidates[State.DepthIndex]);

		State.FramesAtThisDepth = 0;
		State.HighestResolved = 0;
		State.ResolveGaps.Reset();
	}

	++State.Frame;

	State.Ring->Submit(State.Buffer->GetGatherTarget(), 1, State.Frame);

	TArray<FLinearColor> Slots;
	uint64 ResolvedFrame = 0;

	if (State.Ring->CopyResolved(Slots, ResolvedFrame) && ResolvedFrame > State.HighestResolved)
	{
		State.HighestResolved = ResolvedFrame;
		State.ResolveGaps.Add(State.Frame - ResolvedFrame);
	}

	if (++State.FramesAtThisDepth < FramesPerRingDepth)
	{
		return false;
	}

	FlushRenderingCommands();

	const int32 Depth = RingDepthCandidates[State.DepthIndex];
	const int32 Accepted = State.Ring->GetSubmissionsAccepted();
	const int32 Dropped = State.Ring->GetSubmissionsDroppedWhileFull();

	State.ResolveGaps.Sort();

	const uint64 MedianGap = State.ResolveGaps.Num() > 0
		? State.ResolveGaps[State.ResolveGaps.Num() / 2]
		: 0;

	Test->AddInfo(FString::Printf(
		TEXT("%d | %d | %d | %d | %d | %llu"),
		Depth,
		FramesPerRingDepth,
		Accepted,
		Dropped,
		State.ResolveGaps.Num(),
		MedianGap));

	Test->TestEqual(
		FString::Printf(
			TEXT("At ring depth %d every submission is either accepted or counted as dropped"), Depth),
		Accepted + Dropped,
		FramesPerRingDepth);

	if (State.DepthIndex == 0)
	{
		State.DropsAtTheShallowestDepth = Dropped;
	}

	if (State.DepthIndex == UE_ARRAY_COUNT(RingDepthCandidates) - 1)
	{
		State.DropsAtTheDeepestDepth = Dropped;
	}

	State.ReleaseRing();

	if (++State.DepthIndex < UE_ARRAY_COUNT(RingDepthCandidates))
	{
		return false;
	}

	Test->TestTrue(
		FString::Printf(
			TEXT("A one deep ring drops more than the deepest one does, so the depth is buying something "
				"rather than being decoration: %d against %d"),
			State.DropsAtTheShallowestDepth,
			State.DropsAtTheDeepestDepth),
		State.DropsAtTheShallowestDepth >= State.DropsAtTheDeepestDepth);

	Test->AddInfo(TEXT("Depth four is what Nanite streaming, Lumen surface cache feedback and distance "
		"field streaming each settled on independently, with no shared engine constant tying them "
		"together. This is the table that says whether it was right here"));

	State.Finish();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReadbackRingDepth,
	"RacetrackSurface.Performance.QueryReadbackRingDepth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReadbackRingDepth::RunTest(const FString& Parameters)
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

	TSharedPtr<FRingDepthScenario> Scenario = MakeShared<FRingDepthScenario>();

	if (!BuildRingSweepFixture(*this, World, *Scenario))
	{
		Scenario->Finish();
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("One submission per engine frame, %d frames at each depth, on an otherwise idle headless "
			"run. The resolve gap is how many frames behind the newest readable answer is"),
		FramesPerRingDepth));

	AddInfo(TEXT("Depth | Submissions | Accepted | Dropped | Resolves seen | Median resolve gap"));

	ADD_LATENT_AUTOMATION_COMMAND(FSweepReadbackRingDepthLatentCommand(this, Scenario));

	return true;
}

#endif
