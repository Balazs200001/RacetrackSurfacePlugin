// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "SurfaceTrackSpace.h"

#include "Components/SplineComponent.h"

namespace RacetrackSurfaceTrackSpace
{
	ETrackSpaceStatus WorldToTrack(
		const USplineComponent& Centreline,
		double TrackWidth,
		const FVector& WorldPosition,
		FSurfaceTrackCoordinate& OutCoordinate)
	{
		const double HalfWidth = TrackWidth * 0.5;

		if (HalfWidth <= UE_DOUBLE_SMALL_NUMBER || Centreline.GetSplineLength() <= UE_SMALL_NUMBER)
		{
			return ETrackSpaceStatus::NoCentreline;
		}

		const float InputKey = Centreline.FindInputKeyClosestToWorldLocation(WorldPosition);
		const float Distance = Centreline.GetDistanceAlongSplineAtSplineInputKey(InputKey);

		const FVector CentrelinePosition =
			Centreline.GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		const FVector RightVector =
			Centreline.GetRightVectorAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);

		OutCoordinate.DistanceAlongCentreline = static_cast<double>(Distance);
		OutCoordinate.LateralOffsetAlpha =
			FVector::DotProduct(WorldPosition - CentrelinePosition, RightVector) / HalfWidth;

		return FMath::Abs(OutCoordinate.LateralOffsetAlpha) <= 1.0
			? ETrackSpaceStatus::Valid
			: ETrackSpaceStatus::BeyondTrackEdge;
	}

	FVector TrackToWorld(
		const USplineComponent& Centreline,
		double TrackWidth,
		const FSurfaceTrackCoordinate& Coordinate)
	{
		const float Distance = static_cast<float>(Coordinate.DistanceAlongCentreline);

		const FVector CentrelinePosition =
			Centreline.GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		const FVector RightVector =
			Centreline.GetRightVectorAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);

		return CentrelinePosition + RightVector * (Coordinate.LateralOffsetAlpha * TrackWidth * 0.5);
	}

	void WorldVelocityToTrackRates(
		const USplineComponent& Centreline,
		double TrackWidth,
		const FSurfaceTrackCoordinate& At,
		const FVector& WorldVelocity,
		double& OutCentimetresPerSecondAlongCentreline,
		double& OutLateralOffsetAlphaPerSecond)
	{
		const double HalfWidth = FMath::Max(TrackWidth * 0.5, UE_DOUBLE_SMALL_NUMBER);
		const float Distance = static_cast<float>(At.DistanceAlongCentreline);

		const FVector Forward =
			Centreline.GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
		const FVector RightVector =
			Centreline.GetRightVectorAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);

		OutCentimetresPerSecondAlongCentreline = FVector::DotProduct(WorldVelocity, Forward);
		OutLateralOffsetAlphaPerSecond = FVector::DotProduct(WorldVelocity, RightVector) / HalfWidth;
	}
}
