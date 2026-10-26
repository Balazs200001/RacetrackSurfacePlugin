// Copyright 2026 Balázs Fábián. All Rights Reserved.

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
	double TrackLength = 180000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "100.0"))
	double TrackWidth = 800.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Racetrack Surface", meta = (ClampMin = "0.5"))
	double CentimetresPerTexel = 3.125;

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

	int32 RowCount() const
	{
		return FMath::DivideAndRoundUp(AlongTrackTexels(), MaximumTexelsPerRow);
	}

	int32 TexelsPerRow() const
	{
		return FMath::DivideAndRoundUp(AlongTrackTexels(), RowCount());
	}

	int32 AtlasWidth() const
	{
		return TexelsPerRow();
	}

	int32 AtlasHeight() const
	{
		return RowCount() * AcrossTrackTexels();
	}

	int64 AtlasTexelCount() const
	{
		return static_cast<int64>(AtlasWidth()) * static_cast<int64>(AtlasHeight());
	}

	bool FitsUnfolded() const
	{
		return RowCount() == 1;
	}

	bool FitsTheTextureCap() const
	{
		return AtlasWidth() <= MaximumTexelsPerRow && AtlasHeight() <= MaximumTexelsPerRow;
	}

	double MaximumUnfoldedTrackLength() const
	{
		return static_cast<double>(MaximumTexelsPerRow) * SafeCentimetresPerTexel();
	}

	double RowLength() const
	{
		return FMath::Max(TrackLength, UE_DOUBLE_SMALL_NUMBER) / RowCount();
	}

	int32 RowIndexAt(double NormalisedAlongTrackValue) const
	{
		return FMath::Clamp(
			FMath::FloorToInt32(NormalisedAlongTrackValue * RowCount()),
			0,
			RowCount() - 1);
	}

	FVector2D RibbonToAtlasInRow(int32 RowIndex, double NormalisedAlongTrackValue, double NormalisedAcrossTrackValue) const
	{
		const double Rows = RowCount();

		return FVector2D(
			NormalisedAlongTrackValue * Rows - RowIndex,
			(RowIndex + NormalisedAcrossTrackValue) / Rows);
	}

	FVector2D RibbonToAtlas(double NormalisedAlongTrackValue, double NormalisedAcrossTrackValue) const
	{
		return RibbonToAtlasInRow(
			RowIndexAt(NormalisedAlongTrackValue),
			NormalisedAlongTrackValue,
			NormalisedAcrossTrackValue);
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
