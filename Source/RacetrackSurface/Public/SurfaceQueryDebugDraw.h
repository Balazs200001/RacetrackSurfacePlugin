// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RacetrackSurfaceTypes.h"

class UWorld;

namespace RacetrackSurfaceQueryOverlay
{
	static constexpr int32 BarCount = AccumulationChannelCount;

	struct FOverlayBasis
	{
		FVector Along = FVector::ForwardVector;

		FVector Across = FVector::RightVector;

		FVector Up = FVector::UpVector;
	};

	RACETRACKSURFACE_API bool IsEnabled();

	RACETRACKSURFACE_API float GetFullScale();

	RACETRACKSURFACE_API float GetBarLength();

	RACETRACKSURFACE_API FColor ChannelColour(EAccumulationChannel Channel);

	RACETRACKSURFACE_API FColor FrameColour(ESurfaceSampleStatus Status, EAccumulationChannel Channel);

	RACETRACKSURFACE_API float BarFillFraction(float Coverage, float FullScale);

	RACETRACKSURFACE_API void DrawSurfaceStateSample(
		const UWorld* World,
		const FVector& DrawPosition,
		const FOverlayBasis& Basis,
		const FSurfaceStateSample& Sample);
}
