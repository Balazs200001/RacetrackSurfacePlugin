#pragma once

#include "Modules/ModuleManager.h"

class FRacetrackSurfaceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
