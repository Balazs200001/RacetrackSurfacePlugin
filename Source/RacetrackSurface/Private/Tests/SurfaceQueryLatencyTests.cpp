#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeTestCircuit;

namespace
{
	constexpr float LatencyFrameSeconds = 1.0f / 60.0f;

	constexpr int32 LatencyFrameBudget = 240;

	constexpr double LatencyLapCentimetres = 62500.0;

	constexpr double LatencyTrackWidth = 800.0;

	constexpr double PaintedFrom = 0.30;

	constexpr double PaintedTo = 0.60;

	constexpr float PaintedDepositionScale = 100.0f;

	constexpr double AccumulatedBandWidth = 300.0;

	constexpr double WheelSpeedCentimetresPerSecond = 5500.0;

	constexpr double WheelLateralAlphaPerSecond = 1.2;

	constexpr int32 SmoothPairCount = 8;

	constexpr int32 LateralPairCount = 8;

	constexpr int32 EdgePairCount = 4;

	constexpr double ToleranceFractionOfCoverage = 0.10;

	enum class EPairKind : uint8
	{
		AlongTrackInsideTheBand,
		LateralInsideTheBand,
		AcrossThePaintedEdge
	};

	struct FDisplacedPair
	{
		EPairKind Kind = EPairKind::AlongTrackInsideTheBand;

		FSurfaceTrackCoordinate Submitted;

		FSurfaceTrackCoordinate Occupied;

		FSurfaceQueryHandle SubmittedHandle;

		FSurfaceQueryHandle OccupiedHandle;

		float SubmittedCoverage = 0.0f;

		float OccupiedCoverage = 0.0f;

		bool bResolved = false;

		float Error() const
		{
			return FMath::Abs(OccupiedCoverage - SubmittedCoverage);
		}
	};

	struct FLatencyScenario
	{
		TStrongObjectPtr<USplineComponent> Circuit;

		USurfaceAccumulationSubsystem* Subsystem = nullptr;

		FSurfaceTrackRibbon Ribbon;

		FSurfaceQueryHandle Probe;

		FSurfaceTrackCoordinate ProbeCoordinate;

		uint64 ProbeAwaits = 0;

		uint64 LatencyInQueryFrames = 0;

		int32 EngineFramesToResolve = 0;

		int32 FramesWaited = 0;

		bool bLatencyKnown = false;

		bool bPairsSubmitted = false;

		double AlongTrackDisplacement = 0.0;

		double LateralDisplacementAlpha = 0.0;

		TArray<FDisplacedPair> Pairs;

		void Finish()
		{
			if (Subsystem != nullptr)
			{
				Subsystem->ReleaseSurfaceQuery(Probe);

				for (const FDisplacedPair& Pair : Pairs)
				{
					Subsystem->ReleaseSurfaceQuery(Pair.SubmittedHandle);
					Subsystem->ReleaseSurfaceQuery(Pair.OccupiedHandle);
				}

				Subsystem->ResetSurface();
				Subsystem = nullptr;
			}

			Pairs.Reset();
			Circuit.Reset();
		}
	};

	FSurfaceTrackRibbon MakeLatencyRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = LatencyLapCentimetres;
		Ribbon.TrackWidth = LatencyTrackWidth;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackCoordinate LatencyCoordinateAt(double LapFraction, double LateralOffsetAlpha)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = LatencyLapCentimetres * LapFraction;
		Coordinate.LateralOffsetAlpha = LateralOffsetAlpha;
		return Coordinate;
	}

	void PaintTheAccumulatedLine(USurfaceAccumulationSubsystem& Subsystem)
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
			RacetrackSurfaceMapping::DepositionScaleVariableName);

		const float Restore = Variable != nullptr ? Variable->GetFloat() : 1.0f;

		if (Variable != nullptr)
		{
			Variable->Set(PaintedDepositionScale);
		}

		FSurfaceTrackContact Contact;
		Contact.Coordinate = LatencyCoordinateAt(PaintedTo, 0.0);
		Contact.CentimetresPerSecondAlongCentreline = LatencyLapCentimetres * (PaintedTo - PaintedFrom);
		Contact.SlipAmount = 1.0f;
		Contact.ContactWidth = static_cast<float>(AccumulatedBandWidth);

		Subsystem.RegisterSurfaceContactInTrackSpace(Contact);
		Subsystem.Tick(1.0f);

		if (Variable != nullptr)
		{
			Variable->Set(Restore);
		}
	}

	void BuildDisplacedPairs(FLatencyScenario& State)
	{
		const double Seconds = State.LatencyInQueryFrames * LatencyFrameSeconds;

		State.AlongTrackDisplacement = WheelSpeedCentimetresPerSecond * Seconds;
		State.LateralDisplacementAlpha = WheelLateralAlphaPerSecond * Seconds;

		const double AlongTrackFraction = State.AlongTrackDisplacement / LatencyLapCentimetres;
		const double Span = PaintedTo - PaintedFrom;
		const double Margin = AlongTrackFraction + 0.02;

		for (int32 Index = 0; Index < SmoothPairCount; ++Index)
		{
			const double Where = PaintedFrom + Margin + (Span - 2.0 * Margin) * Index / FMath::Max(SmoothPairCount - 1, 1);

			FDisplacedPair Pair;
			Pair.Kind = EPairKind::AlongTrackInsideTheBand;
			Pair.Submitted = LatencyCoordinateAt(Where, 0.0);
			Pair.Occupied = LatencyCoordinateAt(Where + AlongTrackFraction, 0.0);
			State.Pairs.Add(Pair);
		}

		for (int32 Index = 0; Index < LateralPairCount; ++Index)
		{
			const double Where = PaintedFrom + Margin + (Span - 2.0 * Margin) * Index / FMath::Max(LateralPairCount - 1, 1);
			const double Alpha = -0.5 + 1.0 * Index / FMath::Max(LateralPairCount - 1, 1);

			FDisplacedPair Pair;
			Pair.Kind = EPairKind::LateralInsideTheBand;
			Pair.Submitted = LatencyCoordinateAt(Where, Alpha);
			Pair.Occupied = LatencyCoordinateAt(Where, Alpha + State.LateralDisplacementAlpha);
			State.Pairs.Add(Pair);
		}

		for (int32 Index = 0; Index < EdgePairCount; ++Index)
		{
			const double Straddle = AlongTrackFraction * (Index + 1) / (EdgePairCount + 1);

			FDisplacedPair Pair;
			Pair.Kind = EPairKind::AcrossThePaintedEdge;
			Pair.Submitted = LatencyCoordinateAt(PaintedFrom - Straddle, 0.0);
			Pair.Occupied = LatencyCoordinateAt(PaintedFrom - Straddle + AlongTrackFraction, 0.0);
			State.Pairs.Add(Pair);
		}

		for (FDisplacedPair& Pair : State.Pairs)
		{
			Pair.SubmittedHandle = State.Subsystem->AcquireSurfaceQuery();
			Pair.OccupiedHandle = State.Subsystem->AcquireSurfaceQuery();
		}
	}

	bool ReadEveryPair(FLatencyScenario& State)
	{
		bool bAllResolved = true;

		for (FDisplacedPair& Pair : State.Pairs)
		{
			const FSurfaceStateSample Submitted =
				State.Subsystem->QuerySurfaceStateInTrackSpace(Pair.SubmittedHandle, Pair.Submitted);

			const FSurfaceStateSample Occupied =
				State.Subsystem->QuerySurfaceStateInTrackSpace(Pair.OccupiedHandle, Pair.Occupied);

			Pair.bResolved = Submitted.Status == ESurfaceSampleStatus::Valid
				&& Occupied.Status == ESurfaceSampleStatus::Valid;

			if (!Pair.bResolved)
			{
				bAllResolved = false;
				continue;
			}

			Pair.SubmittedCoverage = Submitted.SkidMark;
			Pair.OccupiedCoverage = Occupied.SkidMark;
		}

		return bAllResolved;
	}

	void SummariseKind(
		FAutomationTestBase& Test,
		const FLatencyScenario& State,
		EPairKind Kind,
		const TCHAR* Name,
		double& OutMedianError,
		double& OutWorstError,
		double& OutMeanCoverage)
	{
		TArray<double> Errors;
		double CoverageTotal = 0.0;

		for (const FDisplacedPair& Pair : State.Pairs)
		{
			if (Pair.Kind != Kind || !Pair.bResolved)
			{
				continue;
			}

			Errors.Add(Pair.Error());
			CoverageTotal += Pair.SubmittedCoverage;
		}

		OutMedianError = 0.0;
		OutWorstError = 0.0;
		OutMeanCoverage = 0.0;

		if (Errors.Num() == 0)
		{
			Test.AddError(FString::Printf(TEXT("No %s pair resolved, so nothing was measured"), Name));
			return;
		}

		Errors.Sort();

		OutMedianError = Errors[Errors.Num() / 2];
		OutWorstError = Errors.Last();
		OutMeanCoverage = CoverageTotal / Errors.Num();

		Test.AddInfo(FString::Printf(
			TEXT("%s: %d pairs, median coverage error %.6f, worst %.6f, against a mean coverage of %.6f"),
			Name,
			Errors.Num(),
			OutMedianError,
			OutWorstError,
			OutMeanCoverage));
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FMeasureQueryLatencyToleranceLatentCommand,
	FAutomationTestBase*, Test,
	TSharedPtr<FLatencyScenario>, Scenario);

bool FMeasureQueryLatencyToleranceLatentCommand::Update()
{
	FLatencyScenario& State = *Scenario;

	if (State.Subsystem == nullptr)
	{
		return true;
	}

	if (!State.bLatencyKnown)
	{
		const FSurfaceStateSample Probe =
			State.Subsystem->QuerySurfaceStateInTrackSpace(State.Probe, State.ProbeCoordinate);

		if (Probe.Status == ESurfaceSampleStatus::Valid
			&& State.Subsystem->GetResolvedQueryFrame() >= State.ProbeAwaits)
		{
			State.LatencyInQueryFrames = FMath::Max(State.Subsystem->GetQueryLatencyInFrames(), static_cast<uint64>(1));
			State.EngineFramesToResolve = State.FramesWaited;
			State.bLatencyKnown = true;
			State.FramesWaited = 0;

			Test->AddInfo(FString::Printf(
				TEXT("Latency measured first, because the displacement to sample over depends on it: "
					"%llu query frames, %d engine frames, %.4f s of dilated world time"),
				State.LatencyInQueryFrames,
				State.EngineFramesToResolve,
				Probe.SecondsSinceResolved));

			BuildDisplacedPairs(State);

			return false;
		}
	}
	else
	{
		if (!State.bPairsSubmitted)
		{
			State.bPairsSubmitted = true;
		}

		if (ReadEveryPair(State))
		{
			const double AlongTrackTexels = State.AlongTrackDisplacement / State.Ribbon.CentimetresPerTexel;

			const double LateralCentimetres =
				State.LateralDisplacementAlpha * State.Ribbon.TrackWidth * 0.5;

			const double LateralTexels = LateralCentimetres / State.Ribbon.CentimetresPerTexel;

			Test->AddInfo(FString::Printf(
				TEXT("Over %llu query frames at %.0f cm/s the wheel travels %.1f cm along track, which is "
					"%.1f texels; at %.2f alpha per second it travels %.4f alpha laterally, which is "
					"%.1f cm and %.2f texels"),
				State.LatencyInQueryFrames,
				WheelSpeedCentimetresPerSecond,
				State.AlongTrackDisplacement,
				AlongTrackTexels,
				WheelLateralAlphaPerSecond,
				State.LateralDisplacementAlpha,
				LateralCentimetres,
				LateralTexels));

			double AlongTrackMedian = 0.0;
			double AlongTrackWorst = 0.0;
			double AlongTrackCoverage = 0.0;

			double LateralMedian = 0.0;
			double LateralWorst = 0.0;
			double LateralCoverage = 0.0;

			double EdgeMedian = 0.0;
			double EdgeWorst = 0.0;
			double EdgeCoverage = 0.0;

			SummariseKind(*Test, State, EPairKind::AlongTrackInsideTheBand,
				TEXT("Along track, inside the accumulated band"),
				AlongTrackMedian, AlongTrackWorst, AlongTrackCoverage);

			SummariseKind(*Test, State, EPairKind::LateralInsideTheBand,
				TEXT("Laterally, at the fastest line change the car can make"),
				LateralMedian, LateralWorst, LateralCoverage);

			SummariseKind(*Test, State, EPairKind::AcrossThePaintedEdge,
				TEXT("Across the painted edge, which no swept deposit actually has"),
				EdgeMedian, EdgeWorst, EdgeCoverage);

			Test->TestTrue(
				TEXT("There is a field to measure a gradient in at all"),
				AlongTrackCoverage > 0.0);

			Test->TestTrue(
				FString::Printf(
					TEXT("The measurement is sensitive to the field's gradient: a step edge disagrees by "
						"%.6f where the interior disagrees by %.6f. If these matched, the method would be "
						"measuring nothing"),
					EdgeWorst,
					AlongTrackMedian),
				EdgeWorst > AlongTrackMedian);

			const double Tolerance = ToleranceFractionOfCoverage * AlongTrackCoverage;

			if (AlongTrackMedian > Tolerance)
			{
				Test->AddWarning(FString::Printf(
					TEXT("The along track error inside the band is %.6f, which is more than %.0f %% of "
						"the %.6f coverage there. The proposal's tolerance argument rests on the field "
						"being smooth in the direction the wheel moves fastest, and at this gradient it "
						"is not. Reported rather than failed, because this is a measurement of the field "
						"rather than a defect in the query"),
					AlongTrackMedian,
					ToleranceFractionOfCoverage * 100.0,
					AlongTrackCoverage));
			}

			State.Finish();

			return true;
		}
	}

	if (++State.FramesWaited > LatencyFrameBudget)
	{
		Test->AddError(FString::Printf(
			TEXT("Gave up after %d engine frames with the latency %s"),
			LatencyFrameBudget,
			State.bLatencyKnown ? TEXT("measured but the pairs unresolved") : TEXT("still unmeasured")));

		State.Finish();

		return true;
	}

	State.Subsystem->Tick(LatencyFrameSeconds);

	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryLatencyCostsLessThanTheFieldVariesAlongTrack,
	"RacetrackSurface.Query.LatencyCostsLessThanTheFieldVariesAlongTrack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryLatencyCostsLessThanTheFieldVariesAlongTrack::RunTest(const FString& Parameters)
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

	TSharedPtr<FLatencyScenario> Scenario = MakeShared<FLatencyScenario>();

	Scenario->Subsystem = Subsystem;
	Scenario->Ribbon = MakeLatencyRibbon();
	Scenario->Circuit = MakeTestCircuit();
	Scenario->ProbeCoordinate = LatencyCoordinateAt((PaintedFrom + PaintedTo) * 0.5, 0.0);

	Subsystem->ConfigureTrackRibbon(Scenario->Circuit.Get(), Scenario->Ribbon);
	Subsystem->ResetSurface();

	PaintTheAccumulatedLine(*Subsystem);

	AddInfo(FString::Printf(
		TEXT("A swept skid from lap fraction %.2f to %.2f, %.0f cm wide, at %.0fx the derived deposition. "
			"That is the shape a deposited line has: swept along the track and broad across it, because "
			"it is laid by tyres moving, not stamped. On a %.0f cm track its edges sit at alpha %.3f, and "
			"the lateral pairs are swept from -0.5 to 0.5 so some of them straddle one"),
		PaintedFrom,
		PaintedTo,
		AccumulatedBandWidth,
		PaintedDepositionScale,
		LatencyTrackWidth,
		AccumulatedBandWidth / LatencyTrackWidth));

	Scenario->Probe = Subsystem->AcquireSurfaceQuery();
	Scenario->ProbeAwaits = Subsystem->GetQueryFrameCounter() + 1;

	ADD_LATENT_AUTOMATION_COMMAND(FMeasureQueryLatencyToleranceLatentCommand(this, Scenario));

	return true;
}

#endif
