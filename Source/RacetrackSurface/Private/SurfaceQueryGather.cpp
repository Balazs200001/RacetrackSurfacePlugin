#include "SurfaceQueryGather.h"

namespace
{
	double AlongTrackAfterOffset(const FSurfaceTrackRibbon& Ribbon, double NormalisedAlongTrack)
	{
		if (!Ribbon.bClosedLoop)
		{
			return FMath::Clamp(NormalisedAlongTrack, 0.0, 1.0);
		}

		return NormalisedAlongTrack - FMath::FloorToDouble(NormalisedAlongTrack);
	}

	void AppendTap(
		double TexelAlongLap,
		double NormalisedAcrossTrack,
		double RowWidth,
		double RowTotal,
		int32 SlotIndex,
		double Weight,
		TArray<FSurfaceQueryTap>& OutTaps)
	{
		const double Row = FMath::FloorToDouble(TexelAlongLap / RowWidth);

		FSurfaceQueryTap Tap;

		Tap.AtlasUV = FVector2D(
			(TexelAlongLap - Row * RowWidth + 0.5) / RowWidth,
			(Row + NormalisedAcrossTrack) / RowTotal);

		Tap.SlotIndex = SlotIndex;
		Tap.Weight = static_cast<float>(Weight);

		OutTaps.Add(Tap);
	}
}

void RacetrackSurfaceQuery::BuildTaps(
	const FSurfaceTrackRibbon& Ribbon,
	int32 SlotIndex,
	const FSurfaceTrackCoordinate& Coordinate,
	double ContactPatchWidth,
	TArray<FSurfaceQueryTap>& OutTaps)
{
	const double RowTotal = Ribbon.RowCount();
	const double RowWidth = Ribbon.AtlasWidth();
	const double LapTexels = RowTotal * RowWidth;
	const double AcrossTexels = Ribbon.AcrossTrackTexels();

	const double AlongTrackStep = 1.0 / LapTexels;
	const double AcrossTrackStep = 1.0 / AcrossTexels;
	const double HalfBand = AcrossTrackStep * 0.5;

	const double RadiusInTexels =
		FMath::Max(ContactPatchWidth, 0.0) / (2.0 * Ribbon.SafeCentimetresPerTexel());

	const FVector2D Centre = Ribbon.TrackToNormalised(Coordinate);

	OutTaps.Reserve(OutTaps.Num() + TapsPerQueryPoint);

	for (int32 Y = -1; Y <= 1; ++Y)
	{
		for (int32 X = -1; X <= 1; ++X)
		{
			const double Weight = (X == 0 ? 2.0 : 1.0) * (Y == 0 ? 2.0 : 1.0);

			const double AlongTrack = AlongTrackAfterOffset(
				Ribbon,
				Centre.X + X * AlongTrackStep * RadiusInTexels);

			const double AcrossTrack = FMath::Clamp(
				Centre.Y + Y * AcrossTrackStep * RadiusInTexels,
				HalfBand,
				1.0 - HalfBand);

			const double Sampled = AlongTrack * LapTexels - 0.5;
			const double Lower = FMath::FloorToDouble(Sampled);
			const double Blend = Sampled - Lower;

			const double FirstTexel = Ribbon.bClosedLoop
				? (Lower < 0.0 ? LapTexels - 1.0 : Lower)
				: FMath::Clamp(Lower, 0.0, LapTexels - 1.0);

			const double SecondTexel = Ribbon.bClosedLoop
				? (Lower + 1.0 >= LapTexels ? 0.0 : Lower + 1.0)
				: FMath::Clamp(Lower + 1.0, 0.0, LapTexels - 1.0);

			AppendTap(FirstTexel, AcrossTrack, RowWidth, RowTotal, SlotIndex, Weight * (1.0 - Blend), OutTaps);
			AppendTap(SecondTexel, AcrossTrack, RowWidth, RowTotal, SlotIndex, Weight * Blend, OutTaps);
		}
	}
}
