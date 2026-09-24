#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UObject/Object.h"
#include "RacetrackSurfaceTypes.h"
#include "SurfaceTrackRibbon.h"
#include "SurfaceAccumulationBuffer.generated.h"

class UTexture2D;
struct FSurfaceQueryTap;

UCLASS()
class USurfaceAccumulationBuffer : public UObject
{
	GENERATED_BODY()

public:
	bool Allocate(UObject* WorldContext, int32 Width, int32 Height, ETextureRenderTargetFormat Format = RTF_RGBA16f, int32 RowCount = 1);

	bool Allocate(UObject* WorldContext, const FSurfaceTrackRibbon& Ribbon, ETextureRenderTargetFormat Format = RTF_RGBA16f);

	void Release();

	bool IsAllocated() const
	{
		return Target != nullptr;
	}

	void PaintStrokes(
		UObject* WorldContext,
		const TArray<FSurfaceBrushStroke>& Strokes,
		ESurfacePaintMode PaintMode = ESurfacePaintMode::Additive);

	void SweepStrokes(
		UObject* WorldContext,
		const TArray<FSurfaceBrushStroke>& Strokes,
		EAccumulationChannel SweptChannel,
		float Retention);

	void ApplyDecay(UObject* WorldContext, float Retention);

	void Clear(UObject* WorldContext);

	UTextureRenderTarget2D* EnsureGatherTarget(UObject* WorldContext, int32 SlotCount);

	void GatherTaps(UObject* WorldContext, const TArray<FSurfaceQueryTap>& Taps, int32 SlotCount);

	UTextureRenderTarget2D* GetTarget() const
	{
		return Target;
	}

	UTextureRenderTarget2D* GetGatherTarget() const
	{
		return GatherTarget;
	}

	int32 GetAllocatedWidth() const
	{
		return AllocatedWidth;
	}

	int32 GetAllocatedHeight() const
	{
		return AllocatedHeight;
	}

	ETextureRenderTargetFormat GetAllocatedFormat() const
	{
		return AllocatedFormat;
	}

	int32 GetAllocatedRowCount() const
	{
		return AllocatedRowCount;
	}

	FIntRect GetRowBounds(int32 RowIndex) const;

	int64 GetAllocatedBytes() const;

	static RACETRACKSURFACE_API FLinearColor ChannelMask(EAccumulationChannel Channel);

private:
	void EnsureBrush();

	void EnsureSolid();

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Target;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> GatherTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Brush;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Solid;

	int32 AllocatedWidth = 0;

	int32 AllocatedHeight = 0;

	int32 AllocatedRowCount = 1;

	ETextureRenderTargetFormat AllocatedFormat = RTF_RGBA16f;
};
