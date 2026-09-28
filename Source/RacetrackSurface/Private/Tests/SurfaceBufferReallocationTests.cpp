#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr double PaintedLapFraction = 0.70;

	FSurfaceTrackRibbon MakeSmallRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackRibbon MakeDemoSizedRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 180001.0;
		Ribbon.TrackWidth = 800.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	void PaintOneContact(USurfaceAccumulationSubsystem& Subsystem, const FSurfaceTrackRibbon& Ribbon)
	{
		Subsystem.RegisterSurfaceContactInTrackSpace(
			Ribbon.TrackLength * PaintedLapFraction,
			0.0,
			RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond,
			1.0f,
			20.0f);

		Subsystem.Tick(1.0f / 60.0f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceBufferGrowingTheRibbonAfterPaintingStaysSafe,
	"RacetrackSurface.Accumulator.GrowingTheRibbonAfterPaintingStaysSafe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceBufferGrowingTheRibbonAfterPaintingStaysSafe::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = RacetrackSurfaceTest::FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No renderable world available."));
		return false;
	}

	USurfaceAccumulationSubsystem* Subsystem = World->GetSubsystem<USurfaceAccumulationSubsystem>();

	if (Subsystem == nullptr || !Subsystem->IsInitialized())
	{
		AddWarning(TEXT("Skipped: the world's accumulation subsystem is not initialised."));
		return true;
	}

	TStrongObjectPtr<USplineComponent> Circuit = RacetrackSurfaceTest::MakeTestCircuit();

	const FSurfaceTrackRibbon Small = MakeSmallRibbon();
	const FSurfaceTrackRibbon Large = MakeDemoSizedRibbon();

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), Small);
	Subsystem->ResetSurface();
	PaintOneContact(*Subsystem, Small);

	const USurfaceAccumulationBuffer* Buffer = Subsystem->GetBuffer();

	if (Buffer == nullptr)
	{
		AddError(TEXT("Painting into the small ribbon allocated no buffer."));
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("Painted into a %d x %d buffer, then grew the ribbon to %d x %d, %lld bytes"),
		Buffer->GetAllocatedWidth(),
		Buffer->GetAllocatedHeight(),
		Large.AtlasWidth(),
		Large.AtlasHeight(),
		static_cast<int64>(Large.AtlasTexelCount()) * RacetrackSurfaceTest::ProductionBytesPerTexel()));

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), Large);
	PaintOneContact(*Subsystem, Large);

	const USurfaceAccumulationBuffer* Grown = Subsystem->GetBuffer();

	if (Grown == nullptr || Grown->GetTarget() == nullptr)
	{
		AddError(TEXT("Growing the ribbon left no buffer behind."));
		return false;
	}

	TestEqual(TEXT("The buffer grew to the new ribbon"), Grown->GetAllocatedWidth(), Large.AtlasWidth());
	TestEqual(TEXT("The buffer grew to the new ribbon"), Grown->GetAllocatedHeight(), Large.AtlasHeight());

	FlushRenderingCommands();

	const FVector2D Atlas = Large.RibbonToAtlas(
		Large.NormalisedAlongTrack(Large.TrackLength * PaintedLapFraction),
		FSurfaceTrackRibbon::NormalisedAcrossTrack(0.0));

	const int32 Column = FMath::Clamp(
		FMath::FloorToInt32(Atlas.X * Large.AtlasWidth()), 0, Large.AtlasWidth() - 1);

	const int32 Row = FMath::Clamp(
		FMath::FloorToInt32(Atlas.Y * Large.AtlasHeight()), 0, Large.AtlasHeight() - 1);

	const TArray<FLinearColor> Texel = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Grown->GetTarget(), Column, Row, 1, 1, false);

	TestEqual(TEXT("The grown buffer reads back"), Texel.Num(), 1);

	if (Texel.Num() == 1)
	{
		TestTrue(
			FString::Printf(
				TEXT("The contact painted after growing the ribbon actually landed, %.6f at texel %d, %d"),
				Texel[0].G,
				Column,
				Row),
			Texel[0].G > 0.0f);
	}

	Subsystem->ResetSurface();

	return true;
}

#endif
