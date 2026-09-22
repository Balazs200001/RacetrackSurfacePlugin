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

void URacetrackSurfaceBlueprintLibrary::RegisterSurfaceContactInTrackSpace(
	const UObject* WorldContextObject,
	double DistanceAlongCentreline,
	double LateralOffsetAlpha,
	double CentimetresPerSecondAlongCentreline,
	float SlipAmount,
	float ContactWidth,
	EAccumulationChannel Channel)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->RegisterSurfaceContactInTrackSpace(
			DistanceAlongCentreline,
			LateralOffsetAlpha,
			CentimetresPerSecondAlongCentreline,
			SlipAmount,
			ContactWidth,
			Channel);
	}
}

void URacetrackSurfaceBlueprintLibrary::ConfigureTrackRibbon(
	const UObject* WorldContextObject,
	USplineComponent* Centreline,
	const FSurfaceTrackRibbon& Ribbon)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->ConfigureTrackRibbon(Centreline, Ribbon);
	}
}

FSurfaceTrackRibbon URacetrackSurfaceBlueprintLibrary::GetTrackRibbon(const UObject* WorldContextObject)
{
	if (const USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		return Subsystem->GetRibbon();
	}

	return FSurfaceTrackRibbon();
}

void URacetrackSurfaceBlueprintLibrary::ResetSurface(const UObject* WorldContextObject)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->ResetSurface();
	}
}

UTextureRenderTarget2D* URacetrackSurfaceBlueprintLibrary::GetAccumulationTarget(const UObject* WorldContextObject)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		return Subsystem->GetOrCreateAccumulationTarget();
	}

	return nullptr;
}
