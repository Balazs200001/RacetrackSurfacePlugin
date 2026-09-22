#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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
