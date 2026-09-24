#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;

namespace
{
	constexpr int32 MarbleBufferResolution = 512;
	constexpr double MarbleContactWidthCentimetres = 20.0;

	FSurfaceBrushStroke MakeHorizontalStrokeInChannel(
		const FSurfaceTrackRibbon& Ribbon,
		double AcrossTrack,
		EAccumulationChannel Channel)
	{
		FSurfaceBrushStroke Stroke;
		Stroke.RegionStart = FVector2D(0.3, AcrossTrack);
		Stroke.RegionEnd = FVector2D(0.7, AcrossTrack);
		Stroke.WidthInTexels =
			static_cast<float>(Ribbon.WorldDistanceToTexels(MarbleContactWidthCentimetres));
		Stroke.Intensity = 1.0f;
		Stroke.Channel = Channel;

		return Stroke;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceMarbleStrokeWritesOnlyTheMarbleChannel,
	"RacetrackSurface.Accumulator.MarbleStrokeWritesOnlyTheMarbleChannel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceMarbleStrokeWritesOnlyTheMarbleChannel::RunTest(const FString& Parameters)
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

	FSurfaceTrackRibbon Ribbon;
	Ribbon.TrackLength = MarbleBufferResolution * Ribbon.CentimetresPerTexel;
	Ribbon.TrackWidth = MarbleBufferResolution * Ribbon.CentimetresPerTexel;

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer."));
		return false;
	}

	constexpr double MarbleAcrossTrack = 0.25;
	constexpr double BuildupAcrossTrack = 0.75;

	const TArray<FSurfaceBrushStroke> Strokes({
		MakeHorizontalStrokeInChannel(Ribbon, MarbleAcrossTrack, EAccumulationChannel::Marbles),
		MakeHorizontalStrokeInChannel(Ribbon, BuildupAcrossTrack, EAccumulationChannel::RubberBuildup) });

	Buffer->PaintStrokes(World, Strokes);
	FlushRenderingCommands();

	const int32 SampleX = MarbleBufferResolution / 2;
	const int32 MarbleY = FMath::RoundToInt32(MarbleAcrossTrack * MarbleBufferResolution);
	const int32 BuildupY = FMath::RoundToInt32(BuildupAcrossTrack * MarbleBufferResolution);

	const TArray<FLinearColor> AtMarble = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Buffer->GetTarget(), SampleX, MarbleY, 1, 1, false);

	const TArray<FLinearColor> AtBuildup = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Buffer->GetTarget(), SampleX, BuildupY, 1, 1, false);

	if (AtMarble.Num() != 1 || AtBuildup.Num() != 1)
	{
		AddError(TEXT("Readback did not return one texel per sample point."));
		Buffer->Release();
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("A marbles stroke reads back as (%.4f, %.4f, %.4f) and a rubber stroke on the same buffer "
			"reads back as (%.4f, %.4f, %.4f)"),
		AtMarble[0].R, AtMarble[0].G, AtMarble[0].B,
		AtBuildup[0].R, AtBuildup[0].G, AtBuildup[0].B));

	TestTrue(
		TEXT("A marbles stroke inks the blue channel, which nothing in the project wrote before phase G"),
		AtMarble[0].B > 0.0f);

	TestEqual(
		TEXT("A marbles stroke leaves the rubber channel alone"),
		AtMarble[0].R,
		0.0f);

	TestEqual(
		TEXT("A marbles stroke leaves the skid channel alone"),
		AtMarble[0].G,
		0.0f);

	TestTrue(
		TEXT("A rubber stroke painted into the same buffer still inks red"),
		AtBuildup[0].R > 0.0f);

	TestEqual(
		TEXT("A rubber stroke deposits no marbles, so the two channels cannot be reading each other"),
		AtBuildup[0].B,
		0.0f);

	Buffer->Release();

	return true;
}

#endif
