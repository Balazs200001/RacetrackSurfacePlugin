#pragma once

#include "CoreMinimal.h"
#include "SurfaceTrackRibbon.h"
#include "RacetrackSurfaceTypes.generated.h"

UENUM(BlueprintType)
enum class EAccumulationChannel : uint8
{
	RubberBuildup,
	SkidMark,
	Marbles
};

UENUM(BlueprintType)
enum class ESurfacePaintMode : uint8
{
	Additive,
	Saturating
};

UENUM(BlueprintType)
enum class ESurfaceSampleStatus : uint8
{
	Valid,
	OutsideRegion,
	NotYetResolved
};

USTRUCT(BlueprintType)
struct FSurfaceContact
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	FVector WorldPosition = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlipAmount = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "0.0"))
	float ContactWidth = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	EAccumulationChannel Channel = EAccumulationChannel::SkidMark;
};

USTRUCT(BlueprintType)
struct FSurfaceTrackContact
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	FSurfaceTrackCoordinate Coordinate;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	double CentimetresPerSecondAlongCentreline = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	double LateralOffsetAlphaPerSecond = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlipAmount = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "0.0"))
	float ContactWidth = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	EAccumulationChannel Channel = EAccumulationChannel::SkidMark;
};

USTRUCT(BlueprintType)
struct FSurfaceStateSample
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	float Amount = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	ESurfaceSampleStatus Status = ESurfaceSampleStatus::NotYetResolved;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	float SecondsSinceResolved = 0.0f;

	bool IsUsable() const
	{
		return Status == ESurfaceSampleStatus::Valid;
	}
};

USTRUCT(BlueprintType)
struct FSurfaceBrushStroke
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	FVector2D RegionStart = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	FVector2D RegionEnd = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	float WidthInTexels = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	float Intensity = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	int32 RowIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Racetrack Surface")
	EAccumulationChannel Channel = EAccumulationChannel::SkidMark;
};
