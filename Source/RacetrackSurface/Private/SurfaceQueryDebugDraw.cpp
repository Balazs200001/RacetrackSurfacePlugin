// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "SurfaceQueryDebugDraw.h"

#include "DrawDebugHelpers.h"
#include "HAL/IConsoleManager.h"
#include "Math/RotationMatrix.h"

namespace
{
	TAutoConsoleVariable<int32> CVarDrawQueryOverlay(
		TEXT("r.RacetrackSurface.DrawQueryOverlay"),
		1,
		TEXT("Draws three coloured bars for each surface sample a caller hands the overlay: red for ")
		TEXT("rubber buildup, green for skid, blue for marbles, each inside a full scale frame so a ")
		TEXT("genuine zero reads as an empty bar rather than as nothing drawn. A sample that has not ")
		TEXT("resolved yet draws all three frames in magenta, which no channel uses, so an unanswered ")
		TEXT("query can never be mistaken for clean tarmac. The caller chooses where the bars are drawn."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarQueryOverlayFullScale(
		TEXT("r.RacetrackSurface.QueryOverlayFullScale"),
		1.0f,
		TEXT("The coverage a full length overlay bar stands for. 1 draws coverage directly. Lower it to ")
		TEXT("see the small values a session's first few laps deposit. Nothing on screen says it has been ")
		TEXT("changed, so a capture taken at anything but 1 has to record the value it was taken at. The ")
		TEXT("bar stays linear either way and is never ranged against the frame's own maximum, which would ")
		TEXT("stop four wheels being comparable with each other."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarQueryOverlayLength(
		TEXT("r.RacetrackSurface.QueryOverlayLength"),
		120.0f,
		TEXT("How long a full scale overlay bar is, in centimetres along the track. The default keeps one ")
		TEXT("wheel's three bars clear of the next wheel's at a road car's wheelbase and track width. Raise ")
		TEXT("it when the camera is far enough away that the bars are a few pixels."),
		ECVF_Default);

	constexpr float BarWidth = 12.0f;

	constexpr float BarGap = 5.0f;

	constexpr float BarThickness = 1.0f;

	constexpr float SurfaceClearance = 6.0f;

	constexpr float OneFrame = 0.0f;

	const EAccumulationChannel BarOrder[] =
	{
		EAccumulationChannel::RubberBuildup,
		EAccumulationChannel::SkidMark,
		EAccumulationChannel::Marbles
	};

	static_assert(UE_ARRAY_COUNT(BarOrder) == RacetrackSurfaceQueryOverlay::BarCount);

	FVector BarCentreOffset(const RacetrackSurfaceQueryOverlay::FOverlayBasis& Basis, int32 BarIndex)
	{
		const float Pitch = BarWidth + BarGap;
		const float Across = (BarIndex - (RacetrackSurfaceQueryOverlay::BarCount - 1) * 0.5f) * Pitch;

		return Basis.Across * Across + Basis.Up * SurfaceClearance;
	}

	FQuat BarRotation(const RacetrackSurfaceQueryOverlay::FOverlayBasis& Basis)
	{
		return FRotationMatrix::MakeFromXZ(Basis.Along, Basis.Up).ToQuat();
	}
}

bool RacetrackSurfaceQueryOverlay::IsEnabled()
{
	return CVarDrawQueryOverlay.GetValueOnGameThread() != 0;
}

float RacetrackSurfaceQueryOverlay::GetFullScale()
{
	return FMath::Max(CVarQueryOverlayFullScale.GetValueOnGameThread(), UE_SMALL_NUMBER);
}

float RacetrackSurfaceQueryOverlay::GetBarLength()
{
	return FMath::Max(CVarQueryOverlayLength.GetValueOnGameThread(), 1.0f);
}

FColor RacetrackSurfaceQueryOverlay::ChannelColour(EAccumulationChannel Channel)
{
	switch (Channel)
	{
	case EAccumulationChannel::RubberBuildup:
		return FColor::Red;
	case EAccumulationChannel::SkidMark:
		return FColor::Green;
	case EAccumulationChannel::Marbles:
		return FColor::Blue;
	default:
		return FColor::White;
	}
}

FColor RacetrackSurfaceQueryOverlay::FrameColour(ESurfaceSampleStatus Status, EAccumulationChannel Channel)
{
	switch (Status)
	{
	case ESurfaceSampleStatus::Valid:
		return ChannelColour(Channel);
	case ESurfaceSampleStatus::OutsideRegion:
		return FColor(64, 64, 64);
	case ESurfaceSampleStatus::NotYetResolved:
		return FColor::Magenta;
	default:
		return FColor::White;
	}
}

float RacetrackSurfaceQueryOverlay::BarFillFraction(float Coverage, float FullScale)
{
	return FMath::Clamp(Coverage / FMath::Max(FullScale, UE_SMALL_NUMBER), 0.0f, 1.0f);
}

void RacetrackSurfaceQueryOverlay::DrawSurfaceStateSample(
	const UWorld* World,
	const FVector& DrawPosition,
	const FOverlayBasis& Basis,
	const FSurfaceStateSample& Sample)
{
	if (World == nullptr || !IsEnabled())
	{
		return;
	}

	const float FullScale = GetFullScale();
	const float Length = GetBarLength();
	const FQuat Rotation = BarRotation(Basis);
	const FVector HalfLengthAlong = Basis.Along * (Length * 0.5);

	for (int32 BarIndex = 0; BarIndex < BarCount; ++BarIndex)
	{
		const EAccumulationChannel Channel = BarOrder[BarIndex];
		const FVector Centre = DrawPosition + BarCentreOffset(Basis, BarIndex);

		DrawDebugBox(
			World,
			Centre,
			FVector(Length * 0.5, BarWidth * 0.5, BarThickness * 0.5),
			Rotation,
			FrameColour(Sample.Status, Channel),
			false,
			OneFrame,
			0,
			0.0f);

		if (Sample.Status != ESurfaceSampleStatus::Valid)
		{
			continue;
		}

		const float Fill = BarFillFraction(Sample.GetChannel(Channel), FullScale);

		if (Fill <= 0.0f)
		{
			continue;
		}

		const double FilledLength = Length * Fill;

		DrawDebugSolidBox(
			World,
			Centre - HalfLengthAlong + Basis.Along * (FilledLength * 0.5),
			FVector(FilledLength * 0.5, BarWidth * 0.5, BarThickness * 0.5),
			Rotation,
			ChannelColour(Channel),
			false,
			OneFrame,
			0);
	}
}
