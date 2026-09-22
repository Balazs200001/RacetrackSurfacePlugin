#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "SurfaceTrackRibbon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceTest
{
	constexpr double RaceSpeedCentimetresPerSecond = 5500.0;

	constexpr double HarnessTrackWidth = 1200.0;

	inline TStrongObjectPtr<USplineComponent> MakeSplineFromPoints(const TArray<FVector>& Points, bool bClosedLoop)
	{
		TStrongObjectPtr<USplineComponent> Spline(NewObject<USplineComponent>());

		Spline->ClearSplinePoints(false);

		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Spline->AddSplinePoint(Points[Index], ESplineCoordinateSpace::Local, false);
			Spline->SetSplinePointType(Index, ESplinePointType::Curve, false);
		}

		Spline->SetClosedLoop(bClosedLoop, true);

		return Spline;
	}

	inline TStrongObjectPtr<USplineComponent> MakeClosedCircleSpline(double Radius, int32 PointCount)
	{
		TArray<FVector> Points;
		Points.Reserve(PointCount);

		for (int32 Index = 0; Index < PointCount; ++Index)
		{
			const double Angle = 2.0 * UE_DOUBLE_PI * Index / PointCount;
			Points.Add(FVector(Radius * FMath::Cos(Angle), Radius * FMath::Sin(Angle), 0.0));
		}

		return MakeSplineFromPoints(Points, true);
	}

	inline TStrongObjectPtr<USplineComponent> MakeTestCircuit()
	{
		return MakeClosedCircleSpline(4775.0, 16);
	}

	inline FSurfaceTrackRibbon MakeTestRibbon(const USplineComponent& Centreline)
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = Centreline.GetSplineLength();
		Ribbon.TrackWidth = HarnessTrackWidth;
		Ribbon.bClosedLoop = Centreline.IsClosedLoop();
		return Ribbon;
	}

	inline FSurfaceTrackRibbon MakeSquareTestRibbon(int32 Resolution)
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = Resolution * Ribbon.CentimetresPerTexel;
		Ribbon.TrackWidth = Resolution * Ribbon.CentimetresPerTexel;
		return Ribbon;
	}

	inline UWorld* FindRenderableWorld()
	{
		if (GEngine == nullptr)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			const bool bUsableType = Context.WorldType == EWorldType::Editor
				|| Context.WorldType == EWorldType::Game
				|| Context.WorldType == EWorldType::PIE;

			if (bUsableType && Context.World() != nullptr)
			{
				return Context.World();
			}
		}

		return nullptr;
	}
}

#endif
