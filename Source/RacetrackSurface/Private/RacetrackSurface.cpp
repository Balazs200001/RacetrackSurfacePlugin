// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "RacetrackSurface.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FRacetrackSurfaceModule"

void FRacetrackSurfaceModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("RacetrackSurface"));

	check(Plugin.IsValid());

	AddShaderSourceDirectoryMapping(
		TEXT("/Plugin/RacetrackSurface"),
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
}

void FRacetrackSurfaceModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRacetrackSurfaceModule, RacetrackSurface)
