#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Subsystems/WorldSubsystem.h"
#include "RacetrackSurfaceTypes.h"
#include "SurfaceTrackRibbon.h"
#include "SurfaceAccumulationSubsystem.generated.h"

class USplineComponent;
class USurfaceAccumulationBuffer;
class UTextureRenderTarget2D;

RACETRACKSURFACE_API DECLARE_LOG_CATEGORY_EXTERN(LogRacetrackSurface, Log, All);

UCLASS()
class RACETRACKSURFACE_API USurfaceAccumulationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;

	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(USurfaceAccumulationSubsystem, STATGROUP_Tickables);
	}

	static float GetDepositionScale();

	void ConfigureTrackRibbon(const USplineComponent* NewCentreline, const FSurfaceTrackRibbon& NewRibbon);

	const FSurfaceTrackRibbon& GetRibbon() const
	{
		return Ribbon;
	}

	const USplineComponent* GetCentreline() const
	{
		return Centreline.Get();
	}

	bool RegisterSurfaceContact(const FSurfaceContact& Contact);

	bool RegisterSurfaceContact(
		const FVector& WorldPosition,
		const FVector& Velocity,
		float SlipAmount,
		float ContactWidth,
		EAccumulationChannel Channel);

	bool RegisterSurfaceContactInTrackSpace(const FSurfaceTrackContact& Contact);

	bool RegisterSurfaceContactInTrackSpace(
		double DistanceAlongCentreline,
		double LateralOffsetAlpha,
		double CentimetresPerSecondAlongCentreline,
		float SlipAmount,
		float ContactWidth,
		EAccumulationChannel Channel);

	void ResetSurface();

	int32 GetPendingContactCount() const
	{
		return PendingContacts.Num();
	}

	const TArray<FSurfaceTrackContact>& GetPendingContacts() const
	{
		return PendingContacts;
	}

	int32 GetContactsDiscardedOutsideRibbon() const
	{
		return ContactsDiscardedOutsideRibbon;
	}

	int32 GetContactsDiscardedWithoutCentreline() const
	{
		return ContactsDiscardedWithoutCentreline;
	}

	const TArray<FSurfaceBrushStroke>& GetStrokesResolvedThisFrame() const
	{
		return StrokesResolvedThisFrame;
	}

	void BuildStrokes(
		const FSurfaceTrackContact& Contact,
		float DeltaSeconds,
		TArray<FSurfaceBrushStroke>& OutStrokes) const;

	USurfaceAccumulationBuffer* GetBuffer() const
	{
		return Buffer;
	}

	UTextureRenderTarget2D* GetOrCreateAccumulationTarget();

private:
	void FlushPendingContacts(float DeltaSeconds);

	bool EnsureBuffer();

	void PublishRibbonToMaterials();

	void AppendStroke(
		const FSurfaceTrackContact& Contact,
		double FromDistance,
		double FromAlpha,
		double ToDistance,
		double ToAlpha,
		TArray<FSurfaceBrushStroke>& OutStrokes) const;

	FSurfaceTrackRibbon Ribbon;

	TWeakObjectPtr<const USplineComponent> Centreline;

	UPROPERTY(Transient)
	TObjectPtr<USurfaceAccumulationBuffer> Buffer;

	TArray<FSurfaceTrackContact> PendingContacts;

	TArray<FSurfaceBrushStroke> StrokesResolvedThisFrame;

	int32 ContactsDiscardedOutsideRibbon = 0;

	int32 ContactsDiscardedWithoutCentreline = 0;
};
