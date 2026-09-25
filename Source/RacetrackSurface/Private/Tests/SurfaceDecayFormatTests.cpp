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

	for (const EPixelFormat Candidate : CandidateFormats)
	{
		AddInfo(FString::Printf(
			TEXT("%s: typed UAV loads %s on this RHI"),
			GPixelFormats[Candidate].Name,
			RHIIsTypedUAVLoadSupported(Candidate) ? TEXT("supported") : TEXT("not supported")));
	}

	TestTrue(
		TEXT("The RHI reports typed UAV loads for PF_FloatRGBA, which the coupled decay pass reads its texels "
			"through, so an RHI without them fails here rather than silently never decaying"),
		RHIIsTypedUAVLoadSupported(PF_FloatRGBA));

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, MakeSquareTestRibbon(64)))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer in its default format."));
		return false;
	}

	TestEqual(
		TEXT("The default allocation is the production RGBA16f format"),
		Buffer->GetAllocatedFormat(),
		RTF_RGBA16f);

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

	if (!Buffer->Allocate(World, MakeSquareTestRibbon(Resolution), RTF_RGBA32f))
	{
		AddError(TEXT("Failed to allocate an RGBA32f accumulation buffer."));
		return false;
	}

	TestFalse(
		TEXT("An RGBA32f buffer carries no UAV, because in-place decay rounds to half floats"),
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
		TEXT("Uniform canvas fallback on RGBA32f at %.2f half-lives: %d of %d texels with skid over rubber kept "
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
