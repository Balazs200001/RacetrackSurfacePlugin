#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UObject/Object.h"
#include "RacetrackSurfaceTypes.h"
#include "SurfaceRegion.h"
#include "SurfaceAccumulationBuffer.generated.h"

class UTexture2D;

UCLASS()
class USurfaceAccumulationBuffer : public UObject
{
	GENERATED_BODY()

public:
	bool Allocate(UObject* WorldContext, const FSurfaceRegion& Region, ETextureRenderTargetFormat Format = RTF_RGBA8);

	void Release();

	bool IsAllocated() const
	{
		return Target != nullptr;
	}

	void PaintStrokes(UObject* WorldContext, const TArray<FSurfaceBrushStroke>& Strokes);

	void ApplyDecay(UObject* WorldContext, float Retention);

	void Clear(UObject* WorldContext);

	UTextureRenderTarget2D* GetTarget() const
	{
		return Target;
	}

	int32 GetAllocatedResolution() const
	{
		return AllocatedResolution;
	}

	ETextureRenderTargetFormat GetAllocatedFormat() const
	{
		return AllocatedFormat;
	}

	int64 GetAllocatedBytes() const;

	static FLinearColor ChannelMask(EAccumulationChannel Channel);

private:
	void EnsureBrush();

	void EnsureSolid();

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Target;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Brush;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Solid;

	int32 AllocatedResolution = 0;

	ETextureRenderTargetFormat AllocatedFormat = RTF_RGBA8;
};
