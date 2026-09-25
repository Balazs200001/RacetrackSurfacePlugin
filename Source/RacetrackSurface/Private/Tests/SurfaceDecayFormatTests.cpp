#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/World.h"
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
		Buffer->ApplyDecay(1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife));

	TestFalse(
		TEXT("No progress means no pass, so a zero step cannot cost a dispatch"),
		Buffer->ApplyDecay(0.0f));

	FlushRenderingCommands();
	Buffer->Release();

	return true;
}

#endif
