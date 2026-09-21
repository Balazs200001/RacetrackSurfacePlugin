#include "RacetrackSurfaceBlueprintLibrary.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "SurfaceAccumulationSubsystem.h"

USurfaceAccumulationSubsystem* URacetrackSurfaceBlueprintLibrary::FindSubsystem(const UObject* WorldContextObject)
{
	if (GEngine == nullptr)
	{
		return nullptr;
	}

	const UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (World == nullptr)
	{
		return nullptr;
	}

	return World->GetSubsystem<USurfaceAccumulationSubsystem>();
}

void URacetrackSurfaceBlueprintLibrary::RegisterSurfaceContact(
	const UObject* WorldContextObject,
	FVector WorldPosition,
	FVector Velocity,
	float SlipAmount,
	float ContactWidth,
	EAccumulationChannel Channel)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->RegisterSurfaceContact(WorldPosition, Velocity, SlipAmount, ContactWidth, Channel);
	}
}

void URacetrackSurfaceBlueprintLibrary::ConfigureSurfaceRegion(const UObject* WorldContextObject, const FSurfaceRegion& Region)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->ConfigureRegion(Region);
	}
}

FSurfaceRegion URacetrackSurfaceBlueprintLibrary::GetSurfaceRegion(const UObject* WorldContextObject)
{
	if (const USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		return Subsystem->GetRegion();
	}

	return FSurfaceRegion();
}

void URacetrackSurfaceBlueprintLibrary::ResetSurface(const UObject* WorldContextObject)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->ResetSurface();
	}
}
