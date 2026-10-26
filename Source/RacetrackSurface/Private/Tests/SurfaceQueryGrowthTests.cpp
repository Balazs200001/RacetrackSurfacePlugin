// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "SurfaceAccumulationSubsystem.h"
#include "SurfaceQueryReadback.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeTestCircuit;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;

namespace
{
	constexpr float GrowthTestFrameSeconds = 1.0f / 60.0f;

	constexpr double GrowthPaintedLapFraction = 0.70;

	constexpr int32 GrowthFrameBudget = 240;

	FSurfaceTrackRibbon MakeGrowthRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackCoordinate GrowthCoordinate(const FSurfaceTrackRibbon& Ribbon)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = Ribbon.TrackLength * GrowthPaintedLapFraction;
		Coordinate.LateralOffsetAlpha = 0.0;
		return Coordinate;
	}
}

namespace
{
	constexpr int32 GrownQueryCount = 24;

	struct FGrowingQueryScenario
	{
		TStrongObjectPtr<USplineComponent> Circuit;

		USurfaceAccumulationSubsystem* Subsystem = nullptr;

		FSurfaceTrackRibbon Ribbon;

		TArray<FSurfaceQueryHandle> Handles;

		FSurfaceTrackCoordinate Coordinate;

		int32 FramesWaited = 0;

		bool bGrown = false;

		int32 RebuildsBeforeGrowing = 0;

		void Finish()
		{
			if (Subsystem != nullptr)
			{
				for (const FSurfaceQueryHandle& Handle : Handles)
				{
					Subsystem->ReleaseSurfaceQuery(Handle);
				}

				Subsystem->ResetSurface();
				Subsystem = nullptr;
			}

			Handles.Reset();
			Circuit.Reset();
		}
	};

	bool EveryHandleReads(FGrowingQueryScenario& State, float& OutFirstCoverage)
	{
		bool bAll = true;

		for (const FSurfaceQueryHandle& Handle : State.Handles)
		{
			const FSurfaceStateSample Sample =
				State.Subsystem->QuerySurfaceStateInTrackSpace(Handle, State.Coordinate);

			if (Sample.Status != ESurfaceSampleStatus::Valid)
			{
				bAll = false;
				continue;
			}

			OutFirstCoverage = Sample.SkidMark;
		}

		return bAll;
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FGrowTheQueryCountLatentCommand,
	FAutomationTestBase*, Test,
	TSharedPtr<FGrowingQueryScenario>, Scenario);

bool FGrowTheQueryCountLatentCommand::Update()
{
	FGrowingQueryScenario& State = *Scenario;

	if (State.Subsystem == nullptr)
	{
		return true;
	}

	const FSurfaceQueryReadback* Readback = State.Subsystem->GetQueryReadback();

	float Coverage = 0.0f;

	if (EveryHandleReads(State, Coverage) && !State.bGrown)
	{
		State.RebuildsBeforeGrowing = Readback != nullptr ? Readback->GetStagingRebuildCount() : 0;

		while (State.Handles.Num() < GrownQueryCount)
		{
			State.Handles.Add(State.Subsystem->AcquireSurfaceQuery());
		}

		State.bGrown = true;
		State.FramesWaited = 0;

		Test->AddInfo(FString::Printf(
			TEXT("One query resolved, then the live query count was grown to %d, which widens the gather "
				"target under a readback ring whose staging textures were sized for the old width"),
			GrownQueryCount));

		State.Subsystem->Tick(GrowthTestFrameSeconds);

		return false;
	}

	if (State.bGrown && EveryHandleReads(State, Coverage))
	{
		const int32 Rebuilds = Readback != nullptr ? Readback->GetStagingRebuildCount() : 0;

		Test->TestTrue(
			FString::Printf(
				TEXT("Widening the gather target rebuilds the readback ring's staging textures rather "
					"than reusing one sized for the old source: %d rebuilds against %d before"),
				Rebuilds,
				State.RebuildsBeforeGrowing),
			Rebuilds > State.RebuildsBeforeGrowing);

		Test->TestEqual(
			TEXT("Every one of the grown set of queries answers"),
			State.Handles.Num(),
			GrownQueryCount);

		Test->TestTrue(
			FString::Printf(
				TEXT("And they answer with the painted mark rather than with nothing: %.6f"), Coverage),
			Coverage > 0.0f);

		Test->AddInfo(FString::Printf(
			TEXT("Recovered %d engine frames after the gather target grew"), State.FramesWaited));

		State.Finish();

		return true;
	}

	if (++State.FramesWaited > GrowthFrameBudget)
	{
		Test->AddError(FString::Printf(
			TEXT("The queries never resolved across %d engine frames %s growing the query count"),
			GrowthFrameBudget,
			State.bGrown ? TEXT("after") : TEXT("before")));

		State.Finish();

		return true;
	}

	State.Subsystem->Tick(GrowthTestFrameSeconds);

	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryGrowingTheQueryCountAfterResolvingStaysSafe,
	"RacetrackSurface.Query.GrowingTheQueryCountAfterResolvingStaysSafe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryGrowingTheQueryCountAfterResolvingStaysSafe::RunTest(const FString& Parameters)
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
		return true;
	}

	USurfaceAccumulationSubsystem* Subsystem = World->GetSubsystem<USurfaceAccumulationSubsystem>();

	if (Subsystem == nullptr || !Subsystem->IsInitialized())
	{
		AddWarning(TEXT("Skipped: the world's accumulation subsystem is not initialised."));
		return true;
	}

	TSharedPtr<FGrowingQueryScenario> Scenario = MakeShared<FGrowingQueryScenario>();

	Scenario->Subsystem = Subsystem;
	Scenario->Ribbon = MakeGrowthRibbon();
	Scenario->Circuit = MakeTestCircuit();
	Scenario->Coordinate = GrowthCoordinate(Scenario->Ribbon);

	Subsystem->ConfigureTrackRibbon(Scenario->Circuit.Get(), Scenario->Ribbon);
	Subsystem->ResetSurface();

	FSurfaceTrackContact Contact;
	Contact.Coordinate = Scenario->Coordinate;
	Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
	Contact.SlipAmount = 1.0f;
	Contact.ContactWidth = 20.0f;

	Subsystem->RegisterSurfaceContactInTrackSpace(Contact);
	Subsystem->Tick(GrowthTestFrameSeconds);

	Scenario->Handles.Add(Subsystem->AcquireSurfaceQuery());

	ADD_LATENT_AUTOMATION_COMMAND(FGrowTheQueryCountLatentCommand(this, Scenario));

	return true;
}

#endif
