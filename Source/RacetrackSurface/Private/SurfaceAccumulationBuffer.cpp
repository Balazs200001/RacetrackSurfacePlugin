#include "SurfaceAccumulationBuffer.h"

#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "PixelFormat.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> CVarTimePaint(
		TEXT("r.RacetrackSurface.TimePaint"),
		0,
		TEXT("When 1, brackets each PaintStrokes call in RHI timestamp queries and logs the GPU time. ")
		TEXT("Synchronises the game thread with the render thread on every paint, so it is a measurement ")
		TEXT("mode rather than something to leave on."),
		ECVF_Default);

	struct FPaintTimingQueries
	{
		FRenderQueryRHIRef Start;
		FRenderQueryRHIRef End;
		uint64 StartMicroseconds = 0;
		uint64 EndMicroseconds = 0;
		bool bResolved = false;
	};

	class FScopedPaintTimer
	{
	public:
		explicit FScopedPaintTimer(int32 InStrokeCount)
			: StrokeCount(InStrokeCount)
			, bEnabled(CVarTimePaint.GetValueOnGameThread() != 0 && GSupportsTimestampRenderQueries)
		{
			if (!bEnabled)
			{
				return;
			}

			Queries = MakeShared<FPaintTimingQueries>();

			ENQUEUE_RENDER_COMMAND(RacetrackSurfaceBeginPaintTiming)(
				[Captured = Queries](FRHICommandListImmediate& RHICmdList)
				{
					Captured->Start = RHICreateRenderQuery(RQT_AbsoluteTime);
					Captured->End = RHICreateRenderQuery(RQT_AbsoluteTime);

					if (Captured->Start.IsValid())
					{
						RHICmdList.EndRenderQuery(Captured->Start);
					}
				});
		}

		~FScopedPaintTimer()
		{
			if (!bEnabled || !Queries.IsValid())
			{
				return;
			}

			ENQUEUE_RENDER_COMMAND(RacetrackSurfaceEndPaintTiming)(
				[Captured = Queries](FRHICommandListImmediate& RHICmdList)
				{
					if (!Captured->Start.IsValid() || !Captured->End.IsValid())
					{
						return;
					}

					RHICmdList.EndRenderQuery(Captured->End);
					RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

					Captured->bResolved =
						RHIGetRenderQueryResult(Captured->Start, Captured->StartMicroseconds, true)
						&& RHIGetRenderQueryResult(Captured->End, Captured->EndMicroseconds, true);
				});

			FlushRenderingCommands();

			if (!Queries->bResolved || Queries->EndMicroseconds < Queries->StartMicroseconds)
			{
				return;
			}

			UE_LOG(LogRacetrackSurface, Log,
				TEXT("Paint timing: %d strokes, %.4f ms"),
				StrokeCount,
				static_cast<double>(Queries->EndMicroseconds - Queries->StartMicroseconds) / 1000.0);
		}

	private:
		TSharedPtr<FPaintTimingQueries> Queries;
		int32 StrokeCount;
		bool bEnabled;
	};

	class FScopedCanvasFlushDeferral
	{
	public:
		explicit FScopedCanvasFlushDeferral(UCanvas* InCanvas)
			: Canvas(InCanvas != nullptr ? InCanvas->Canvas : nullptr)
			, ModesToRestore(Canvas != nullptr ? Canvas->GetAllowedModes() : 0)
		{
			if (Canvas != nullptr)
			{
				Canvas->SetAllowedModes(ModesToRestore & ~FCanvas::Allow_Flush);
			}
		}

		~FScopedCanvasFlushDeferral()
		{
			if (Canvas != nullptr)
			{
				Canvas->SetAllowedModes(ModesToRestore);
			}
		}

	private:
		FCanvas* Canvas;
		uint32 ModesToRestore;
	};
}

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

bool USurfaceAccumulationBuffer::Allocate(
	UObject* WorldContext,
	int32 Width,
	int32 Height,
	ETextureRenderTargetFormat Format)
{
	if (WorldContext == nullptr)
	{
		return false;
	}

	const int32 ClampedWidth = FMath::Max(Width, 1);
	const int32 ClampedHeight = FMath::Max(Height, 1);
	const int32 MaximumDimension = static_cast<int32>(GetMax2DTextureDimension());

	if (ClampedWidth > MaximumDimension || ClampedHeight > MaximumDimension)
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("Cannot allocate a %d x %d accumulation buffer: this RHI allows at most %d in either dimension. ")
			TEXT("A ribbon this long has to be folded into rows, which is not implemented, or drawn at a coarser texel density."),
			ClampedWidth,
			ClampedHeight,
			MaximumDimension);

		return false;
	}

	if (Target != nullptr && AllocatedWidth == ClampedWidth && AllocatedHeight == ClampedHeight && AllocatedFormat == Format)
	{
		return true;
	}

	Release();

	Target = UKismetRenderingLibrary::CreateRenderTarget2D(
		WorldContext,
		ClampedWidth,
		ClampedHeight,
		Format,
		FLinearColor::Black,
		false,
		false);

	if (Target == nullptr)
	{
		return false;
	}

	AllocatedWidth = ClampedWidth;
	AllocatedHeight = ClampedHeight;
	AllocatedFormat = Format;

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Accumulation buffer allocated at %d x %d, %lld bytes"),
		AllocatedWidth,
		AllocatedHeight,
		GetAllocatedBytes());

	return true;
}

bool USurfaceAccumulationBuffer::Allocate(
	UObject* WorldContext,
	const FSurfaceTrackRibbon& Ribbon,
	ETextureRenderTargetFormat Format)
{
	return Allocate(WorldContext, Ribbon.AlongTrackTexels(), Ribbon.AcrossTrackTexels(), Format);
}

void USurfaceAccumulationBuffer::Release()
{
	if (Target != nullptr)
	{
		UKismetRenderingLibrary::ReleaseRenderTarget2D(Target);
		Target = nullptr;
	}

	AllocatedWidth = 0;
	AllocatedHeight = 0;
}

int64 USurfaceAccumulationBuffer::GetAllocatedBytes() const
{
	if (Target == nullptr)
	{
		return 0;
	}

	const EPixelFormat PixelFormat = GetPixelFormatFromRenderTargetFormat(AllocatedFormat);

	return static_cast<int64>(GPixelFormats[PixelFormat].BlockBytes)
		* static_cast<int64>(AllocatedWidth)
		* static_cast<int64>(AllocatedHeight);
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

void USurfaceAccumulationBuffer::PaintStrokes(
	UObject* WorldContext,
	const TArray<FSurfaceBrushStroke>& Strokes,
	ESurfacePaintMode PaintMode)
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

	const FScopedPaintTimer PaintTimer(Strokes.Num());

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize = FVector2D::ZeroVector;
	FDrawToRenderTargetContext Context;

	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Target, Canvas, CanvasSize, Context);

	if (Canvas != nullptr)
	{
		const FScopedCanvasFlushDeferral FlushDeferral(Canvas);

		const bool bSaturating = PaintMode == ESurfacePaintMode::Saturating;
		const EBlendMode BlendMode = bSaturating ? BLEND_Translucent : BLEND_Additive;

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

			const FLinearColor Mask = ChannelMask(Stroke.Channel);

			const FLinearColor RenderColour = bSaturating
				? FLinearColor(Mask.R, Mask.G, Mask.B, Stroke.Intensity)
				: Mask * Stroke.Intensity;

			Canvas->K2_DrawTexture(
				Brush,
				TopLeft,
				QuadSize,
				FVector2D::ZeroVector,
				FVector2D::UnitVector,
				RenderColour,
				BlendMode,
				RotationDegrees,
				FVector2D(0.5f, 0.5f));
		}
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);
}
