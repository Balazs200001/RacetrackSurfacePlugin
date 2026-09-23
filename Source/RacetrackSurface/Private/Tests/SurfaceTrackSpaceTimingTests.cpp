#include "Misc/AutomationTest.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "SurfaceTrackRibbon.h"
#include "SurfaceTrackSpace.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"
#include "Tests/SurfaceCpuTiming.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::MakeClosedCircleSpline;
using RacetrackSurfaceTest::SampleCpuWork;
using RacetrackSurfaceTrackSpace::TrackToWorld;
using RacetrackSurfaceTrackSpace::WorldToTrack;

namespace
{
	constexpr double CircuitRadius = 15000.0;
	constexpr double CircuitTrackWidth = 1200.0;
	constexpr double FrameMillisecondsAtSixtyHertz = 1000.0 / 60.0;

	const int32 SplinePointCandidates[] = { 18, 64, 77, 128, 192, 256, 384, 1024 };
	const int32 ContactCountCandidates[] = { 4, 80 };
	constexpr int32 CrossoverContactCount = 80;
	constexpr double CrossoverBudgetMilliseconds = 1.0;

	TArray<FVector> MakeContactPositions(const USplineComponent& Circuit, int32 ContactCount)
	{
		const double Length = Circuit.GetSplineLength();

		TArray<FVector> Positions;
		Positions.Reserve(ContactCount);

		for (int32 Index = 0; Index < ContactCount; ++Index)
		{
			FSurfaceTrackCoordinate Coordinate;
			Coordinate.DistanceAlongCentreline = Length * Index / ContactCount;
			Coordinate.LateralOffsetAlpha = -0.8 + 1.6 * (Index % 5) / 4.0;

			Positions.Add(TrackToWorld(Circuit, CircuitTrackWidth, Coordinate));
		}

		return Positions;
	}

	double InterpolateCrossoverPointCount(
		const TArray<int32>& PointCounts,
		const TArray<double>& Costs,
		double BudgetMilliseconds)
	{
		for (int32 Index = 1; Index < PointCounts.Num(); ++Index)
		{
			const double Below = Costs[Index - 1];
			const double Above = Costs[Index];

			if (Below >= BudgetMilliseconds || Above < BudgetMilliseconds)
			{
				continue;
			}

			const double Span = Above - Below;
			const double Fraction = Span > UE_DOUBLE_SMALL_NUMBER ? (BudgetMilliseconds - Below) / Span : 0.0;

			return PointCounts[Index - 1] + Fraction * (PointCounts[Index] - PointCounts[Index - 1]);
		}

		return 0.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrackSpaceConversionCost,
	"RacetrackSurface.Performance.TrackSpaceConversion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrackSpaceConversionCost::RunTest(const FString& Parameters)
{
	AddInfo(TEXT("Spline points | Contacts | Full search | per contact | Known segment | per contact | Speedup"));

	double WorstHarnessScaleCost = 0.0;

	TArray<int32> CrossoverPointCounts;
	TArray<double> CrossoverCosts;

	for (const int32 PointCount : SplinePointCandidates)
	{
		const TStrongObjectPtr<USplineComponent> Circuit = MakeClosedCircleSpline(CircuitRadius, PointCount);

		if (Circuit->GetSplineLength() <= 0.0)
		{
			AddError(FString::Printf(TEXT("Failed to build a %d point circuit"), PointCount));
			continue;
		}

		for (const int32 ContactCount : ContactCountCandidates)
		{
			const TArray<FVector> Positions = MakeContactPositions(*Circuit, ContactCount);

			TArray<int32> KnownSegments;
			KnownSegments.Reserve(ContactCount);

			for (const FVector& Position : Positions)
			{
				KnownSegments.Add(FMath::FloorToInt32(Circuit->FindInputKeyClosestToWorldLocation(Position)));
			}

			double SearchAccumulator = 0.0;
			double SearchMedian = 0.0;
			double SearchMinimum = 0.0;

			SampleCpuWork(
				[&]()
				{
					for (const FVector& Position : Positions)
					{
						FSurfaceTrackCoordinate Coordinate;
						WorldToTrack(*Circuit, CircuitTrackWidth, Position, Coordinate);
						SearchAccumulator += Coordinate.DistanceAlongCentreline;
					}
				},
				SearchMedian,
				SearchMinimum);

			double SegmentAccumulator = 0.0;
			double SegmentMedian = 0.0;
			double SegmentMinimum = 0.0;

			SampleCpuWork(
				[&]()
				{
					for (int32 Index = 0; Index < Positions.Num(); ++Index)
					{
						SegmentAccumulator += Circuit->FindInputKeyOnSegmentClosestToWorldLocation(
							Positions[Index], KnownSegments[Index]);
					}
				},
				SegmentMedian,
				SegmentMinimum);

			AddInfo(FString::Printf(
				TEXT("%d | %d | %.4f ms | %.4f ms | %.4f ms | %.4f ms | %.1fx"),
				PointCount,
				ContactCount,
				SearchMedian,
				SearchMedian / ContactCount,
				SegmentMedian,
				SegmentMedian / ContactCount,
				SegmentMedian > 0.0 ? SearchMedian / SegmentMedian : 0.0));

			if (PointCount == SplinePointCandidates[0])
			{
				WorstHarnessScaleCost = FMath::Max(WorstHarnessScaleCost, SearchMedian);
			}

			if (ContactCount == CrossoverContactCount)
			{
				CrossoverPointCounts.Add(PointCount);
				CrossoverCosts.Add(SearchMedian);
			}

			TestTrue(
				TEXT("The timed work was not optimised away"),
				SearchAccumulator != 0.0 && SegmentAccumulator != 0.0);
		}
	}

	AddInfo(FString::Printf(
		TEXT("Worst cost at harness spline complexity: %.4f ms, %.2f %% of a 60 Hz frame"),
		WorstHarnessScaleCost,
		100.0 * WorstHarnessScaleCost / FrameMillisecondsAtSixtyHertz));

	const double CrossoverPointCount = InterpolateCrossoverPointCount(
		CrossoverPointCounts, CrossoverCosts, CrossoverBudgetMilliseconds);

	if (CrossoverPointCount > 0.0)
	{
		AddInfo(FString::Printf(
			TEXT("Full search at %d contacts reaches %.1f ms at %.0f spline points"),
			CrossoverContactCount,
			CrossoverBudgetMilliseconds,
			CrossoverPointCount));
	}
	else
	{
		AddInfo(FString::Printf(
			TEXT("Full search at %d contacts does not reach %.1f ms anywhere in the sampled range"),
			CrossoverContactCount,
			CrossoverBudgetMilliseconds));
	}

	if (WorstHarnessScaleCost > 1.0)
	{
		AddWarning(FString::Printf(
			TEXT("World to track conversion costs %.4f ms at harness spline complexity, which is over the 1 ms ")
			TEXT("threshold this measurement was taken to check. The per-contact segment cache is needed now, ")
			TEXT("not later."),
			WorstHarnessScaleCost));
	}

	return true;
}

#endif
