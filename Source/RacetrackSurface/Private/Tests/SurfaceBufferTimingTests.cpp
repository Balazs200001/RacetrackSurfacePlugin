#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "RHIGlobals.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"
#include "Tests/SurfaceGpuTiming.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;
using RacetrackSurfaceTest::SampleGpuWork;
using RacetrackSurfaceTest::TimingSampleCount;

namespace
{
	constexpr double TargetFrameHertz = 60.0;
	constexpr double GateBudgetMilliseconds = 0.5;

	TArray<FSurfaceBrushStroke> MakeContactStrokes(const FSurfaceRegion& Region, int32 Count)
	{
		const double FrameTravel = RaceSpeedCentimetresPerSecond / TargetFrameHertz;
		const double NormalisedLength = Region.WorldDistanceToNormalised(FrameTravel);
		const float WidthInTexels = static_cast<float>(Region.WorldDistanceToTexels(20.0));

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

	const FSurfaceRegion Region = RacetrackSurfaceTest::MakeTestRegion();

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Region, RTF_RGBA8))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer."));
		return false;
	}

	const TArray<FSurfaceBrushStroke> SingleContact = MakeContactStrokes(Region, 1);
	const TArray<FSurfaceBrushStroke> FourContacts = MakeContactStrokes(Region, 4);
	const TArray<FSurfaceBrushStroke> EightyContacts = MakeContactStrokes(Region, 80);

	Buffer->PaintStrokes(World, FourContacts);
	Buffer->ApplyDecay(World, 0.99f);
	FlushRenderingCommands();

	double SingleMedian = 0.0;
	double SingleMinimum = 0.0;
	double FourMedian = 0.0;
	double FourMinimum = 0.0;
	double EightyMedian = 0.0;
	double EightyMinimum = 0.0;
	double DecayMedian = 0.0;
	double DecayMinimum = 0.0;

	const bool bSampled =
		SampleGpuWork([&]() { Buffer->PaintStrokes(World, SingleContact); }, SingleMedian, SingleMinimum)
		&& SampleGpuWork([&]() { Buffer->PaintStrokes(World, FourContacts); }, FourMedian, FourMinimum)
		&& SampleGpuWork([&]() { Buffer->PaintStrokes(World, EightyContacts); }, EightyMedian, EightyMinimum)
		&& SampleGpuWork([&]() { Buffer->ApplyDecay(World, 0.99f); }, DecayMedian, DecayMinimum);

	if (!bSampled)
	{
		AddError(TEXT("Timestamp query readback failed despite reported support."));
		return false;
	}

	AddInfo(FString::Printf(TEXT("Region %.0f cm square at %d texels, %.2f cm per texel, %lld bytes"),
		Region.WorldSize,
		Region.ResolutionTexels,
		Region.CentimetresPerTexel(),
		Buffer->GetAllocatedBytes()));

	AddInfo(FString::Printf(TEXT("Samples %d, stroke length %.1f cm at %.0f Hz"),
		TimingSampleCount,
		RaceSpeedCentimetresPerSecond / TargetFrameHertz,
		TargetFrameHertz));

	AddInfo(FString::Printf(TEXT("Paint 1 contact: median %.4f ms, min %.4f ms"), SingleMedian, SingleMinimum));
	AddInfo(FString::Printf(TEXT("Paint 4 contacts: median %.4f ms, min %.4f ms"), FourMedian, FourMinimum));
	AddInfo(FString::Printf(TEXT("Paint 80 contacts: median %.4f ms, min %.4f ms"), EightyMedian, EightyMinimum));
	AddInfo(FString::Printf(TEXT("Full buffer decay: median %.4f ms, min %.4f ms"), DecayMedian, DecayMinimum));

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

#endif
