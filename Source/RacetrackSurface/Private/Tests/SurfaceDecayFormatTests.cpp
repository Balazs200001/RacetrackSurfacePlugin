#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "PixelFormat.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeSquareTestRibbon;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceBufferDecaysInPlaceInTheProductionFormat,
	"RacetrackSurface.Decay.BufferDecaysInPlaceInTheProductionFormat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceBufferDecaysInPlaceInTheProductionFormat::RunTest(const FString& Parameters)
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

	const EPixelFormat CandidateFormats[] = { PF_FloatRGBA, PF_A32B32G32R32F, PF_B8G8R8A8 };

	const auto Supported = [](EPixelFormat Format, EPixelFormatCapabilities Capability)
	{
		return RHIPixelFormatHasCapabilities(Format, Capability) ? TEXT("yes") : TEXT("no");
	};

	AddInfo(TEXT("format | typed UAV loads | render target | blendable | sampled and filterable"));

	for (const EPixelFormat Candidate : CandidateFormats)
	{
		AddInfo(FString::Printf(
			TEXT("%s | %s | %s | %s | %s"),
			GPixelFormats[Candidate].Name,
			Supported(Candidate, EPixelFormatCapabilities::TypedUAVLoad),
			Supported(Candidate, EPixelFormatCapabilities::RenderTarget),
			Supported(Candidate, EPixelFormatCapabilities::TextureBlendable),
			Supported(Candidate, EPixelFormatCapabilities::TextureFilterable)));
	}

	const EPixelFormat ProductionPixelFormat =
		GetPixelFormatFromRenderTargetFormat(USurfaceAccumulationBuffer::ProductionFormat);

	TestEqual(
		TEXT("The production format is RGBA32f, because RGBA16f cannot hold the marble fan's increments"),
		USurfaceAccumulationBuffer::ProductionFormat,
		RTF_RGBA32f);

	TestTrue(
		TEXT("The RHI reports typed UAV loads for the production format, which the coupled decay pass reads its "
			"texels through, so an RHI without them fails here rather than silently never decaying in place"),
		RHIIsTypedUAVLoadSupported(ProductionPixelFormat));

	TestTrue(
		TEXT("The RHI reports the production format as a render target it can blend into, which every canvas "
			"write, additive and modulate, depends on"),
		RHIPixelFormatHasCapabilities(
			ProductionPixelFormat,
			EPixelFormatCapabilities::RenderTarget | EPixelFormatCapabilities::TextureBlendable));

	TestTrue(
		TEXT("The RHI reports the production format as sampled with filtering, which the material's bilinear "
			"across the track depends on. D3D12 sets this from the format's shader-sample support, so the "
			"read-path agreement test is what shows the filtering itself"),
		RHIPixelFormatHasCapabilities(ProductionPixelFormat, EPixelFormatCapabilities::TextureFilterable));

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, MakeSquareTestRibbon(64)))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer in its default format."));
		return false;
	}

	TestEqual(
		TEXT("The default allocation is the production format"),
		Buffer->GetAllocatedFormat(),
		USurfaceAccumulationBuffer::ProductionFormat);

	TestTrue(
		TEXT("The default allocation carries a UAV, so it can decay in place"),
		Buffer->CanDecayInPlace());

	TestTrue(
		TEXT("A decay pass on the default allocation is issued rather than skipped"),
		Buffer->ApplyDecay(World, 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife));

	TestFalse(
		TEXT("No progress means no pass, so a zero step cannot cost a dispatch"),
		Buffer->ApplyDecay(World, 0.0f));

	FlushRenderingCommands();
	Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceBufferWithoutAUavDecaysUniformlyOnTheCanvas,
	"RacetrackSurface.Decay.BufferWithoutAUavDecaysUniformlyOnTheCanvas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceBufferWithoutAUavDecaysUniformlyOnTheCanvas::RunTest(const FString& Parameters)
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

	constexpr int32 Resolution = 64;
	constexpr float HalfLivesElapsed = 0.25f;

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, MakeSquareTestRibbon(Resolution), RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate an RGBA16f accumulation buffer."));
		return false;
	}

	TestFalse(
		TEXT("An RGBA16f buffer carries no UAV, because the in-place pass stores full floats and its UAV store "
			"into a half float truncates"),
		Buffer->CanDecayInPlace());

	FSurfaceBrushStroke Rubber;
	Rubber.RegionStart = FVector2D(0.1, 0.5);
	Rubber.RegionEnd = FVector2D(0.9, 0.5);
	Rubber.WidthInTexels = 20.0f;
	Rubber.Intensity = 2.0f;
	Rubber.Channel = EAccumulationChannel::RubberBuildup;

	FSurfaceBrushStroke Skid;
	Skid.RegionStart = FVector2D(0.5, 0.1);
	Skid.RegionEnd = FVector2D(0.5, 0.9);
	Skid.WidthInTexels = 20.0f;
	Skid.Intensity = 1.0f;
	Skid.Channel = EAccumulationChannel::SkidMark;

	Buffer->Clear(World);
	Buffer->PaintStrokes(World, { Rubber, Skid });
	FlushRenderingCommands();

	const TArray<FLinearColor> Before = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Buffer->GetTarget(), 0, 0, Resolution, Resolution, false);

	TestTrue(
		TEXT("Without a UAV the buffer still decays, on the canvas, rather than skipping the pass"),
		Buffer->ApplyDecay(World, HalfLivesElapsed));

	FlushRenderingCommands();

	const TArray<FLinearColor> After = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Buffer->GetTarget(), 0, 0, Resolution, Resolution, false);

	const float SharedRetention = RacetrackSurfaceMapping::DecayRetentionForProgress(HalfLivesElapsed);

	int32 SkidOverRubber = 0;
	int32 SkidAtTheSharedRate = 0;
	float WorstRubberError = 0.0f;

	for (int32 Index = 0; Index < Before.Num() && Index < After.Num(); ++Index)
	{
		if (Before[Index].R > 0.01f)
		{
			WorstRubberError = FMath::Max(
				WorstRubberError,
				FMath::Abs(After[Index].R / Before[Index].R - SharedRetention));
		}

		if (Before[Index].R > 0.01f && Before[Index].G > 0.01f)
		{
			++SkidOverRubber;

			if (FMath::IsNearlyEqual(After[Index].G / Before[Index].G, SharedRetention, 1.0e-3f))
			{
				++SkidAtTheSharedRate;
			}
		}
	}

	AddInfo(FString::Printf(
		TEXT("Uniform canvas fallback on RGBA16f at %.2f half-lives: %d of %d texels with skid over rubber kept "
			"skid at the shared retention %.6f, and the worst rubber texel was %.3e off it"),
		HalfLivesElapsed,
		SkidAtTheSharedRate,
		SkidOverRubber,
		SharedRetention,
		WorstRubberError));

	TestTrue(TEXT("The fixture crosses skid over rubber"), SkidOverRubber > 100);

	TestEqual(
		TEXT("On the fallback every skid texel decays at the shared rate however much rubber is under it, so "
			"the coupling is absent rather than half-applied"),
		SkidAtTheSharedRate,
		SkidOverRubber);

	TestTrue(TEXT("On the fallback rubber decays at the shared rate"), WorstRubberError < 1.0e-3f);

	Buffer->Release();

	return true;
}

#endif
