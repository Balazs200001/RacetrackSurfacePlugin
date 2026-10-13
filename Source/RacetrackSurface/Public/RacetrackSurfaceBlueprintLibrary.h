#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RacetrackSurfaceTypes.h"
#include "SurfaceTrackRibbon.h"
#include "RacetrackSurfaceBlueprintLibrary.generated.h"

class USplineComponent;
class USurfaceAccumulationSubsystem;
class UTextureRenderTarget2D;

UCLASS()
class RACETRACKSURFACE_API URacetrackSurfaceBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject", AdvancedDisplay = "ContactWidth"))
	static void RegisterSurfaceContact(
		const UObject* WorldContextObject,
		FVector WorldPosition,
		FVector Velocity,
		float SlipAmount,
		float ContactWidth = 20.0f);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject", AdvancedDisplay = "ContactWidth"))
	static void RegisterSurfaceContactInTrackSpace(
		const UObject* WorldContextObject,
		double DistanceAlongCentreline,
		double LateralOffsetAlpha,
		double CentimetresPerSecondAlongCentreline,
		float SlipAmount,
		float ContactWidth = 20.0f);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static void ConfigureTrackRibbon(
		const UObject* WorldContextObject,
		USplineComponent* Centreline,
		const FSurfaceTrackRibbon& Ribbon);

	UFUNCTION(BlueprintPure, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static FSurfaceTrackRibbon GetTrackRibbon(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static FSurfaceQueryHandle AcquireSurfaceQuery(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static void ReleaseSurfaceQuery(const UObject* WorldContextObject, FSurfaceQueryHandle Handle);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static FSurfaceStateSample QuerySurfaceState(
		const UObject* WorldContextObject,
		FSurfaceQueryHandle Handle,
		FVector WorldPosition);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static float QuerySurfaceCoverage(
		const UObject* WorldContextObject,
		FSurfaceQueryHandle Handle,
		FVector WorldPosition,
		EAccumulationChannel Channel = EAccumulationChannel::RubberBuildup);

	UFUNCTION(BlueprintPure, Category = "Racetrack Surface")
	static float GetSurfaceStateChannel(const FSurfaceStateSample& Sample, EAccumulationChannel Channel);

	UFUNCTION(BlueprintPure, Category = "Racetrack Surface")
	static bool IsSurfaceStateUsable(const FSurfaceStateSample& Sample);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static void ResetSurface(const UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static bool SaveSurfaceState(const UObject* WorldContextObject, const FString& FilePath);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static bool LoadSurfaceState(const UObject* WorldContextObject, const FString& FilePath);

	UFUNCTION(BlueprintCallable, Category = "Racetrack Surface",
		meta = (WorldContext = "WorldContextObject"))
	static UTextureRenderTarget2D* GetAccumulationTarget(const UObject* WorldContextObject);

	static USurfaceAccumulationSubsystem* FindSubsystem(const UObject* WorldContextObject);
};
