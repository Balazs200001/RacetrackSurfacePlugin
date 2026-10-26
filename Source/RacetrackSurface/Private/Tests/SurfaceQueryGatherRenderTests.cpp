// Copyright 2026 Balázs Fábián. All Rights Reserved.

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
#include "SurfaceQueryGather.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeTestCircuit;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;

namespace
{
	constexpr float GatherFrameSeconds = 1.0f / 30.0f;

	constexpr double GatherPaintedLapFraction = 0.75;

	FSurfaceTrackRibbon MakeGatherRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackContact MakeGatherContact(double Distance)
	{
		FSurfaceTrackContact Contact;
		Contact.Coordinate.DistanceAlongCentreline = Distance;
		Contact.Coordinate.LateralOffsetAlpha = 0.0;
		Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
		Contact.SlipAmount = 1.0f;
		Contact.ContactWidth = 20.0f;
		return Contact;
	}

	struct FGatherFixture
	{
		TStrongObjectPtr<USplineComponent> Circuit;
		TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem;
		TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer;
		FSurfaceTrackRibbon Ribbon;
	};

	bool BuildFixture(
		FAutomationTestBase& Test,
		UWorld* World,
		EAccumulationChannel PaintedChannel,
		FGatherFixture& Out)
	{
		Out.Ribbon = MakeGatherRibbon();
		Out.Circuit = MakeTestCircuit();
		Out.Subsystem = TStrongObjectPtr<USurfaceAccumulationSubsystem>(NewObject<USurfaceAccumulationSubsystem>());
		Out.Subsystem->ConfigureTrackRibbon(Out.Circuit.Get(), Out.Ribbon);

		TArray<FSurfaceBrushStroke> Strokes;

		Out.Subsystem->BuildStrokes(
			MakeGatherContact(Out.Ribbon.TrackLength * GatherPaintedLapFraction),
			GatherFrameSeconds,
			Strokes);

		if (Strokes.Num() == 0)
		{
			Test.AddError(TEXT("The fixture painted no strokes, so there is nothing to gather."));
			return false;
		}

		for (FSurfaceBrushStroke& Stroke : Strokes)
		{
			Stroke.Channel = PaintedChannel;
		}

		Out.Buffer = TStrongObjectPtr<USurfaceAccumulationBuffer>(NewObject<USurfaceAccumulationBuffer>());

		if (!Out.Buffer->Allocate(World, Out.Ribbon, RTF_RGBA16f))
		{
			Test.AddError(TEXT("Failed to allocate the accumulation buffer."));
			return false;
		}

		Out.Buffer->PaintStrokes(World, Strokes);
		FlushRenderingCommands();

		return true;
	}

	bool ReadGatheredSlot(
		FAutomationTestBase& Test,
		UWorld* World,
		USurfaceAccumulationBuffer& Buffer,
		int32 SlotCount,
		int32 SlotIndex,
		FLinearColor& OutAverage)
	{
		FlushRenderingCommands();

		const TArray<FLinearColor> Gathered = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
			World, Buffer.GetGatherTarget(), 0, 0, SlotCount, 1, false);

		if (Gathered.Num() != SlotCount)
		{
			Test.AddError(FString::Printf(
				TEXT("The gather target read back %d slots, expected %d"), Gathered.Num(), SlotCount));
			return false;
		}

		OutAverage = Gathered[SlotIndex] / RacetrackSurfaceQuery::TotalTapWeight;

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryGatherReturnsThePaintedMark,
	"RacetrackSurface.Query.GatherReturnsThePaintedMark",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryGatherReturnsThePaintedMark::RunTest(const FString& Parameters)
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

	FGatherFixture Fixture;

	if (!BuildFixture(*this, World, EAccumulationChannel::SkidMark, Fixture))
	{
		return false;
	}

	FSurfaceTrackCoordinate OnTheMark;
	OnTheMark.DistanceAlongCentreline = Fixture.Ribbon.TrackLength * GatherPaintedLapFraction;
	OnTheMark.LateralOffsetAlpha = 0.0;

	FSurfaceTrackCoordinate AcrossTheTrack;
	AcrossTheTrack.DistanceAlongCentreline = OnTheMark.DistanceAlongCentreline;
	AcrossTheTrack.LateralOffsetAlpha = 0.9;

	TArray<FSurfaceQueryTap> Taps;
	RacetrackSurfaceQuery::BuildTaps(Fixture.Ribbon, 0, OnTheMark, 0.0, Taps);
	RacetrackSurfaceQuery::BuildTaps(Fixture.Ribbon, 1, AcrossTheTrack, 0.0, Taps);

	Fixture.Buffer->GatherTaps(World, Taps, 2);

	FLinearColor OnTheLine = FLinearColor::Black;
	FLinearColor OffTheLine = FLinearColor::Black;

	if (!ReadGatheredSlot(*this, World, *Fixture.Buffer, 2, 0, OnTheLine)
		|| !ReadGatheredSlot(*this, World, *Fixture.Buffer, 2, 1, OffTheLine))
	{
		Fixture.Buffer->Release();
		return false;
	}

	const FVector2D Atlas = Fixture.Ribbon.RibbonToAtlas(
		Fixture.Ribbon.NormalisedAlongTrack(OnTheMark.DistanceAlongCentreline),
		FSurfaceTrackRibbon::NormalisedAcrossTrack(OnTheMark.LateralOffsetAlpha));

	const int32 Column = FMath::Clamp(
		FMath::FloorToInt32(Atlas.X * Fixture.Ribbon.AtlasWidth()),
		0,
		Fixture.Ribbon.AtlasWidth() - 1);

	const int32 Row = FMath::Clamp(
		FMath::FloorToInt32(Atlas.Y * Fixture.Ribbon.AtlasHeight()),
		0,
		Fixture.Ribbon.AtlasHeight() - 1);

	const TArray<FLinearColor> Direct = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Fixture.Buffer->GetTarget(), Column, Row, 1, 1, false);

	if (Direct.Num() != 1)
	{
		AddError(TEXT("Could not read the atlas texel the gather was aimed at."));
		Fixture.Buffer->Release();
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("Atlas texel %d, %d holds %.6f in the skid channel; the gather at the same coordinate "
			"returned %.6f, and a gather 0.9 across the track returned %.6f"),
		Column,
		Row,
		Direct[0].G,
		OnTheLine.G,
		OffTheLine.G));

	TestTrue(
		TEXT("The painted mark is actually in the atlas, so the comparison means something"),
		Direct[0].G > 0.0f);

	TestTrue(
		FString::Printf(
			TEXT("The gather returns what is stored at the coordinate it was aimed at, %.6f against %.6f"),
			OnTheLine.G,
			Direct[0].G),
		FMath::Abs(OnTheLine.G - Direct[0].G) <= FMath::Max(Direct[0].G * 0.02f, 1.0e-4f));

	TestTrue(
		FString::Printf(
			TEXT("A gather aimed across the track from the mark reads far less, %.6f against %.6f"),
			OffTheLine.G,
			OnTheLine.G),
		OffTheLine.G < OnTheLine.G * 0.05f);

	Fixture.Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryGatherIsolatesChannels,
	"RacetrackSurface.Query.GatherIsolatesChannels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryGatherIsolatesChannels::RunTest(const FString& Parameters)
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

	FGatherFixture Fixture;

	if (!BuildFixture(*this, World, EAccumulationChannel::Marbles, Fixture))
	{
		return false;
	}

	FSurfaceTrackCoordinate OnTheMark;
	OnTheMark.DistanceAlongCentreline = Fixture.Ribbon.TrackLength * GatherPaintedLapFraction;
	OnTheMark.LateralOffsetAlpha = 0.0;

	TArray<FSurfaceQueryTap> Taps;
	RacetrackSurfaceQuery::BuildTaps(Fixture.Ribbon, 0, OnTheMark, 0.0, Taps);

	Fixture.Buffer->GatherTaps(World, Taps, 1);

	FLinearColor Average = FLinearColor::Black;

	if (!ReadGatheredSlot(*this, World, *Fixture.Buffer, 1, 0, Average))
	{
		Fixture.Buffer->Release();
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("A marbles-only mark gathers as rubber %.6f, skid %.6f, marbles %.6f"),
		Average.R,
		Average.G,
		Average.B));

	TestTrue(TEXT("The marbles channel carries the mark"), Average.B > 0.0f);
	TestEqual(TEXT("The rubber channel stays clean"), Average.R, 0.0f);
	TestEqual(TEXT("The skid channel stays clean"), Average.G, 0.0f);

	Fixture.Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryGatherReadsZeroOnCleanTarmac,
	"RacetrackSurface.Query.GatherReadsZeroOnCleanTarmac",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryGatherReadsZeroOnCleanTarmac::RunTest(const FString& Parameters)
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

	const FSurfaceTrackRibbon Ribbon = MakeGatherRibbon();

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate the accumulation buffer."));
		return false;
	}

	Buffer->Clear(World);
	FlushRenderingCommands();

	FSurfaceTrackCoordinate Anywhere;
	Anywhere.DistanceAlongCentreline = Ribbon.TrackLength * 0.5;
	Anywhere.LateralOffsetAlpha = 0.0;

	TArray<FSurfaceQueryTap> Taps;
	RacetrackSurfaceQuery::BuildTaps(Ribbon, 0, Anywhere, RacetrackSurfaceQuery::DefaultContactPatchWidth, Taps);

	Buffer->GatherTaps(World, Taps, 1);

	FLinearColor Average = FLinearColor::Black;

	if (!ReadGatheredSlot(*this, World, *Buffer, 1, 0, Average))
	{
		Buffer->Release();
		return false;
	}

	TestEqual(TEXT("Clean tarmac gathers as zero rubber"), Average.R, 0.0f);
	TestEqual(TEXT("Clean tarmac gathers as zero skid"), Average.G, 0.0f);
	TestEqual(TEXT("Clean tarmac gathers as zero marbles"), Average.B, 0.0f);

	Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryGatherTargetIsLinear,
	"RacetrackSurface.Query.GatherTargetIsLinear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryGatherTargetIsLinear::RunTest(const FString& Parameters)
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

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	UTextureRenderTarget2D* Gather = Buffer->EnsureGatherTarget(World, 64);

	if (Gather == nullptr)
	{
		AddError(TEXT("The gather target could not be allocated."));
		return false;
	}

	TestEqual(TEXT("The gather target is one row of slots"), Gather->SizeY, 1);
	TestEqual(TEXT("The gather target holds every requested slot"), Gather->SizeX, 64);

	TestEqual(
		TEXT("The gather target is float, so the canvas writes exposure linearly rather than "
			"baking a 2.2 curve into it"),
		Gather->GetDisplayGamma(),
		1.0f);

	TestEqual(
		TEXT("A wider gather reuses the same target rather than growing it every frame"),
		Buffer->EnsureGatherTarget(World, 16),
		Gather);

	Buffer->Release();

	return true;
}

#endif
