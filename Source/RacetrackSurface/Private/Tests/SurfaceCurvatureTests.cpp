// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "SurfaceTrackSpace.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceCurvatureTest
{
	constexpr double CurvatureCircleRadius = 4775.0;

	constexpr int32 CurvatureCirclePoints = 16;

	constexpr int32 CurvatureProbeCount = 64;

	constexpr double CurvatureMagnitudeTolerance = 0.05;

	constexpr double OutwardProbeAlpha = 0.5;

	TStrongObjectPtr<USplineComponent> MakeCircle(bool bClockwise)
	{
		TArray<FVector> Points;
		Points.Reserve(CurvatureCirclePoints);

		for (int32 Index = 0; Index < CurvatureCirclePoints; ++Index)
		{
			const double Turn = 2.0 * UE_DOUBLE_PI * Index / CurvatureCirclePoints;
			const double Angle = bClockwise ? -Turn : Turn;

			Points.Add(FVector(
				CurvatureCircleRadius * FMath::Cos(Angle),
				CurvatureCircleRadius * FMath::Sin(Angle),
				0.0));
		}

		return RacetrackSurfaceTest::MakeSplineFromPoints(Points, true);
	}

	double RadiusInThePlane(const FVector& WorldPosition)
	{
		return FVector2D(WorldPosition.X, WorldPosition.Y).Size();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceCurvatureSignFollowsTheRightVector,
	"RacetrackSurface.Curvature.SignFollowsTheRightVector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceCurvatureSignFollowsTheRightVector::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceCurvatureTest;

	double SignOfEachDirection[2] = { 0.0, 0.0 };

	for (int32 Direction = 0; Direction < 2; ++Direction)
	{
		const bool bClockwise = Direction == 1;
		const TCHAR* Name = bClockwise ? TEXT("clockwise") : TEXT("counter-clockwise");

		const TStrongObjectPtr<USplineComponent> Circle = MakeCircle(bClockwise);
		const FSurfaceTrackRibbon Ribbon = RacetrackSurfaceTest::MakeTestRibbon(*Circle);

		const TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem(NewObject<USurfaceAccumulationSubsystem>());
		Subsystem->ConfigureTrackRibbon(Circle.Get(), Ribbon);

		double SmallestMagnitude = TNumericLimits<double>::Max();
		double LargestMagnitude = 0.0;
		int32 PositiveProbes = 0;
		int32 NegativeProbes = 0;
		int32 OutwardProbesInside = 0;

		for (int32 Probe = 0; Probe < CurvatureProbeCount; ++Probe)
		{
			const double Distance = Ribbon.TrackLength * (Probe + 0.5) / CurvatureProbeCount;
			const double Curvature = Subsystem->GetSignedCurvatureAtDistance(Distance);
			const double Magnitude = FMath::Abs(Curvature) * CurvatureCircleRadius;

			SmallestMagnitude = FMath::Min(SmallestMagnitude, Magnitude);
			LargestMagnitude = FMath::Max(LargestMagnitude, Magnitude);
			PositiveProbes += Curvature > 0.0 ? 1 : 0;
			NegativeProbes += Curvature < 0.0 ? 1 : 0;

			FSurfaceTrackCoordinate OnTheCentreline;
			OnTheCentreline.DistanceAlongCentreline = Distance;

			FSurfaceTrackCoordinate TowardTheThrow = OnTheCentreline;
			TowardTheThrow.LateralOffsetAlpha = -FMath::Sign(Curvature) * OutwardProbeAlpha;

			const double CentrelineRadius = RadiusInThePlane(
				RacetrackSurfaceTrackSpace::TrackToWorld(*Circle, Ribbon.TrackWidth, OnTheCentreline));
			const double ThrowRadius = RadiusInThePlane(
				RacetrackSurfaceTrackSpace::TrackToWorld(*Circle, Ribbon.TrackWidth, TowardTheThrow));

			OutwardProbesInside += ThrowRadius > CentrelineRadius ? 0 : 1;
		}

		SignOfEachDirection[Direction] = PositiveProbes > NegativeProbes ? 1.0 : -1.0;

		AddInfo(FString::Printf(
			TEXT("A %.0f cm circle traced %s: curvature times radius %.4f to %.4f over %d probes, %d positive and %d "
				"negative, and %d probes where the side the throw goes to is not the larger radius"),
			CurvatureCircleRadius,
			Name,
			SmallestMagnitude,
			LargestMagnitude,
			CurvatureProbeCount,
			PositiveProbes,
			NegativeProbes,
			OutwardProbesInside));

		TestTrue(
			*FString::Printf(TEXT("Traced %s, the profile reads the circle's curvature, 1/R, to within 5 %%"), Name),
			FMath::Abs(SmallestMagnitude - 1.0) <= CurvatureMagnitudeTolerance
				&& FMath::Abs(LargestMagnitude - 1.0) <= CurvatureMagnitudeTolerance);

		TestTrue(
			*FString::Printf(TEXT("Traced %s, the sign never changes round a circle"), Name),
			PositiveProbes == CurvatureProbeCount || NegativeProbes == CurvatureProbeCount);

		TestEqual(
			*FString::Printf(
				TEXT("Traced %s, the side opposite the curvature's sign against the right vector is always the "
					"larger radius, so the throw goes outward whatever the engine's handedness"),
				Name),
			OutwardProbesInside,
			0);
	}

	TestTrue(
		TEXT("The two directions round the same circle read opposite signs, so the sign follows the geometry rather "
			"than being constant"),
		SignOfEachDirection[0] != SignOfEachDirection[1]);

	return true;
}

#endif
