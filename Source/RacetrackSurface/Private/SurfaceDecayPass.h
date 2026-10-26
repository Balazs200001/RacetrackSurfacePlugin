// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FTextureRenderTargetResource;

namespace RacetrackSurfaceDecayPass
{
	void Enqueue(FTextureRenderTargetResource* Target, float HalfLivesElapsed, float SaturationConstant);
}
