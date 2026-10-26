// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FRacetrackSurfaceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
