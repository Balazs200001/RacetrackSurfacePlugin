#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA8))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer."));
		return false;
	}

	const TArray<FSurfaceBrushStroke> SingleContact = MakeContactStrokes(Ribbon, 1);
	const TArray<FSurfaceBrushStroke> FourContacts = MakeContactStrokes(Ribbon, 4);
	const TArray<FSurfaceBrushStroke> EightyContacts = MakeContactStrokes(Ribbon, 80);
	const TArray<FSurfaceBrushStroke> EightyFannedContacts = MakeFannedContactStrokes(Ribbon, 80);

	Buffer->PaintStrokes(World, FourContacts);
	Buffer->ApplyDecay(World, 0.99f);
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
		&& SampleGpuWork([&]() { Buffer->ApplyDecay(World, 0.99f); }, DecayMedian, DecayMinimum);

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

	if (!Buffer->Allocate(World, Ribbon))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer at the production atlas."));
		return false;
	}

	Buffer->ApplyDecay(World, 0.997296f);
	FlushRenderingCommands();

	double DecayMedian = 0.0;
	double DecayMinimum = 0.0;

	if (!SampleGpuWork([&]() { Buffer->ApplyDecay(World, 0.997296f); }, DecayMedian, DecayMinimum))
	{
		AddError(TEXT("Timestamp query readback failed despite reported support."));
		return false;
	}

	const double StepsPerHalfLife = RacetrackSurfaceMapping::DecayStepsPerHalfLife;
	const double HalfLife = RacetrackSurfaceMapping::FallbackDecayHalfLifeSeconds;
	const double PassesPerSecondAtOne = StepsPerHalfLife / HalfLife;

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

	AddInfo(FString::Printf(
		TEXT("Full buffer decay: median %.4f ms, min %.4f ms over %d samples"),
		DecayMedian,
		DecayMinimum,
		TimingSampleCount));

	AddInfo(FString::Printf(
		TEXT("At %.0f steps per half-life and a %.0f s half-life that is %.4f passes a second at 1x, so "
			"%.4f ms a second and %.4f ms a frame at %.0f Hz. At 20x it is %.4f ms a second"),
		StepsPerHalfLife,
		HalfLife,
		PassesPerSecondAtOne,
		PassesPerSecondAtOne * DecayMedian,
		PassesPerSecondAtOne * DecayMedian / TargetFrameHertz,
		TargetFrameHertz,
		20.0 * PassesPerSecondAtOne * DecayMedian));

	TestEqual(
		TEXT("The default ribbon is the demo's own configuration, so this measures the buffer the demo runs"),
		Buffer->GetAllocatedBytes(),
		static_cast<int64>(117964800));

	TestTrue(
		TEXT("The decay pass resolved a timestamp, so the figure above is a measurement rather than a "
			"failed readback reported as zero"),
		DecayMedian > 0.0);

	return true;
}

#endif
