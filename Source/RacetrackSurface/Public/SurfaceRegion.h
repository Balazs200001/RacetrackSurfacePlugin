#pragma once

#include "CoreMinimal.h"
#include "SurfaceRegion.generated.h"

USTRUCT(BlueprintType)
struct FSurfaceRegion
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	FVector2D WorldCentre = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "100.0"))
	double WorldSize = 20000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "64", ClampMax = "8192"))
	int32 ResolutionTexels = 2048;

	FVector2D WorldMin() const
	{
		return WorldCentre - FVector2D(WorldSize * 0.5, WorldSize * 0.5);
	}

	FVector2D WorldToNormalised(const FVector& WorldPosition) const
	{
		const FVector2D Min = WorldMin();
		return FVector2D(
			(WorldPosition.X - Min.X) / WorldSize,
			(WorldPosition.Y - Min.Y) / WorldSize);
	}

	FVector2D NormalisedToTexel(const FVector2D& Normalised) const
	{
		return Normalised * static_cast<double>(ResolutionTexels);
	}

	FVector2D WorldToTexel(const FVector& WorldPosition) const
	{
		return NormalisedToTexel(WorldToNormalised(WorldPosition));
	}

	bool ContainsNormalised(const FVector2D& Normalised) const
	{
		return Normalised.X >= 0.0 && Normalised.X <= 1.0
			&& Normalised.Y >= 0.0 && Normalised.Y <= 1.0;
	}

	bool ContainsWorldPosition(const FVector& WorldPosition) const
	{
		return ContainsNormalised(WorldToNormalised(WorldPosition));
	}

	double CentimetresPerTexel() const
	{
		return WorldSize / static_cast<double>(FMath::Max(ResolutionTexels, 1));
	}

	double WorldDistanceToNormalised(double WorldDistance) const
	{
		return WorldDistance / WorldSize;
	}

	double WorldDistanceToTexels(double WorldDistance) const
	{
		return WorldDistance / CentimetresPerTexel();
	}
};
