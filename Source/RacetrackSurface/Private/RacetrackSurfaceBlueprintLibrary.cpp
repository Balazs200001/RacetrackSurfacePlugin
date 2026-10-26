// Copyright 2026 Balázs Fábián. All Rights Reserved.

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

FSurfaceQueryHandle URacetrackSurfaceBlueprintLibrary::AcquireSurfaceQuery(const UObject* WorldContextObject)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		return Subsystem->AcquireSurfaceQuery();
	}

	return FSurfaceQueryHandle();
}

void URacetrackSurfaceBlueprintLibrary::ReleaseSurfaceQuery(
	const UObject* WorldContextObject,
	FSurfaceQueryHandle Handle)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->ReleaseSurfaceQuery(Handle);
	}
}

FSurfaceStateSample URacetrackSurfaceBlueprintLibrary::QuerySurfaceState(
	const UObject* WorldContextObject,
	FSurfaceQueryHandle Handle,
	FVector WorldPosition)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		return Subsystem->QuerySurfaceState(Handle, WorldPosition);
	}

	return FSurfaceStateSample();
}

float URacetrackSurfaceBlueprintLibrary::QuerySurfaceCoverage(
	const UObject* WorldContextObject,
	FSurfaceQueryHandle Handle,
	FVector WorldPosition,
	EAccumulationChannel Channel)
{
	return QuerySurfaceState(WorldContextObject, Handle, WorldPosition).GetChannel(Channel);
}

float URacetrackSurfaceBlueprintLibrary::GetSurfaceStateChannel(
	const FSurfaceStateSample& Sample,
	EAccumulationChannel Channel)
{
	return Sample.GetChannel(Channel);
}

bool URacetrackSurfaceBlueprintLibrary::IsSurfaceStateUsable(const FSurfaceStateSample& Sample)
{
	return Sample.IsUsable();
}

void URacetrackSurfaceBlueprintLibrary::RegisterSurfaceContact(
	const UObject* WorldContextObject,
	FVector WorldPosition,
	FVector Velocity,
	float SlipAmount,
	float ContactWidth)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->RegisterSurfaceContact(WorldPosition, Velocity, SlipAmount, ContactWidth);
	}
}

void URacetrackSurfaceBlueprintLibrary::RegisterSurfaceContactInTrackSpace(
	const UObject* WorldContextObject,
	double DistanceAlongCentreline,
	double LateralOffsetAlpha,
	double CentimetresPerSecondAlongCentreline,
	float SlipAmount,
	float ContactWidth)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->RegisterSurfaceContactInTrackSpace(
			DistanceAlongCentreline,
			LateralOffsetAlpha,
			CentimetresPerSecondAlongCentreline,
			SlipAmount,
			ContactWidth);
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

bool URacetrackSurfaceBlueprintLibrary::SaveSurfaceState(const UObject* WorldContextObject, const FString& FilePath)
{
	USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject);

	return Subsystem != nullptr && Subsystem->SaveSurfaceState(FilePath);
}

bool URacetrackSurfaceBlueprintLibrary::LoadSurfaceState(const UObject* WorldContextObject, const FString& FilePath)
{
	USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject);

	return Subsystem != nullptr && Subsystem->LoadSurfaceState(FilePath);
}

void URacetrackSurfaceBlueprintLibrary::SetDecayRate(const UObject* WorldContextObject, float Rate)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		Subsystem->SetDecayRate(Rate);
	}
}

float URacetrackSurfaceBlueprintLibrary::GetDecayRate(const UObject* WorldContextObject)
{
	const USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject);

	return Subsystem != nullptr ? Subsystem->GetDecayRate() : 0.0f;
}

UTextureRenderTarget2D* URacetrackSurfaceBlueprintLibrary::GetAccumulationTarget(const UObject* WorldContextObject)
{
	if (USurfaceAccumulationSubsystem* Subsystem = FindSubsystem(WorldContextObject))
	{
		return Subsystem->GetOrCreateAccumulationTarget();
	}

	return nullptr;
}
