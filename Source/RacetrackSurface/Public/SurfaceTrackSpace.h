// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SurfaceTrackRibbon.h"

class USplineComponent;

namespace RacetrackSurfaceTrackSpace
{
	RACETRACKSURFACE_API ETrackSpaceStatus WorldToTrack(
		const USplineComponent& Centreline,
		double TrackWidth,
		const FVector& WorldPosition,
		FSurfaceTrackCoordinate& OutCoordinate);

	RACETRACKSURFACE_API FVector TrackToWorld(
		const USplineComponent& Centreline,
		double TrackWidth,
		const FSurfaceTrackCoordinate& Coordinate);

	RACETRACKSURFACE_API void WorldVelocityToTrackRates(
		const USplineComponent& Centreline,
		double TrackWidth,
		const FSurfaceTrackCoordinate& At,
		const FVector& WorldVelocity,
		double& OutCentimetresPerSecondAlongCentreline,
		double& OutLateralOffsetAlphaPerSecond);
}
