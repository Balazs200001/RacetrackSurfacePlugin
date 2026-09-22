#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Subsystems/WorldSubsystem.h"
#include "RacetrackSurfaceTypes.h"
#include "SurfaceRegion.h"
#include "SurfaceAccumulationSubsystem.generated.h"

class USurfaceAccumulationBuffer;

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

	void RegisterSurfaceContact(const FSurfaceContact& Contact);

	void RegisterSurfaceContact(
		const FVector& WorldPosition,
		const FVector& Velocity,
		float SlipAmount,
		float ContactWidth,
		EAccumulationChannel Channel);

	void ConfigureRegion(const FSurfaceRegion& NewRegion);

	const FSurfaceRegion& GetRegion() const
	{
		return Region;
	}

	void ResetSurface();

	int32 GetPendingContactCount() const
	{
		return PendingContacts.Num();
	}

	const TArray<FSurfaceBrushStroke>& GetStrokesResolvedThisFrame() const
	{
		return StrokesResolvedThisFrame;
	}

	bool BuildStroke(const FSurfaceContact& Contact, float DeltaSeconds, FSurfaceBrushStroke& OutStroke) const;

	USurfaceAccumulationBuffer* GetBuffer() const
	{
		return Buffer;
	}

private:
	void FlushPendingContacts(float DeltaSeconds);

	bool EnsureBuffer();

	FSurfaceRegion Region;

	UPROPERTY(Transient)
	TObjectPtr<USurfaceAccumulationBuffer> Buffer;

	TArray<FSurfaceContact> PendingContacts;

	TArray<FSurfaceBrushStroke> StrokesResolvedThisFrame;

	int32 ContactsDiscardedOutsideRegion = 0;
};
