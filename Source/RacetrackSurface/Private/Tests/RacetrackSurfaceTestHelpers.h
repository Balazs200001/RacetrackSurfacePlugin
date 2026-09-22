#pragma once

#include "CoreMinimal.h"
#include "SurfaceRegion.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceTest
{
	constexpr double RaceSpeedCentimetresPerSecond = 5500.0;

	inline FSurfaceRegion MakeTestRegion()
	{
		FSurfaceRegion Region;
		Region.WorldCentre = FVector2D::ZeroVector;
		Region.WorldSize = 20000.0;
		Region.ResolutionTexels = 2048;
		return Region;
	}
}

#endif
