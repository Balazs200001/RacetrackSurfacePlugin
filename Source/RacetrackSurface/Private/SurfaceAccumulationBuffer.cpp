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
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "SurfaceDecayPass.h"
#include "SurfaceQueryGather.h"
#include "TextureResource.h"

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
	ETextureRenderTargetFormat Format,
	int32 RowCount)
{
	if (WorldContext == nullptr)
	{
		return false;
	}

	const int32 ClampedWidth = FMath::Max(Width, 1);
	const int32 ClampedHeight = FMath::Max(Height, 1);
	const int32 ClampedRowCount = FMath::Max(RowCount, 1);
	const int32 MaximumDimension = static_cast<int32>(GetMax2DTextureDimension());

	if (ClampedWidth > MaximumDimension || ClampedHeight > MaximumDimension)
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("Cannot allocate a %d x %d accumulation buffer folded into %d rows: this RHI allows at most %d ")
			TEXT("in either dimension. The ribbon needs more rows, a coarser texel density or a shorter track."),
			ClampedWidth,
			ClampedHeight,
			ClampedRowCount,
			MaximumDimension);

		return false;
	}

	if (Target != nullptr
		&& AllocatedWidth == ClampedWidth
		&& AllocatedHeight == ClampedHeight
		&& AllocatedRowCount == ClampedRowCount
		&& AllocatedFormat == Format)
	{
		return true;
	}

	if (Target != nullptr)
	{
		FlushRenderingCommands();
	}

	Release();

	const bool bDecaysInPlace = Format == RTF_RGBA16f && RHIIsTypedUAVLoadSupported(PF_FloatRGBA);

	Target = UKismetRenderingLibrary::CreateRenderTarget2D(
		WorldContext,
		ClampedWidth,
		ClampedHeight,
		Format,
		FLinearColor::Black,
		false,
		bDecaysInPlace);

	if (Target == nullptr)
	{
		return false;
	}

	AllocatedWidth = ClampedWidth;
	AllocatedHeight = ClampedHeight;
	AllocatedRowCount = ClampedRowCount;
	AllocatedFormat = Format;

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Accumulation buffer allocated at %d x %d in %d rows, %lld bytes, %s"),
		AllocatedWidth,
		AllocatedHeight,
		AllocatedRowCount,
		GetAllocatedBytes(),
		bDecaysInPlace
			? TEXT("with a UAV so it decays in place")
			: TEXT("without a UAV, so it decays uniformly on the canvas and skid is not paved over: in-place "
				"decay rounds to half floats and needs RGBA16f with typed UAV loads"));

	return true;
}

bool USurfaceAccumulationBuffer::Allocate(
	UObject* WorldContext,
	const FSurfaceTrackRibbon& Ribbon,
	ETextureRenderTargetFormat Format)
{
	return Allocate(WorldContext, Ribbon.AtlasWidth(), Ribbon.AtlasHeight(), Format, Ribbon.RowCount());
}

FIntRect USurfaceAccumulationBuffer::GetRowBounds(int32 RowIndex) const
{
	const int32 Rows = FMath::Max(AllocatedRowCount, 1);
	const int32 RowHeight = AllocatedHeight / Rows;
	const int32 ClampedRow = FMath::Clamp(RowIndex, 0, Rows - 1);

	return FIntRect(0, ClampedRow * RowHeight, AllocatedWidth, (ClampedRow + 1) * RowHeight);
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
	AllocatedRowCount = 1;
	bWarnedThatDecayIsUniform = false;
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

void USurfaceAccumulationBuffer::SweepStrokes(
	UObject* WorldContext,
	const TArray<FSurfaceBrushStroke>& Strokes,
	EAccumulationChannel SweptChannel,
	float Retention)
{
	if (Target == nullptr || WorldContext == nullptr || Strokes.Num() == 0 || Retention >= 1.0f)
	{
		return;
	}

	EnsureSolid();

	if (Solid == nullptr)
	{
		return;
	}

	const FLinearColor Surviving =
		FLinearColor::White - ChannelMask(SweptChannel) * (1.0f - FMath::Clamp(Retention, 0.0f, 1.0f));

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize = FVector2D::ZeroVector;
	FDrawToRenderTargetContext Context;

	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Target, Canvas, CanvasSize, Context);

	if (Canvas != nullptr)
	{
		const FScopedCanvasFlushDeferral FlushDeferral(Canvas);

		const bool bClipToRows = AllocatedRowCount > 1 && Canvas->Canvas != nullptr;
		int32 ClippedRow = INDEX_NONE;

		for (const FSurfaceBrushStroke& Stroke : Strokes)
		{
			if (Stroke.Channel == SweptChannel)
			{
				continue;
			}

			if (bClipToRows && Stroke.RowIndex != ClippedRow)
			{
				if (ClippedRow != INDEX_NONE)
				{
					Canvas->Canvas->Flush_GameThread(true);
				}

				ClippedRow = Stroke.RowIndex;
				Canvas->Canvas->SetRenderTargetScissorRect(GetRowBounds(ClippedRow));
			}

			const FVector2D Start = Stroke.RegionStart * CanvasSize;
			const FVector2D End = Stroke.RegionEnd * CanvasSize;
			const FVector2D Delta = End - Start;

			const float Width = FMath::Max(Stroke.WidthInTexels, 1.0f);
			const FVector2D QuadSize(static_cast<float>(Delta.Size()) + Width, Width);
			const FVector2D Centre = (Start + End) * 0.5;

			const float RotationDegrees = Delta.IsNearlyZero()
				? 0.0f
				: FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));

			Canvas->K2_DrawTexture(
				Solid,
				Centre - QuadSize * 0.5,
				QuadSize,
				FVector2D::ZeroVector,
				FVector2D::UnitVector,
				Surviving,
				BLEND_Modulate,
				RotationDegrees,
				FVector2D(0.5f, 0.5f));
		}
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);
}

bool USurfaceAccumulationBuffer::CanDecayInPlace() const
{
	return Target != nullptr && Target->bCanCreateUAV;
}

bool USurfaceAccumulationBuffer::ApplyDecay(UObject* WorldContext, float HalfLivesElapsed)
{
	if (Target == nullptr || HalfLivesElapsed <= 0.0f)
	{
		return false;
	}

	if (!CanDecayInPlace())
	{
		if (!bWarnedThatDecayIsUniform)
		{
			bWarnedThatDecayIsUniform = true;

			UE_LOG(LogRacetrackSurface, Warning,
				TEXT("The %s accumulation target has no UAV, so it decays uniformly on the canvas and skid is not "
					"paved over by rubber. In-place decay rounds to half floats, so it needs RGBA16f and an RHI "
					"with typed UAV loads for it"),
				GPixelFormats[Target->GetFormat()].Name);
		}

		return ApplyUniformDecayOnCanvas(WorldContext, HalfLivesElapsed);
	}

	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();

	if (Resource == nullptr)
	{
		return false;
	}

	RacetrackSurfaceDecayPass::Enqueue(Resource, HalfLivesElapsed, RacetrackSurfaceMapping::SaturationConstant);

	return true;
}

bool USurfaceAccumulationBuffer::ApplyUniformDecayOnCanvas(UObject* WorldContext, float HalfLivesElapsed)
{
	if (WorldContext == nullptr)
	{
		return false;
	}

	EnsureSolid();

	if (Solid == nullptr)
	{
		return false;
	}

	const float Retention = RacetrackSurfaceMapping::DecayRetentionForProgress(HalfLivesElapsed);

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
			FLinearColor(Retention, Retention, Retention, 1.0f),
			BLEND_Modulate,
			0.0f,
			FVector2D::ZeroVector);
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);

	return Canvas != nullptr;
}

void USurfaceAccumulationBuffer::Clear(UObject* WorldContext)
{
	if (Target == nullptr || WorldContext == nullptr)
	{
		return;
	}

	UKismetRenderingLibrary::ClearRenderTarget2D(WorldContext, Target, FLinearColor::Black);
}

UTextureRenderTarget2D* USurfaceAccumulationBuffer::EnsureGatherTarget(UObject* WorldContext, int32 SlotCount)
{
	if (WorldContext == nullptr)
	{
		return nullptr;
	}

	const int32 Width = FMath::Max(SlotCount, 1);

	if (GatherTarget != nullptr && GatherTarget->SizeX >= Width)
	{
		return GatherTarget;
	}

	if (GatherTarget != nullptr)
	{
		UKismetRenderingLibrary::ReleaseRenderTarget2D(GatherTarget);
		GatherTarget = nullptr;
	}

	GatherTarget = UKismetRenderingLibrary::CreateRenderTarget2D(
		WorldContext,
		Width,
		GatherTargetHeight,
		RTF_RGBA16f,
		FLinearColor::Black,
		false,
		false);

	if (GatherTarget == nullptr)
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("Could not allocate a %d x 1 query gather target, so no surface query can be answered"),
			Width);

		return nullptr;
	}

	GatherTarget->TargetGamma = 0.0f;

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Query gather target allocated at %d x 1, %d slots, display gamma %.1f"),
		Width,
		Width,
		GatherTarget->GetDisplayGamma());

	return GatherTarget;
}

void USurfaceAccumulationBuffer::GatherTaps(
	UObject* WorldContext,
	const TArray<FSurfaceQueryTap>& Taps,
	int32 SlotCount)
{
	if (Target == nullptr || WorldContext == nullptr || Taps.Num() == 0)
	{
		return;
	}

	UTextureRenderTarget2D* Destination = EnsureGatherTarget(WorldContext, SlotCount);

	if (Destination == nullptr || Destination == Target)
	{
		return;
	}

	UKismetRenderingLibrary::ClearRenderTarget2D(WorldContext, Destination, FLinearColor::Black);

	UCanvas* Canvas = nullptr;
	FVector2D CanvasSize = FVector2D::ZeroVector;
	FDrawToRenderTargetContext Context;

	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Destination, Canvas, CanvasSize, Context);

	if (Canvas != nullptr)
	{
		const FScopedCanvasFlushDeferral FlushDeferral(Canvas);

		const FVector2D SourceTexel(
			1.0 / FMath::Max(Target->SizeX, 1),
			1.0 / FMath::Max(Target->SizeY, 1));

		for (const FSurfaceQueryTap& Tap : Taps)
		{
			if (Tap.SlotIndex < 0 || Tap.SlotIndex >= Destination->SizeX || Tap.Weight <= 0.0f)
			{
				continue;
			}

			Canvas->K2_DrawTexture(
				Target,
				FVector2D(Tap.SlotIndex, 0),
				FVector2D::UnitVector,
				Tap.AtlasUV - SourceTexel * 0.5,
				SourceTexel,
				FLinearColor(Tap.Weight, Tap.Weight, Tap.Weight, 1.0f),
				BLEND_Additive,
				0.0f,
				FVector2D::ZeroVector);
		}
	}

	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);
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

		const bool bClipToRows = AllocatedRowCount > 1 && Canvas->Canvas != nullptr;
		int32 ClippedRow = INDEX_NONE;

		for (const FSurfaceBrushStroke& Stroke : Strokes)
		{
			if (bClipToRows && Stroke.RowIndex != ClippedRow)
			{
				if (ClippedRow != INDEX_NONE)
				{
					Canvas->Canvas->Flush_GameThread(true);
				}

				ClippedRow = Stroke.RowIndex;
				Canvas->Canvas->SetRenderTargetScissorRect(GetRowBounds(ClippedRow));
			}

			const FVector2D Start = Stroke.RegionStart * CanvasSize;
			const FVector2D End = Stroke.RegionEnd * CanvasSize;
			const FVector2D Delta = End - Start;

			const float Width = FMath::Max(Stroke.WidthInTexels, 1.0f);
			const float Travel = static_cast<float>(Delta.Size());
			const float Length = Travel + Width;

			const float SweptShare = Travel > UE_SMALL_NUMBER ? Travel / Length : 1.0f;
			const float DepositedIntensity = Stroke.Intensity * SweptShare;

			const FVector2D QuadSize(Length, Width);
			const FVector2D Centre = (Start + End) * 0.5;
			const FVector2D TopLeft = Centre - QuadSize * 0.5;

			const float RotationDegrees = Delta.IsNearlyZero()
				? 0.0f
				: FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));

			const FLinearColor Mask = ChannelMask(Stroke.Channel);

			const FLinearColor RenderColour = bSaturating
				? FLinearColor(Mask.R, Mask.G, Mask.B, DepositedIntensity)
				: Mask * DepositedIntensity;

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
