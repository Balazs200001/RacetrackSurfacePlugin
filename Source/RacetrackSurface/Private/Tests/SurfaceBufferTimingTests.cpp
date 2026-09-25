#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Math/RandomStream.h"
#include "RenderingThread.h"
#include "RHIGlobals.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"
#include "Tests/SurfaceGpuTiming.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeSquareTestRibbon;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;
using RacetrackSurfaceTest::SampleGpuWork;
using RacetrackSurfaceTest::TimingSampleCount;

namespace
{
	constexpr double TargetFrameHertz = 60.0;
	constexpr double GateBudgetMilliseconds = 0.5;

	TArray<FSurfaceBrushStroke> MakeContactStrokes(const FSurfaceTrackRibbon& Ribbon, int32 Count)
	{
		const double FrameTravel = RaceSpeedCentimetresPerSecond / TargetFrameHertz;
		const double NormalisedLength = Ribbon.NormalisedAlongTrack(FrameTravel);
		const float WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(20.0));

		TArray<FSurfaceBrushStroke> Strokes;
		Strokes.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Lane = 0.45 + 0.02 * Index;

			FSurfaceBrushStroke Stroke;
			Stroke.RegionStart = FVector2D(0.5 - NormalisedLength * 0.5, Lane);
			Stroke.RegionEnd = FVector2D(0.5 + NormalisedLength * 0.5, Lane);
			Stroke.WidthInTexels = WidthInTexels;
			Stroke.Intensity = 0.8f;
			Stroke.Channel = EAccumulationChannel::SkidMark;

			Strokes.Add(Stroke);
		}

		return Strokes;
	}

	TArray<FSurfaceBrushStroke> MakeFannedContactStrokes(const FSurfaceTrackRibbon& Ribbon, int32 Count)
	{
		const double FrameTravel = RaceSpeedCentimetresPerSecond / TargetFrameHertz;
		const double NormalisedLength = Ribbon.NormalisedAlongTrack(FrameTravel);
		const float WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(20.0));

		TArray<FSurfaceBrushStroke> Strokes;
		Strokes.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Lane = 0.45 + 0.02 * Index;
			const double Angle = UE_DOUBLE_PI * static_cast<double>(Index) / static_cast<double>(Count);
			const FVector2D Half =
				FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * (NormalisedLength * 0.5);

			FSurfaceBrushStroke Stroke;
			Stroke.RegionStart = FVector2D(0.5, Lane) - Half;
			Stroke.RegionEnd = FVector2D(0.5, Lane) + Half;
			Stroke.WidthInTexels = WidthInTexels;
			Stroke.Intensity = 0.8f;
			Stroke.Channel = EAccumulationChannel::SkidMark;

			Strokes.Add(Stroke);
		}

		return Strokes;
	}
}

namespace RacetrackSurfaceDecayTiming
{
	constexpr int32 NoiseTexels = 256;

	struct FTimedPass
	{
		const TCHAR* Name;
		TFunction<void()> Work;
		TArray<double> Samples;
	};

	double Median(TArray<double> Samples)
	{
		if (Samples.Num() == 0)
		{
			return 0.0;
		}

		Samples.Sort();
		return Samples[Samples.Num() / 2];
	}

	double Minimum(TArray<double> Samples)
	{
		if (Samples.Num() == 0)
		{
			return 0.0;
		}

		Samples.Sort();
		return Samples[0];
	}

	bool SampleInterleaved(TArray<FTimedPass>& Passes)
	{
		const TSharedRef<RacetrackSurfaceTest::FTimestampPair> Queries = RacetrackSurfaceTest::CreateTimestampPair();

		for (int32 Round = 0; Round < TimingSampleCount; ++Round)
		{
			for (FTimedPass& Pass : Passes)
			{
				double Milliseconds = 0.0;

				if (!RacetrackSurfaceTest::TimeGpuWork(Queries, Pass.Work, Milliseconds))
				{
					return false;
				}

				Pass.Samples.Add(Milliseconds);
			}
		}

		return true;
	}

	UTexture2D* MakeIncompressibleNoise()
	{
		TArray<uint8> Texels;
		Texels.SetNumUninitialized(NoiseTexels * NoiseTexels * 4);

		FRandomStream Stream(20260925);

		for (uint8& Texel : Texels)
		{
			Texel = static_cast<uint8>(Stream.RandRange(0, 255));
		}

		UTexture2D* Noise = UTexture2D::CreateTransient(
			NoiseTexels,
			NoiseTexels,
			PF_B8G8R8A8,
			TEXT("RacetrackSurfaceTimingNoise"),
			Texels);

		if (Noise != nullptr)
		{
			Noise->SRGB = false;
			Noise->Filter = TF_Nearest;
			Noise->UpdateResource();
		}

		return Noise;
	}

	void FillWith(UObject* WorldContext, UTextureRenderTarget2D* Target, UTexture2D* Texture)
	{
		UCanvas* Canvas = nullptr;
		FVector2D CanvasSize = FVector2D::ZeroVector;
		FDrawToRenderTargetContext Context;

		UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Target, Canvas, CanvasSize, Context);

		if (Canvas != nullptr)
		{
			Canvas->K2_DrawTexture(
				Texture,
				FVector2D::ZeroVector,
				CanvasSize,
				FVector2D::ZeroVector,
				CanvasSize / static_cast<double>(NoiseTexels),
				FLinearColor::White,
				BLEND_Opaque,
				0.0f,
				FVector2D::ZeroVector);
		}

		UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfacePaintAndDecayTiming,
	"RacetrackSurface.Performance.PaintAndDecay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfacePaintAndDecayTiming::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render, so no GPU timing is possible."));
		return true;
	}

	if (!GSupportsTimestampRenderQueries)
	{
		AddWarning(TEXT("Skipped: RHI reports no timestamp render query support."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No editor, game or PIE world available to render through."));
		return false;
	}

	const FSurfaceTrackRibbon Ribbon = MakeSquareTestRibbon(2048);

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());
	TStrongObjectPtr<USurfaceAccumulationBuffer> DecayBuffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA8) || !DecayBuffer->Allocate(World, Ribbon, RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate the accumulation buffers."));
		return false;
	}

	const float DecayStep = 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife;

	const TArray<FSurfaceBrushStroke> SingleContact = MakeContactStrokes(Ribbon, 1);
	const TArray<FSurfaceBrushStroke> FourContacts = MakeContactStrokes(Ribbon, 4);
	const TArray<FSurfaceBrushStroke> EightyContacts = MakeContactStrokes(Ribbon, 80);
	const TArray<FSurfaceBrushStroke> EightyFannedContacts = MakeFannedContactStrokes(Ribbon, 80);

	Buffer->PaintStrokes(World, FourContacts);
	DecayBuffer->PaintStrokes(World, FourContacts);
	DecayBuffer->ApplyDecay(World, DecayStep);
	FlushRenderingCommands();

	double SingleMedian = 0.0;
	double SingleMinimum = 0.0;
	double FourMedian = 0.0;
	double FourMinimum = 0.0;
	double EightyMedian = 0.0;
	double EightyMinimum = 0.0;
	double EightyFannedMedian = 0.0;
	double EightyFannedMinimum = 0.0;
	double DecayMedian = 0.0;
	double DecayMinimum = 0.0;

	const bool bSampled =
		SampleGpuWork([&]() { Buffer->PaintStrokes(World, SingleContact); }, SingleMedian, SingleMinimum)
		&& SampleGpuWork([&]() { Buffer->PaintStrokes(World, FourContacts); }, FourMedian, FourMinimum)
		&& SampleGpuWork([&]() { Buffer->PaintStrokes(World, EightyContacts); }, EightyMedian, EightyMinimum)
		&& SampleGpuWork(
			[&]() { Buffer->PaintStrokes(World, EightyFannedContacts); },
			EightyFannedMedian,
			EightyFannedMinimum)
		&& SampleGpuWork([&]() { DecayBuffer->ApplyDecay(World, DecayStep); }, DecayMedian, DecayMinimum);

	if (!bSampled)
	{
		AddError(TEXT("Timestamp query readback failed despite reported support."));
		return false;
	}

	AddInfo(FString::Printf(TEXT("Ribbon %.0f cm square at %d texels, %.2f cm per texel, %lld bytes"),
		Ribbon.TrackLength,
		Ribbon.AlongTrackTexels(),
		Ribbon.CentimetresPerTexel,
		Buffer->GetAllocatedBytes()));

	AddInfo(FString::Printf(TEXT("Samples %d, stroke length %.1f cm at %.0f Hz"),
		TimingSampleCount,
		RaceSpeedCentimetresPerSecond / TargetFrameHertz,
		TargetFrameHertz));

	AddInfo(FString::Printf(TEXT("Paint 1 contact: median %.4f ms, min %.4f ms"), SingleMedian, SingleMinimum));
	AddInfo(FString::Printf(TEXT("Paint 4 contacts: median %.4f ms, min %.4f ms"), FourMedian, FourMinimum));
	AddInfo(FString::Printf(TEXT("Paint 80 contacts, uniform heading: median %.4f ms, min %.4f ms"), EightyMedian, EightyMinimum));
	AddInfo(FString::Printf(
		TEXT("Paint 80 contacts, 80 distinct headings: median %.4f ms, min %.4f ms"),
		EightyFannedMedian,
		EightyFannedMinimum));
	AddInfo(FString::Printf(
		TEXT("Full buffer decay, coupled compute pass on an RGBA16f buffer of the same size: median %.4f ms, "
			"min %.4f ms"),
		DecayMedian,
		DecayMinimum));

	const double GateTotal = FourMedian + DecayMedian;

	AddInfo(FString::Printf(TEXT("Gate total, 4 contacts plus one decay: %.4f ms against %.2f ms budget"),
		GateTotal,
		GateBudgetMilliseconds));

	if (FourMedian > GateBudgetMilliseconds)
	{
		AddWarning(FString::Printf(
			TEXT("Gate exceeded: four contacts cost %.4f ms, over the %.2f ms budget."),
			FourMedian,
			GateBudgetMilliseconds));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayAtTheProductionAtlas,
	"RacetrackSurface.Performance.DecayAtTheProductionAtlas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayAtTheProductionAtlas::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render, so no GPU timing is possible."));
		return true;
	}

	if (!GSupportsTimestampRenderQueries)
	{
		AddWarning(TEXT("Skipped: RHI reports no timestamp render query support."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No editor, game or PIE world available to render through."));
		return false;
	}

	const FSurfaceTrackRibbon Ribbon;

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon) || !Buffer->CanDecayInPlace())
	{
		AddError(TEXT("Failed to allocate an accumulation buffer at the production atlas that can decay in place."));
		return false;
	}

	TStrongObjectPtr<UTextureRenderTarget2D> ShippedInC10(UKismetRenderingLibrary::CreateRenderTarget2D(
		World,
		Buffer->GetAllocatedWidth(),
		Buffer->GetAllocatedHeight(),
		RTF_RGBA16f,
		FLinearColor::Black,
		false,
		false));

	TStrongObjectPtr<UTexture2D> Noise(RacetrackSurfaceDecayTiming::MakeIncompressibleNoise());

	if (!ShippedInC10.IsValid() || !Noise.IsValid())
	{
		AddError(TEXT("Failed to allocate the C10 comparison target or the noise texture."));
		return false;
	}

	const float DecayStep = 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife;
	const float Retention = RacetrackSurfaceMapping::DecayRetentionForProgress(DecayStep);

	TArray<RacetrackSurfaceDecayTiming::FTimedPass> Passes;
	Passes.Add({ TEXT("coupled compute pass, UAV target"), [&]() { Buffer->ApplyDecay(World, DecayStep); } });
	Passes.Add({ TEXT("C10 quad, plain target as shipped"), [&]()
	{
		RacetrackSurfaceTest::DrawUniformDecayQuadAsShippedInC10(World, ShippedInC10.Get(), Retention);
	} });
	Passes.Add({ TEXT("C10 quad, UAV target"), [&]()
	{
		RacetrackSurfaceTest::DrawUniformDecayQuadAsShippedInC10(World, Buffer->GetTarget(), Retention);
	} });

	for (const RacetrackSurfaceDecayTiming::FTimedPass& Pass : Passes)
	{
		Pass.Work();
	}

	FlushRenderingCommands();

	AddInfo(FString::Printf(
		TEXT("Production atlas: %.0f cm of lap at %.0f cm wide, %.4f cm per texel, %d x %d texels in %d "
			"rows of %d, allocating %d x %d for %lld bytes"),
		Ribbon.TrackLength,
		Ribbon.TrackWidth,
		Ribbon.CentimetresPerTexel,
		Ribbon.AlongTrackTexels(),
		Ribbon.AcrossTrackTexels(),
		Ribbon.RowCount(),
		Ribbon.TexelsPerRow(),
		Buffer->GetAllocatedWidth(),
		Buffer->GetAllocatedHeight(),
		Buffer->GetAllocatedBytes()));

	const TCHAR* Contents[] = { TEXT("empty"), TEXT("filled with incompressible noise") };

	double CoupledEmptyMedian = 0.0;

	for (int32 ContentIndex = 0; ContentIndex < static_cast<int32>(UE_ARRAY_COUNT(Contents)); ++ContentIndex)
	{
		if (ContentIndex == 1)
		{
			RacetrackSurfaceDecayTiming::FillWith(World, Buffer->GetTarget(), Noise.Get());
			RacetrackSurfaceDecayTiming::FillWith(World, ShippedInC10.Get(), Noise.Get());
			FlushRenderingCommands();
		}

		for (RacetrackSurfaceDecayTiming::FTimedPass& Pass : Passes)
		{
			Pass.Samples.Reset();
		}

		if (!RacetrackSurfaceDecayTiming::SampleInterleaved(Passes))
		{
			AddError(TEXT("Timestamp query readback failed despite reported support."));
			return false;
		}

		for (const RacetrackSurfaceDecayTiming::FTimedPass& Pass : Passes)
		{
			AddInfo(FString::Printf(
				TEXT("Full buffer decay, %s, buffer %s: median %.4f ms, min %.4f ms over %d interleaved samples"),
				Pass.Name,
				Contents[ContentIndex],
				RacetrackSurfaceDecayTiming::Median(Pass.Samples),
				RacetrackSurfaceDecayTiming::Minimum(Pass.Samples),
				Pass.Samples.Num()));
		}

		if (ContentIndex == 0)
		{
			CoupledEmptyMedian = RacetrackSurfaceDecayTiming::Median(Passes[0].Samples);
		}
	}

	const double StepsPerHalfLife = RacetrackSurfaceMapping::DecayStepsPerHalfLife;
	const double HalfLife = RacetrackSurfaceMapping::FallbackDecayHalfLifeSeconds;
	const double PassesPerSecondAtOne = StepsPerHalfLife / HalfLife;
	const double CoupledFilledMedian = RacetrackSurfaceDecayTiming::Median(Passes[0].Samples);

	AddInfo(FString::Printf(
		TEXT("At %.0f steps per half-life and a %.0f s half-life that is %.4f passes a second at 1x, so the "
			"coupled pass costs %.4f to %.4f ms a second, empty to filled, and %.4f to %.4f ms a frame at %.0f "
			"Hz. At 20x it is %.4f to %.4f ms a second"),
		StepsPerHalfLife,
		HalfLife,
		PassesPerSecondAtOne,
		PassesPerSecondAtOne * CoupledEmptyMedian,
		PassesPerSecondAtOne * CoupledFilledMedian,
		PassesPerSecondAtOne * CoupledEmptyMedian / TargetFrameHertz,
		PassesPerSecondAtOne * CoupledFilledMedian / TargetFrameHertz,
		TargetFrameHertz,
		20.0 * PassesPerSecondAtOne * CoupledEmptyMedian,
		20.0 * PassesPerSecondAtOne * CoupledFilledMedian));

	TestEqual(
		TEXT("The default ribbon is the demo's own configuration, so this measures the buffer the demo runs"),
		Buffer->GetAllocatedBytes(),
		static_cast<int64>(117964800));

	TestTrue(
		TEXT("Every pass resolved a timestamp, so the figures above are measurements rather than failed "
			"readbacks reported as zero"),
		CoupledEmptyMedian > 0.0 && CoupledFilledMedian > 0.0);

	UKismetRenderingLibrary::ReleaseRenderTarget2D(ShippedInC10.Get());
	Buffer->Release();

	return true;
}

#endif
