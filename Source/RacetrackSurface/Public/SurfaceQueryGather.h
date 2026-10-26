// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SurfaceTrackRibbon.h"

struct FSurfaceQueryTap
{
	FVector2D AtlasUV = FVector2D::ZeroVector;

	int32 SlotIndex = 0;

	float Weight = 0.0f;
};

namespace RacetrackSurfaceQuery
{
	static constexpr int32 TentTapsPerAxis = 3;

	static constexpr int32 AlongTrackSamplesPerTap = 2;

	static constexpr int32 TapsPerQueryPoint =
		TentTapsPerAxis * TentTapsPerAxis * AlongTrackSamplesPerTap;

	static constexpr float TotalTapWeight = 16.0f;

	static constexpr double DefaultContactPatchWidth = 20.0;

	void BuildTaps(
		const FSurfaceTrackRibbon& Ribbon,
		int32 SlotIndex,
		const FSurfaceTrackCoordinate& Coordinate,
		double ContactPatchWidth,
		TArray<FSurfaceQueryTap>& OutTaps);
}
