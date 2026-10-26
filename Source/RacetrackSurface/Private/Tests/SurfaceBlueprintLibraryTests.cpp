// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "RacetrackSurfaceBlueprintLibrary.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceBlueprintDecayRateReachesTheSubsystem,
	"RacetrackSurface.Blueprint.DecayRateReachesTheSubsystem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceBlueprintDecayRateReachesTheSubsystem::RunTest(const FString& Parameters)
{
	UWorld* World = RacetrackSurfaceTest::FindRenderableWorld();

	USurfaceAccumulationSubsystem* Subsystem =
		World != nullptr ? World->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

	if (Subsystem == nullptr)
	{
		AddError(TEXT("No world or subsystem available."));
		return false;
	}

	const float RateToRestore = Subsystem->GetDecayRate();

	URacetrackSurfaceBlueprintLibrary::SetDecayRate(World, 3.0f);
	const float SubsystemRate = Subsystem->GetDecayRate();
	const float LibraryRate = URacetrackSurfaceBlueprintLibrary::GetDecayRate(World);

	URacetrackSurfaceBlueprintLibrary::SetDecayRate(World, -1.0f);
	const float RateAfterANegativeSet = URacetrackSurfaceBlueprintLibrary::GetDecayRate(World);

	Subsystem->SetDecayRate(RateToRestore);

	AddInfo(FString::Printf(
		TEXT("The library set 3: the subsystem reads %.1f, the library reads %.1f. The library set -1: reads %.1f"),
		SubsystemRate,
		LibraryRate,
		RateAfterANegativeSet));

	TestEqual(TEXT("The library's setter sets what the world's subsystem reads"), SubsystemRate, 3.0f);

	TestEqual(TEXT("The library's getter reads the world's subsystem"), LibraryRate, 3.0f);

	TestEqual(TEXT("A negative rate from Blueprint is held as 0"), RateAfterANegativeSet, 0.0f);

	return true;
}

#endif
