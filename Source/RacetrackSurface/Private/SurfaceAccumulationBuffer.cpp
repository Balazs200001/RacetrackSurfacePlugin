#include "SurfaceAccumulationBuffer.h"

#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "PixelFormat.h"
#include "SurfaceAccumulationSubsystem.h"

namespace RacetrackSurfaceBrush
{
	static constexpr int32 Resolution = 64;

	static TArray<uint8> BuildRadialBandTexels()
	{
		TArray<uint8> Texels;
		Texels.SetNumUninitialized(Resolution * Resolution * 4);

		const float Centre = (Resolution - 1) * 0.5f;

		for (int32 Y = 0; Y < Resolution; ++Y)
		{
			const float DistanceFromCentre = FMath::Abs(Y - Centre) / Centre;
			const float Falloff = 1.0f - FMath::SmoothStep(0.0f, 1.0f, DistanceFromCentre);
			const uint8 Alpha = static_cast<uint8>(FMath::Clamp(Falloff, 0.0f, 1.0f) * 255.0f + 0.5f);

			for (int32 X = 0; X < Resolution; ++X)
			{
				const int32 Offset = (Y * Resolution + X) * 4;
				Texels[Offset + 0] = 255;
				Texels[Offset + 1] = 255;
				Texels[Offset + 2] = 255;
				Texels[Offset + 3] = Alpha;
			}
		}

		return Texels;
	}
}

FLinearColor USurfaceAccumulationBuffer::ChannelMask(EAccumulationChannel Channel)
{
	switch (Channel)
	{
	case EAccumulationChannel::RubberBuildup:
		return FLinearColor(1.0f, 0.0f, 0.0f, 0.0f);
	case EAccumulationChannel::SkidMark:
		return FLinearColor(0.0f, 1.0f, 0.0f, 0.0f);
	case EAccumulationChannel::Marbles:
		return FLinearColor(0.0f, 0.0f, 1.0f, 0.0f);
	default:
		return FLinearColor::Black;
	}
}

bool USurfaceAccumulationBuffer::Allocate(UObject* WorldContext, const FSurfaceRegion& Region, ETextureRenderTargetFormat Format)
{
	if (WorldContext == nullptr)
	{
		return false;
	}

	const int32 Resolution = FMath::Max(Region.ResolutionTexels, 1);

	if (Target != nullptr && AllocatedResolution == Resolution && AllocatedFormat == Format)
	{
		return true;
	}

	Release();

	Target = UKismetRenderingLibrary::CreateRenderTarget2D(
		WorldContext,
		Resolution,
		Resolution,
		Format,
		FLinearColor::Black,
		false,
		false);

	if (Target == nullptr)
	{
		return false;
	}

	AllocatedResolution = Resolution;
	AllocatedFormat = Format;

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Accumulation buffer allocated at %d x %d, %lld bytes"),
		AllocatedResolution,
		AllocatedResolution,
		GetAllocatedBytes());

	return true;
}

void USurfaceAccumulationBuffer::Release()
{
	if (Target != nullptr)
	{
		UKismetRenderingLibrary::ReleaseRenderTarget2D(Target);
		Target = nullptr;
	}

	AllocatedResolution = 0;
}

int64 USurfaceAccumulationBuffer::GetAllocatedBytes() const
{
	if (Target == nullptr)
	{
		return 0;
	}

	const EPixelFormat PixelFormat = GetPixelFormatFromRenderTargetFormat(AllocatedFormat);

	return static_cast<int64>(GPixelFormats[PixelFormat].BlockBytes)
		* static_cast<int64>(AllocatedResolution)
		* static_cast<int64>(AllocatedResolution);
}

void USurfaceAccumulationBuffer::EnsureBrush()
{
	if (Brush != nullptr)
	{
		return;
	}

	const TArray<uint8> Texels = RacetrackSurfaceBrush::BuildRadialBandTexels();

	Brush = UTexture2D::CreateTransient(
		RacetrackSurfaceBrush::Resolution,
		RacetrackSurfaceBrush::Resolution,
		PF_B8G8R8A8,
		TEXT("RacetrackSurfaceBrush"),
		Texels);

	if (Brush != nullptr)
	{
		Brush->SRGB = false;
		Brush->UpdateResource();
	}
}

void USurfaceAccumulationBuffer::EnsureSolid()
{
	if (Solid != nullptr)
	{
		return;
	}

	const TArray<uint8> Texels = { 255, 255, 255, 255 };

	Solid = UTexture2D::CreateTransient(1, 1, PF_B8G8R8A8, TEXT("RacetrackSurfaceSolid"), Texels);

	if (Solid != nullptr)
	{
		Solid->SRGB = false;
		Solid->UpdateResource();
	}
}

void USurfaceAccumulationBuffer::ApplyDecay(UObject* WorldContext, float Retention)
{
	if (Target == nullptr || WorldContext == nullptr)
	{
		return;
	}

	EnsureSolid();

	if (Solid == nullptr)
	{
		return;
	}

	const float Clamped = FMath::Clamp(Retention, 0.0f, 1.0f);

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize = FVector2D::ZeroVector;
	FDrawToRenderTargetContext Context;

	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Target, Canvas, CanvasSize, Context);

	if (Canvas != nullptr)
	{
		Canvas->K2_DrawTexture(
			Solid,
			FVector2D::ZeroVector,
			CanvasSize,
			FVector2D::ZeroVector,
			FVector2D::UnitVector,
			FLinearColor(Clamped, Clamped, Clamped, 1.0f),
			BLEND_Modulate,
			0.0f,
			FVector2D::ZeroVector);
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);
}

void USurfaceAccumulationBuffer::Clear(UObject* WorldContext)
{
	if (Target == nullptr || WorldContext == nullptr)
	{
		return;
	}

	UKismetRenderingLibrary::ClearRenderTarget2D(WorldContext, Target, FLinearColor::Black);
}

void USurfaceAccumulationBuffer::PaintStrokes(UObject* WorldContext, const TArray<FSurfaceBrushStroke>& Strokes)
{
	if (Target == nullptr || WorldContext == nullptr || Strokes.Num() == 0)
	{
		return;
	}

	EnsureBrush();

	if (Brush == nullptr)
	{
		return;
	}

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize = FVector2D::ZeroVector;
	FDrawToRenderTargetContext Context;

	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Target, Canvas, CanvasSize, Context);

	if (Canvas != nullptr)
	{
		for (const FSurfaceBrushStroke& Stroke : Strokes)
		{
			const FVector2D Start = Stroke.RegionStart * CanvasSize;
			const FVector2D End = Stroke.RegionEnd * CanvasSize;
			const FVector2D Delta = End - Start;

			const float Width = FMath::Max(Stroke.WidthInTexels, 1.0f);
			const float Length = static_cast<float>(Delta.Size()) + Width;

			const FVector2D QuadSize(Length, Width);
			const FVector2D Centre = (Start + End) * 0.5;
			const FVector2D TopLeft = Centre - QuadSize * 0.5;

			const float RotationDegrees = Delta.IsNearlyZero()
				? 0.0f
				: FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));

			Canvas->K2_DrawTexture(
				Brush,
				TopLeft,
				QuadSize,
				FVector2D::ZeroVector,
				FVector2D::UnitVector,
				ChannelMask(Stroke.Channel) * Stroke.Intensity,
				BLEND_Additive,
				RotationDegrees,
				FVector2D(0.5f, 0.5f));
		}
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);
}
