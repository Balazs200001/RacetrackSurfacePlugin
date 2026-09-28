#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
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
	constexpr float QueryFrameSeconds = 1.0f / 60.0f;

	constexpr double QueryPaintedLapFraction = 0.75;

	constexpr int32 QueryFrameBudget = 240;

	constexpr float QueryLockupSlip = 1.0f;

	constexpr float QueryRubberSlip = 0.5f;

	struct FQueryScenario
	{
		TStrongObjectPtr<USplineComponent> Circuit;

		USurfaceAccumulationSubsystem* Subsystem = nullptr;

		FSurfaceTrackRibbon Ribbon;

		FSurfaceQueryHandle Handle;

		FSurfaceTrackCoordinate Coordinate;

		uint64 FirstSubmission = 0;

		int32 FramesWaited = 0;

		FSurfaceStateSample Sample;

		TFunction<void(FAutomationTestBase&, FQueryScenario&)> OnResolved;

		void Retarget(const FSurfaceTrackCoordinate& NewCoordinate)
		{
			Coordinate = NewCoordinate;
			FirstSubmission = Subsystem->GetQueryFrameCounter() + 1;
			FramesWaited = 0;
		}

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

	FSurfaceTrackRibbon MakeQueryRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackCoordinate PaintedCoordinate(const FSurfaceTrackRibbon& Ribbon)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = Ribbon.TrackLength * QueryPaintedLapFraction;
		Coordinate.LateralOffsetAlpha = 0.0;
		return Coordinate;
	}

	void PaintTheMark(FQueryScenario& Scenario, float DepositionScale, float SlipAmount)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
			RacetrackSurfaceMapping::DepositionScaleVariableName);

		const float Restore = Variable != nullptr ? Variable->GetFloat() : 1.0f;

		if (Variable != nullptr && DepositionScale > 0.0f)
		{
			Variable->Set(DepositionScale);
		}

		FSurfaceTrackContact Contact;
		Contact.Coordinate = PaintedCoordinate(Scenario.Ribbon);
		Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
		Contact.SlipAmount = SlipAmount;
		Contact.ContactWidth = 20.0f;

		Scenario.Subsystem->RegisterSurfaceContactInTrackSpace(Contact);
		Scenario.Subsystem->Tick(QueryFrameSeconds);

		if (Variable != nullptr && DepositionScale > 0.0f)
		{
			Variable->Set(Restore);
		}
	}

	TSharedPtr<FQueryScenario> BuildScenario(FAutomationTestBase& Test)
	{
		UWorld* World = FindRenderableWorld();

		if (World == nullptr)
		{
			Test.AddError(TEXT("No renderable world available."));
			return nullptr;
		}

		USurfaceAccumulationSubsystem* Subsystem = World->GetSubsystem<USurfaceAccumulationSubsystem>();

		if (Subsystem == nullptr)
		{
			Test.AddError(TEXT("Could not resolve USurfaceAccumulationSubsystem from the world."));
			return nullptr;
		}

		if (!Subsystem->IsInitialized())
		{
			Test.AddWarning(TEXT("Skipped: the world's accumulation subsystem is not initialised."));
			return nullptr;
		}

		if (Subsystem->GetQueryReadback() == nullptr)
		{
			Test.AddError(TEXT("Initialize did not build a readback ring, which is the configuration path "
				"that must not be allowed to silently never run."));
			return nullptr;
		}

		TSharedPtr<FQueryScenario> Scenario = MakeShared<FQueryScenario>();

		Scenario->Subsystem = Subsystem;
		Scenario->Ribbon = MakeQueryRibbon();
		Scenario->Circuit = MakeTestCircuit();

		Subsystem->ConfigureTrackRibbon(Scenario->Circuit.Get(), Scenario->Ribbon);
		Subsystem->ResetSurface();

		return Scenario;
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FResolveSurfaceQueryLatentCommand,
	FAutomationTestBase*, Test,
	TSharedPtr<FQueryScenario>, Scenario);

bool FResolveSurfaceQueryLatentCommand::Update()
{
	FQueryScenario& State = *Scenario;

	if (State.Subsystem == nullptr)
	{
		return true;
	}

	State.Sample = State.Subsystem->QuerySurfaceStateInTrackSpace(State.Handle, State.Coordinate);

	const bool bAnsweredForThisPosition =
		State.Subsystem->GetResolvedQueryFrame() >= State.FirstSubmission;

	if (State.Sample.Status == ESurfaceSampleStatus::Valid && bAnsweredForThisPosition)
	{
		Test->AddInfo(FString::Printf(
			TEXT("Resolved after %d engine frames, %llu query frames behind: rubber %.6f, skid %.6f, "
				"marbles %.6f, %.4f s stale"),
			State.FramesWaited,
			State.Subsystem->GetQueryLatencyInFrames(),
			State.Sample.RubberBuildup,
			State.Sample.SkidMark,
			State.Sample.Marbles,
			State.Sample.SecondsSinceResolved));

		if (State.OnResolved)
		{
			State.OnResolved(*Test, State);
		}

		State.Finish();

		return true;
	}

	if (++State.FramesWaited > QueryFrameBudget)
	{
		Test->AddError(FString::Printf(
			TEXT("The query never resolved across %d engine frames, ending in status %d"),
			QueryFrameBudget,
			static_cast<int32>(State.Sample.Status)));

		State.Finish();

		return true;
	}

	State.Subsystem->Tick(QueryFrameSeconds);

	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReturnsCoverageForEveryChannelAtOnce,
	"RacetrackSurface.Query.ReturnsCoverageForEveryChannelAtOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReturnsCoverageForEveryChannelAtOnce::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	TSharedPtr<FQueryScenario> Scenario = BuildScenario(*this);

	if (!Scenario.IsValid())
	{
		return true;
	}

	PaintTheMark(*Scenario, 0.0f, QueryLockupSlip);

	Scenario->Handle = Scenario->Subsystem->AcquireSurfaceQuery();

	TestTrue(TEXT("Acquiring a query gives a handle that is set"), Scenario->Handle.IsSet());

	const FSurfaceStateSample First = Scenario->Subsystem->QuerySurfaceStateInTrackSpace(
		Scenario->Handle,
		PaintedCoordinate(Scenario->Ribbon));

	TestEqual(
		TEXT("The first query on a fresh handle is NotYetResolved, never a fabricated zero"),
		First.Status,
		ESurfaceSampleStatus::NotYetResolved);

	TestFalse(TEXT("An unresolved sample is not usable"), First.IsUsable());

	Scenario->Retarget(PaintedCoordinate(Scenario->Ribbon));

	Scenario->OnResolved = [](FAutomationTestBase& Test, FQueryScenario& State)
	{
		Test.TestTrue(TEXT("The skid channel carries the painted mark"), State.Sample.SkidMark > 0.0f);
		Test.TestEqual(TEXT("The rubber channel is a genuine zero"), State.Sample.RubberBuildup, 0.0f);
		Test.TestEqual(TEXT("The marbles channel is a genuine zero"), State.Sample.Marbles, 0.0f);

		Test.TestTrue(
			FString::Printf(
				TEXT("Coverage is bounded to one rather than unbounded exposure: %.6f"),
				State.Sample.SkidMark),
			State.Sample.SkidMark <= 1.0f);

		Test.TestEqual(
			TEXT("GetChannel agrees with the named field"),
			State.Sample.GetChannel(EAccumulationChannel::SkidMark),
			State.Sample.SkidMark);
	};

	ADD_LATENT_AUTOMATION_COMMAND(FResolveSurfaceQueryLatentCommand(this, Scenario));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryReturnsCoverageNotExposure,
	"RacetrackSurface.Query.ReturnsCoverageNotExposure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryReturnsCoverageNotExposure::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	TSharedPtr<FQueryScenario> Scenario = BuildScenario(*this);

	if (!Scenario.IsValid())
	{
		return true;
	}

	PaintTheMark(*Scenario, 400.0f, QueryRubberSlip);

	Scenario->Handle = Scenario->Subsystem->AcquireSurfaceQuery();
	Scenario->Retarget(PaintedCoordinate(Scenario->Ribbon));

	Scenario->OnResolved = [](FAutomationTestBase& Test, FQueryScenario& State)
	{
		Test.TestTrue(
			FString::Printf(
				TEXT("A rubber mark deposited at four hundred times the derived rate still reads inside the "
					"coverage range rather than returning raw exposure: %.6f"),
				State.Sample.RubberBuildup),
			State.Sample.RubberBuildup > 0.0f && State.Sample.RubberBuildup <= 1.0f);

		Test.TestTrue(
			TEXT("Heavy deposition reads far higher than the derived rate does, so the curve is being "
				"applied rather than the value being clamped"),
			State.Sample.RubberBuildup > 0.1f);
	};

	ADD_LATENT_AUTOMATION_COMMAND(FResolveSurfaceQueryLatentCommand(this, Scenario));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryDistinguishesOutsideTheRibbonFromZero,
	"RacetrackSurface.Query.DistinguishesOutsideTheRibbonFromZero",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryDistinguishesOutsideTheRibbonFromZero::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	TSharedPtr<FQueryScenario> Scenario = BuildScenario(*this);

	if (!Scenario.IsValid())
	{
		return true;
	}

	PaintTheMark(*Scenario, 0.0f, QueryLockupSlip);

	Scenario->Handle = Scenario->Subsystem->AcquireSurfaceQuery();

	FSurfaceTrackCoordinate BeyondTheEdge = PaintedCoordinate(Scenario->Ribbon);
	BeyondTheEdge.LateralOffsetAlpha = 1.8;

	const FSurfaceStateSample OffTrack =
		Scenario->Subsystem->QuerySurfaceStateInTrackSpace(Scenario->Handle, BeyondTheEdge);

	TestEqual(
		TEXT("A point past the track edge reports OutsideRegion rather than a zero a caller would "
			"mistake for clean tarmac inside the ribbon"),
		OffTrack.Status,
		ESurfaceSampleStatus::OutsideRegion);

	TestFalse(TEXT("An out of region sample is not usable"), OffTrack.IsUsable());

	FSurfaceTrackCoordinate CleanButInside = PaintedCoordinate(Scenario->Ribbon);
	CleanButInside.DistanceAlongCentreline = Scenario->Ribbon.TrackLength * 0.25;

	Scenario->Retarget(CleanButInside);

	Scenario->OnResolved = [](FAutomationTestBase& Test, FQueryScenario& State)
	{
		Test.TestEqual(
			TEXT("Clean tarmac a quarter lap from the mark reads zero skid, and reads it as Valid "
				"rather than as OutsideRegion"),
			State.Sample.SkidMark,
			0.0f);

		Test.AddInfo(TEXT("Off the ribbon is OutsideRegion; on the ribbon with nothing painted is Valid "
			"and zero. A caller can tell the two apart, which is what the three state enum is for"));
	};

	ADD_LATENT_AUTOMATION_COMMAND(FResolveSurfaceQueryLatentCommand(this, Scenario));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryAStaleHandleDoesNotReadAnotherSlot,
	"RacetrackSurface.Query.AStaleHandleDoesNotReadAnotherSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryAStaleHandleDoesNotReadAnotherSlot::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	TSharedPtr<FQueryScenario> Scenario = BuildScenario(*this);

	if (!Scenario.IsValid())
	{
		return true;
	}

	const FSurfaceQueryHandle Destroyed = Scenario->Subsystem->AcquireSurfaceQuery();

	Scenario->Subsystem->ReleaseSurfaceQuery(Destroyed);

	const FSurfaceQueryHandle Recycled = Scenario->Subsystem->AcquireSurfaceQuery();

	TestEqual(
		TEXT("The freed slot is reused rather than growing the array"),
		Recycled.SlotIndex,
		Destroyed.SlotIndex);

	TestNotEqual(
		TEXT("The recycled handle carries a new generation"),
		Recycled.Generation,
		Destroyed.Generation);

	const FSurfaceStateSample FromTheStaleHandle =
		Scenario->Subsystem->QuerySurfaceStateInTrackSpace(Destroyed, PaintedCoordinate(Scenario->Ribbon));

	TestEqual(
		TEXT("A handle held by a destroyed actor reads nothing rather than another actor's grip"),
		FromTheStaleHandle.Status,
		ESurfaceSampleStatus::NotYetResolved);

	TestEqual(TEXT("A stale handle reports no coverage at all"), FromTheStaleHandle.SkidMark, 0.0f);

	TestEqual(
		TEXT("One slot is live after the recycle, so the stale handle holds nothing of its own"),
		Scenario->Subsystem->GetActiveSurfaceQueryCount(),
		1);

	Scenario->Handle = Recycled;
	Scenario->Finish();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryNeverAllocatesABufferOfItsOwn,
	"RacetrackSurface.Query.NeverAllocatesABufferOfItsOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryNeverAllocatesABufferOfItsOwn::RunTest(const FString& Parameters)
{
	TSharedPtr<FQueryScenario> Scenario = BuildScenario(*this);

	if (!Scenario.IsValid())
	{
		return true;
	}

	const USurfaceAccumulationBuffer* Before = Scenario->Subsystem->GetBuffer();

	Scenario->Handle = Scenario->Subsystem->AcquireSurfaceQuery();

	FSurfaceTrackCoordinate CleanTarmac = PaintedCoordinate(Scenario->Ribbon);
	CleanTarmac.DistanceAlongCentreline = Scenario->Ribbon.TrackLength * 0.1;

	for (int32 Frame = 0; Frame < 8; ++Frame)
	{
		Scenario->Subsystem->QuerySurfaceStateInTrackSpace(Scenario->Handle, CleanTarmac);
		Scenario->Subsystem->Tick(QueryFrameSeconds);
	}

	TestTrue(
		TEXT("Eight frames of querying and resolving never allocate the accumulation buffer, which is "
			"225 MiB on the demo ribbon. Only a contact may allocate it"),
		Scenario->Subsystem->GetBuffer() == Before);

	AddInfo(FString::Printf(
		TEXT("The accumulation buffer was %s before the queries and is %s after them"),
		Before != nullptr ? TEXT("allocated") : TEXT("absent"),
		Scenario->Subsystem->GetBuffer() != nullptr ? TEXT("allocated") : TEXT("absent")));

	Scenario->Finish();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryResetClearsTheCache,
	"RacetrackSurface.Query.ResetClearsTheCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryResetClearsTheCache::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	TSharedPtr<FQueryScenario> Scenario = BuildScenario(*this);

	if (!Scenario.IsValid())
	{
		return true;
	}

	PaintTheMark(*Scenario, 0.0f, QueryLockupSlip);

	Scenario->Handle = Scenario->Subsystem->AcquireSurfaceQuery();
	Scenario->Retarget(PaintedCoordinate(Scenario->Ribbon));

	Scenario->OnResolved = [](FAutomationTestBase& Test, FQueryScenario& State)
	{
		Test.TestTrue(TEXT("There was something to read before the reset"), State.Sample.SkidMark > 0.0f);

		State.Subsystem->ResetSurface();

		const FSurfaceStateSample AfterReset =
			State.Subsystem->QuerySurfaceStateInTrackSpace(State.Handle, State.Coordinate);

		Test.TestEqual(
			TEXT("Resetting the surface drops the query cache too, rather than leaving it reporting "
				"rubber that has just been wiped off the GPU"),
			AfterReset.Status,
			ESurfaceSampleStatus::NotYetResolved);
	};

	ADD_LATENT_AUTOMATION_COMMAND(FResolveSurfaceQueryLatentCommand(this, Scenario));

	return true;
}

#endif
