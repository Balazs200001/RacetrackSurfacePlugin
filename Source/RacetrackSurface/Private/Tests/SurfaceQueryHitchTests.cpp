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
	constexpr float HitchTestFrameSeconds = 1.0f / 60.0f;

	constexpr float HitchSeconds = 0.5f;

	constexpr double HitchPaintedLapFraction = 0.70;

	constexpr int32 HitchFrameBudget = 240;

	struct FHitchScenario
	{
		TStrongObjectPtr<USplineComponent> Circuit;

		USurfaceAccumulationSubsystem* Subsystem = nullptr;

		FSurfaceTrackRibbon Ribbon;

		FSurfaceQueryHandle Handle;

		FSurfaceTrackCoordinate Coordinate;

		uint64 AwaitingSubmission = 0;

		int32 FramesWaited = 0;

		bool bHitched = false;

		uint64 QueryFramesBeforeHitch = 0;

		int32 AcceptedBeforeHitch = 0;

		int32 DroppedBeforeHitch = 0;

		float StaleBeforeHitch = 0.0f;

		void Finish()
		{
			if (Subsystem != nullptr)
			{
				Subsystem->ReleaseSurfaceQuery(Handle);
				Subsystem->ResetSurface();
				Subsystem = nullptr;
			}

			Circuit.Reset();
		}
	};

	FSurfaceTrackRibbon MakeHitchRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackCoordinate HitchCoordinate(const FSurfaceTrackRibbon& Ribbon)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = Ribbon.TrackLength * HitchPaintedLapFraction;
		Coordinate.LateralOffsetAlpha = 0.0;
		return Coordinate;
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FHitchSurfaceQueryLatentCommand,
	FAutomationTestBase*, Test,
	TSharedPtr<FHitchScenario>, Scenario);

bool FHitchSurfaceQueryLatentCommand::Update()
{
	FHitchScenario& State = *Scenario;

	if (State.Subsystem == nullptr)
	{
		return true;
	}

	const FSurfaceStateSample Sample =
		State.Subsystem->QuerySurfaceStateInTrackSpace(State.Handle, State.Coordinate);

	const FSurfaceQueryReadback* Readback = State.Subsystem->GetQueryReadback();

	const bool bAnswered = Sample.Status == ESurfaceSampleStatus::Valid
		&& State.Subsystem->GetResolvedQueryFrame() >= State.AwaitingSubmission;

	if (bAnswered && !State.bHitched)
	{
		State.QueryFramesBeforeHitch = State.Subsystem->GetQueryFrameCounter();
		State.AcceptedBeforeHitch = Readback != nullptr ? Readback->GetSubmissionsAccepted() : 0;
		State.DroppedBeforeHitch = Readback != nullptr ? Readback->GetSubmissionsDroppedWhileFull() : 0;
		State.StaleBeforeHitch = Sample.SecondsSinceResolved;

		State.Subsystem->Tick(HitchSeconds);

		State.bHitched = true;
		State.AwaitingSubmission = State.Subsystem->GetQueryFrameCounter();
		State.FramesWaited = 0;

		Test->AddInfo(FString::Printf(
			TEXT("Resolved once, then ticked a single %.0f ms frame with a gather in flight"),
			HitchSeconds * 1000.0f));

		return false;
	}

	if (bAnswered && State.bHitched)
	{
		const int32 Accepted = Readback != nullptr ? Readback->GetSubmissionsAccepted() : 0;
		const int32 Dropped = Readback != nullptr ? Readback->GetSubmissionsDroppedWhileFull() : 0;

		const int32 SubmittedAcrossTheHitch =
			static_cast<int32>(State.Subsystem->GetQueryFrameCounter() - State.QueryFramesBeforeHitch);

		Test->AddInfo(FString::Printf(
			TEXT("Recovered %d engine frames after the hitch: %d submitted, %d accepted, %d dropped, "
				"%.4f s stale against %.4f s before it"),
			State.FramesWaited,
			SubmittedAcrossTheHitch,
			Accepted - State.AcceptedBeforeHitch,
			Dropped - State.DroppedBeforeHitch,
			Sample.SecondsSinceResolved,
			State.StaleBeforeHitch));

		Test->TestTrue(
			TEXT("The hitch submitted at least the gather it was ticked for"),
			SubmittedAcrossTheHitch >= 1);

		Test->TestEqual(
			TEXT("Every submission made across the hitch is either accepted or counted as dropped, so a "
				"ring the GPU has fallen behind reports it rather than losing work quietly"),
			(Accepted - State.AcceptedBeforeHitch) + (Dropped - State.DroppedBeforeHitch),
			SubmittedAcrossTheHitch);

		Test->TestTrue(
			TEXT("Staleness after a half second frame is never negative, which is what a clock read "
				"backwards would look like"),
			Sample.SecondsSinceResolved >= 0.0f);

		Test->TestTrue(
			TEXT("The answer belongs to a gather submitted after the hitch, not to the snapshot from "
				"before it"),
			State.Subsystem->GetResolvedQueryFrame() >= State.AwaitingSubmission);

		State.Finish();

		return true;
	}

	if (++State.FramesWaited > HitchFrameBudget)
	{
		Test->AddError(FString::Printf(
			TEXT("The query never resolved across %d engine frames %s the hitch, ending in status %d. A "
				"ring that wedges after a long frame is exactly what F7 exists to catch"),
			HitchFrameBudget,
			State.bHitched ? TEXT("after") : TEXT("before"),
			static_cast<int32>(Sample.Status)));

		State.Finish();

		return true;
	}

	State.Subsystem->Tick(HitchTestFrameSeconds);

	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQuerySurvivesAHitch,
	"RacetrackSurface.Query.SurvivesAHitch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQuerySurvivesAHitch::RunTest(const FString& Parameters)
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

	TSharedPtr<FHitchScenario> Scenario = MakeShared<FHitchScenario>();

	Scenario->Subsystem = Subsystem;
	Scenario->Ribbon = MakeHitchRibbon();
	Scenario->Circuit = MakeTestCircuit();
	Scenario->Coordinate = HitchCoordinate(Scenario->Ribbon);

	Subsystem->ConfigureTrackRibbon(Scenario->Circuit.Get(), Scenario->Ribbon);
	Subsystem->ResetSurface();

	FSurfaceTrackContact Contact;
	Contact.Coordinate = Scenario->Coordinate;
	Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
	Contact.SlipAmount = 1.0f;
	Contact.ContactWidth = 20.0f;

	Subsystem->RegisterSurfaceContactInTrackSpace(Contact);
	Subsystem->Tick(HitchTestFrameSeconds);

	Scenario->Handle = Subsystem->AcquireSurfaceQuery();
	Scenario->AwaitingSubmission = Subsystem->GetQueryFrameCounter() + 1;

	ADD_LATENT_AUTOMATION_COMMAND(FHitchSurfaceQueryLatentCommand(this, Scenario));

	return true;
}

#endif
