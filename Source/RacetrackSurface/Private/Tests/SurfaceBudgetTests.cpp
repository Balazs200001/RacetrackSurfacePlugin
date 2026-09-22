#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "RHIGlobals.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"
#include "Tests/SurfaceGpuTiming.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::SampleGpuWork;

namespace
{
	constexpr double CarriedOverCentimetresPerTexel = 9.765625;

	struct FFormatCandidate
	{
		ETextureRenderTargetFormat Format;
		const TCHAR* Name;
	};

	const FFormatCandidate FormatCandidates[] =
	{
		{ RTF_RGBA8, TEXT("RGBA8") },
		{ RTF_RGBA16f, TEXT("RGBA16f") },
		{ RTF_RGBA32f, TEXT("RGBA32f") }
	};

	const int32 ResolutionCandidates[] = { 1024, 2048, 4096 };

	using RacetrackSurfaceTest::MakeSquareTestRibbon;

	TArray<FSurfaceBrushStroke> MakeStrokes(const FSurfaceTrackRibbon& Ribbon, int32 Count, float Intensity)
	{
		const double FrameTravel = RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond / 60.0;
		const double NormalisedLength = Ribbon.NormalisedAlongTrack(FrameTravel);
		const float WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(20.0));

		TArray<FSurfaceBrushStroke> Strokes;
		Strokes.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			FSurfaceBrushStroke Stroke;
			Stroke.RegionStart = FVector2D(0.5 - NormalisedLength * 0.5, 0.45 + 0.02 * Index);
			Stroke.RegionEnd = FVector2D(0.5 + NormalisedLength * 0.5, 0.45 + 0.02 * Index);
			Stroke.WidthInTexels = WidthInTexels;
			Stroke.Intensity = Intensity;
			Stroke.Channel = EAccumulationChannel::SkidMark;

			Strokes.Add(Stroke);
		}

		return Strokes;
	}

	TArray<FSurfaceBrushStroke> MakeCentredStroke(
		const FSurfaceTrackRibbon& Ribbon,
		float Intensity,
		EAccumulationChannel Channel)
	{
		const float WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(200.0));

		FSurfaceBrushStroke Stroke;
		Stroke.RegionStart = FVector2D(0.4, 0.5);
		Stroke.RegionEnd = FVector2D(0.6, 0.5);
		Stroke.WidthInTexels = WidthInTexels;
		Stroke.Intensity = Intensity;
		Stroke.Channel = Channel;

		return TArray<FSurfaceBrushStroke>({ Stroke });
	}

	bool IsReadableFormat(ETextureRenderTargetFormat Format)
	{
		const EPixelFormat PixelFormat = GetPixelFormatFromRenderTargetFormat(Format);

		return PixelFormat == PF_B8G8R8A8
			|| PixelFormat == PF_FloatRGBA
			|| PixelFormat == PF_A32B32G32R32F;
	}

	FLinearColor ReadCentreTexel(UObject* WorldContext, UTextureRenderTarget2D* Target)
	{
		const int32 X = Target->SizeX / 2;
		const int32 Y = Target->SizeY / 2;

		FLinearColor Raw = UKismetRenderingLibrary::ReadRenderTargetRawPixel(WorldContext, Target, X, Y, false);

		if (Target->GetFormat() == PF_B8G8R8A8)
		{
			Raw = Raw / 255.0f;
		}

		return Raw;
	}

	TArray<FSurfaceBrushStroke> MakeOverlappingFannedStrokes(const FSurfaceTrackRibbon& Ribbon, int32 Count)
	{
		const float WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(400.0));
		const double HalfLength = 0.15;

		TArray<FSurfaceBrushStroke> Strokes;
		Strokes.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Angle = UE_DOUBLE_PI * static_cast<double>(Index) / static_cast<double>(Count);
			const FVector2D Half = FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * HalfLength;

			FSurfaceBrushStroke Stroke;
			Stroke.RegionStart = FVector2D(0.5, 0.5) - Half;
			Stroke.RegionEnd = FVector2D(0.5, 0.5) + Half;
			Stroke.WidthInTexels = WidthInTexels;
			Stroke.Intensity = 0.05f;
			Stroke.Channel = EAccumulationChannel::SkidMark;

			Strokes.Add(Stroke);
		}

		return Strokes;
	}

	TArray<FLinearColor> ReadCentredTexelWindow(
		UObject* WorldContext,
		UTextureRenderTarget2D* Target,
		int32 Steps,
		int32 Spacing)
	{
		TArray<FLinearColor> Samples;
		Samples.Reserve(Steps * Steps);

		for (int32 Y = 0; Y < Steps; ++Y)
		{
			for (int32 X = 0; X < Steps; ++X)
			{
				const int32 PixelX = Target->SizeX / 2 + (X - Steps / 2) * Spacing;
				const int32 PixelY = Target->SizeY / 2 + (Y - Steps / 2) * Spacing;

				FLinearColor Raw = UKismetRenderingLibrary::ReadRenderTargetRawPixel(
					WorldContext, Target, PixelX, PixelY, false);

				if (Target->GetFormat() == PF_B8G8R8A8)
				{
					Raw = Raw / 255.0f;
				}

				Samples.Add(Raw);
			}
		}

		return Samples;
	}

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceRegionBudgetSweep,
	"RacetrackSurface.Budget.RegionAndFormatSweep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceRegionBudgetSweep::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender() || !GSupportsTimestampRenderQueries)
	{
		AddWarning(TEXT("Skipped: no rendering or no timestamp query support."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	AddInfo(TEXT("Resolution | Format | Bytes | MiB | Ribbon metres along | Ribbon metres across | Paint 4 | Decay"));

	for (const int32 Resolution : ResolutionCandidates)
	{
		for (const FFormatCandidate& Candidate : FormatCandidates)
		{
			const FSurfaceTrackRibbon Ribbon = MakeSquareTestRibbon(Resolution);

			TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

			if (!Buffer->Allocate(World, Ribbon, Candidate.Format))
			{
				AddError(FString::Printf(
					TEXT("Failed to allocate %d square in %s"), Resolution, Candidate.Name));
				continue;
			}

			const TArray<FSurfaceBrushStroke> FourContacts = MakeStrokes(Ribbon, 4, 0.8f);

			Buffer->PaintStrokes(World, FourContacts);
			Buffer->ApplyDecay(World, 0.99f);
			FlushRenderingCommands();

			double PaintMedian = 0.0;
			double PaintMinimum = 0.0;
			double DecayMedian = 0.0;
			double DecayMinimum = 0.0;

			const bool bSampled =
				RacetrackSurfaceTest::SampleGpuWork(
					[&]() { Buffer->PaintStrokes(World, FourContacts); }, PaintMedian, PaintMinimum)
				&& RacetrackSurfaceTest::SampleGpuWork(
					[&]() { Buffer->ApplyDecay(World, 0.99f); }, DecayMedian, DecayMinimum);

			if (!bSampled)
			{
				AddError(FString::Printf(
					TEXT("Timing failed for %d square in %s"), Resolution, Candidate.Name));
				Buffer->Release();
				continue;
			}

			const double RibbonMetres =
				Resolution * CarriedOverCentimetresPerTexel / 100.0;

			AddInfo(FString::Printf(
				TEXT("%d | %s | %lld | %.1f | %.1f | %.1f | %.4f ms | %.4f ms"),
				Resolution,
				Candidate.Name,
				Buffer->GetAllocatedBytes(),
				static_cast<double>(Buffer->GetAllocatedBytes()) / (1024.0 * 1024.0),
				RibbonMetres,
				RibbonMetres,
				PaintMedian,
				DecayMedian));

			Buffer->Release();
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceLinearGammaOnWrite,
	"RacetrackSurface.Accumulator.LinearGammaOnWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceLinearGammaOnWrite::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	const FSurfaceTrackRibbon Ribbon = MakeSquareTestRibbon(256);
	const float Intensity = 0.5f;

	UTextureRenderTarget2D* Unforced = UKismetRenderingLibrary::CreateRenderTarget2D(
		World, 256, 256, RTF_RGBA8, FLinearColor::Black, false, false);

	if (Unforced == nullptr)
	{
		AddError(TEXT("Failed to create the comparison render target."));
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("CreateRenderTarget2D default for RTF_RGBA8: display gamma %.2f, IsSRGB %s"),
		Unforced->GetDisplayGamma(),
		Unforced->IsSRGB() ? TEXT("true") : TEXT("false")));

	Unforced->bForceLinearGamma = true;
	Unforced->UpdateResource();

	AddInfo(FString::Printf(
		TEXT("With bForceLinearGamma set: display gamma %.2f, IsSRGB %s"),
		Unforced->GetDisplayGamma(),
		Unforced->IsSRGB() ? TEXT("true") : TEXT("false")));

	UKismetRenderingLibrary::ReleaseRenderTarget2D(Unforced);

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA8))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer."));
		return false;
	}

	Buffer->Clear(World);
	Buffer->PaintStrokes(World, MakeCentredStroke(Ribbon, Intensity, EAccumulationChannel::SkidMark));
	FlushRenderingCommands();

	const FLinearColor Stored = ReadCentreTexel(World, Buffer->GetTarget());

	const double GammaEncoded = FMath::Pow(Intensity, 1.0 / 2.2);

	AddInfo(FString::Printf(
		TEXT("One additive pass at intensity %.3f stored %.4f. Linear would be %.4f, 2.2-encoded would be %.4f"),
		Intensity,
		Stored.G,
		Intensity,
		GammaEncoded));

	TestTrue(
		TEXT("An additive pass stores its linear intensity, not a gamma-encoded one"),
		FMath::Abs(Stored.G - Intensity) < 0.02f);

	Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceSaturationCurve,
	"RacetrackSurface.Accumulator.SaturationCurve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceSaturationCurve::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	const FSurfaceTrackRibbon Ribbon = MakeSquareTestRibbon(256);
	const float Increment = 0.1f;
	const int32 PassCount = 40;

	const ESurfacePaintMode Modes[] = { ESurfacePaintMode::Additive, ESurfacePaintMode::Saturating };
	const TCHAR* ModeNames[] = { TEXT("Additive"), TEXT("Saturating") };

	for (int32 ModeIndex = 0; ModeIndex < UE_ARRAY_COUNT(Modes); ++ModeIndex)
	{
		TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

		if (!Buffer->Allocate(World, Ribbon, RTF_RGBA16f))
		{
			AddError(TEXT("Failed to allocate the accumulation buffer."));
			return false;
		}

		Buffer->Clear(World);

		const TArray<FSurfaceBrushStroke> Buildup =
			MakeCentredStroke(Ribbon, 0.5f, EAccumulationChannel::RubberBuildup);
		const TArray<FSurfaceBrushStroke> Skid =
			MakeCentredStroke(Ribbon, Increment, EAccumulationChannel::SkidMark);

		Buffer->PaintStrokes(World, Buildup, ESurfacePaintMode::Additive);
		FlushRenderingCommands();

		const FLinearColor Before = ReadCentreTexel(World, Buffer->GetTarget());

		FString Curve;

		for (int32 Pass = 1; Pass <= PassCount; ++Pass)
		{
			Buffer->PaintStrokes(World, Skid, Modes[ModeIndex]);

			if (Pass == 1 || Pass == 2 || Pass == 5 || Pass == 10 || Pass == 20 || Pass == PassCount)
			{
				FlushRenderingCommands();
				const FLinearColor Value = ReadCentreTexel(World, Buffer->GetTarget());
				Curve += FString::Printf(TEXT("  pass %d: %.5f"), Pass, Value.G);
			}
		}

		FlushRenderingCommands();
		const FLinearColor After = ReadCentreTexel(World, Buffer->GetTarget());

		AddInfo(FString::Printf(TEXT("%s, increment %.3f, RGBA16f:%s"),
			ModeNames[ModeIndex], Increment, *Curve));

		AddInfo(FString::Printf(
			TEXT("%s sibling buildup channel: %.5f before, %.5f after %d skid passes"),
			ModeNames[ModeIndex],
			Before.R,
			After.R,
			PassCount));

		Buffer->Release();
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceBatchedPaintMatchesSequential,
	"RacetrackSurface.Accumulator.BatchedPaintMatchesSequential",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceBatchedPaintMatchesSequential::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	const FSurfaceTrackRibbon Ribbon = MakeSquareTestRibbon(512);
	const int32 StrokeCount = 12;
	const int32 GridSteps = 16;
	const int32 GridSpacing = 4;

	const TArray<FSurfaceBrushStroke> Strokes = MakeOverlappingFannedStrokes(Ribbon, StrokeCount);

	TStrongObjectPtr<USurfaceAccumulationBuffer> Batched(NewObject<USurfaceAccumulationBuffer>());
	TStrongObjectPtr<USurfaceAccumulationBuffer> Sequential(NewObject<USurfaceAccumulationBuffer>());

	if (!Batched->Allocate(World, Ribbon, RTF_RGBA16f) || !Sequential->Allocate(World, Ribbon, RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate the comparison buffers."));
		return false;
	}

	Batched->Clear(World);
	Batched->PaintStrokes(World, Strokes);
	FlushRenderingCommands();

	Sequential->Clear(World);

	for (const FSurfaceBrushStroke& Stroke : Strokes)
	{
		Sequential->PaintStrokes(World, TArray<FSurfaceBrushStroke>({ Stroke }));
	}

	FlushRenderingCommands();

	const TArray<FLinearColor> BatchedSamples =
		ReadCentredTexelWindow(World, Batched->GetTarget(), GridSteps, GridSpacing);
	const TArray<FLinearColor> SequentialSamples =
		ReadCentredTexelWindow(World, Sequential->GetTarget(), GridSteps, GridSpacing);

	if (BatchedSamples.Num() != SequentialSamples.Num())
	{
		AddError(TEXT("Grid readback returned mismatched sample counts."));
		return false;
	}

	float MaxDifference = 0.0f;
	float MaxBatchedValue = 0.0f;
	int32 InkedSamples = 0;

	for (int32 Index = 0; Index < BatchedSamples.Num(); ++Index)
	{
		const float BatchedValue = BatchedSamples[Index].G;
		const float SequentialValue = SequentialSamples[Index].G;

		MaxDifference = FMath::Max(MaxDifference, FMath::Abs(BatchedValue - SequentialValue));
		MaxBatchedValue = FMath::Max(MaxBatchedValue, BatchedValue);

		if (BatchedValue > 0.0f)
		{
			++InkedSamples;
		}
	}

	AddInfo(FString::Printf(
		TEXT("%d strokes at %d distinct headings, %d grid samples, %d inked, peak %.5f, max batched-vs-sequential difference %.6f"),
		StrokeCount,
		StrokeCount,
		BatchedSamples.Num(),
		InkedSamples,
		MaxBatchedValue,
		MaxDifference));

	TestTrue(
		TEXT("The batched buffer actually contains ink, so the comparison is not two empty buffers"),
		InkedSamples > 0 && MaxBatchedValue > 0.01f);

	TestTrue(
		TEXT("The sampled window covers overlapping strokes, so blend ordering is actually exercised"),
		MaxBatchedValue > 0.15f);

	TestTrue(
		TEXT("One batched paint call produces the same accumulation as one call per stroke"),
		MaxDifference < 0.002f);

	Batched->Release();
	Sequential->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfacePrecisionFloor,
	"RacetrackSurface.Accumulator.PrecisionFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfacePrecisionFloor::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	const FSurfaceTrackRibbon Ribbon = MakeSquareTestRibbon(256);
	const float Increments[] = { 0.05f, 0.01f, 0.004f, 0.002f, 0.001f };
	const int32 PassCount = 200;

	AddInfo(TEXT("Format | increment | value after 200 additive passes | expected | first pass that moved the texel"));

	for (const FFormatCandidate& Candidate : FormatCandidates)
	{
		if (!IsReadableFormat(Candidate.Format))
		{
			AddWarning(FString::Printf(
				TEXT("%s is not readable through ReadRenderTargetRawPixel, skipped."), Candidate.Name));
			continue;
		}

		for (const float Increment : Increments)
		{
			TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

			if (!Buffer->Allocate(World, Ribbon, Candidate.Format))
			{
				AddError(FString::Printf(TEXT("Failed to allocate in %s"), Candidate.Name));
				continue;
			}

			Buffer->Clear(World);

			const TArray<FSurfaceBrushStroke> Stroke =
				MakeCentredStroke(Ribbon, Increment, EAccumulationChannel::SkidMark);

			int32 FirstMovingPass = 0;

			for (int32 Pass = 1; Pass <= PassCount; ++Pass)
			{
				Buffer->PaintStrokes(World, Stroke, ESurfacePaintMode::Additive);

				if (FirstMovingPass == 0)
				{
					FlushRenderingCommands();

					if (ReadCentreTexel(World, Buffer->GetTarget()).G > 0.0f)
					{
						FirstMovingPass = Pass;
					}
				}
			}

			FlushRenderingCommands();

			const FLinearColor Final = ReadCentreTexel(World, Buffer->GetTarget());
			const float Expected = FMath::Min(Increment * PassCount, 1.0f);

			AddInfo(FString::Printf(
				TEXT("%s | %.4f | %.5f | %.5f | %s"),
				Candidate.Name,
				Increment,
				Final.G,
				Expected,
				FirstMovingPass == 0 ? TEXT("never") : *FString::Printf(TEXT("%d"), FirstMovingPass)));

			Buffer->Release();
		}
	}

	return true;
}

#endif
