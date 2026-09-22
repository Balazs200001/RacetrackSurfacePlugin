#pragma once

#include "CoreMinimal.h"
#include "SurfaceTrackRibbon.generated.h"

UENUM(BlueprintType)
enum class ETrackSpaceStatus : uint8
{
	Valid,
	BeyondTrackEdge,
	NoCentreline
};

USTRUCT(BlueprintType)
struct FSurfaceTrackCoordinate
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	double DistanceAlongCentreline = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	double LateralOffsetAlpha = 0.0;
};

USTRUCT(BlueprintType)
struct FSurfaceTrackRibbon
{
	GENERATED_BODY()

	static constexpr int32 MaximumTexelsPerRow = 16384;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "100.0"))
	double TrackLength = 30000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "100.0"))
	double TrackWidth = 1200.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "0.5"))
	double CentimetresPerTexel = 9.765625;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface")
	bool bClosedLoop = true;

	double SafeCentimetresPerTexel() const
	{
		return FMath::Max(CentimetresPerTexel, UE_DOUBLE_SMALL_NUMBER);
	}

	int32 AlongTrackTexels() const
	{
		return FMath::Max(FMath::CeilToInt32(TrackLength / SafeCentimetresPerTexel()), 1);
	}

	int32 AcrossTrackTexels() const
	{
		return FMath::Max(FMath::CeilToInt32(TrackWidth / SafeCentimetresPerTexel()), 1);
	}

	bool FitsUnfolded() const
	{
		return AlongTrackTexels() <= MaximumTexelsPerRow;
	}

	int64 TexelCount() const
	{
		return static_cast<int64>(AlongTrackTexels()) * static_cast<int64>(AcrossTrackTexels());
	}

	double MaximumUnfoldedTrackLength() const
	{
		return static_cast<double>(MaximumTexelsPerRow) * SafeCentimetresPerTexel();
	}

	double WrapDistance(double DistanceAlongCentreline) const
	{
		const double Length = FMath::Max(TrackLength, UE_DOUBLE_SMALL_NUMBER);

		if (!bClosedLoop)
		{
			return FMath::Clamp(DistanceAlongCentreline, 0.0, Length);
		}

		const double Wrapped = FMath::Fmod(DistanceAlongCentreline, Length);

		return Wrapped < 0.0 ? Wrapped + Length : Wrapped;
	}

	bool ContainsLateralOffset(double LateralOffsetAlpha) const
	{
		return FMath::Abs(LateralOffsetAlpha) <= 1.0;
	}

	bool Contains(const FSurfaceTrackCoordinate& Coordinate) const
	{
		if (!ContainsLateralOffset(Coordinate.LateralOffsetAlpha))
		{
			return false;
		}

		return bClosedLoop
			|| (Coordinate.DistanceAlongCentreline >= 0.0 && Coordinate.DistanceAlongCentreline <= TrackLength);
	}

	double NormalisedAlongTrack(double DistanceAlongCentreline) const
	{
		return DistanceAlongCentreline / FMath::Max(TrackLength, UE_DOUBLE_SMALL_NUMBER);
	}

	static double NormalisedAcrossTrack(double LateralOffsetAlpha)
	{
		return LateralOffsetAlpha * 0.5 + 0.5;
	}

	FVector2D TrackToNormalised(const FSurfaceTrackCoordinate& Coordinate) const
	{
		return FVector2D(
			NormalisedAlongTrack(WrapDistance(Coordinate.DistanceAlongCentreline)),
			NormalisedAcrossTrack(Coordinate.LateralOffsetAlpha));
	}

	double WorldDistanceToTexels(double WorldDistance) const
	{
		return WorldDistance / SafeCentimetresPerTexel();
	}

	double WorldDistanceToNormalisedAlongTrack(double WorldDistance) const
	{
		return WorldDistance / FMath::Max(TrackLength, UE_DOUBLE_SMALL_NUMBER);
	}
};
