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

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeTestCircuit;
using RacetrackSurfaceTest::RaceSpeedCentimetresPerSecond;

namespace
{
	constexpr float FoldFrameSeconds = 1.0f / 30.0f;

	FSurfaceTrackRibbon MakeTwoRowRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 62500.0;
		Ribbon.TrackWidth = 100.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackContact MakeFoldedContact(double Distance)
	{
		FSurfaceTrackContact Contact;
		Contact.Coordinate.DistanceAlongCentreline = Distance;
		Contact.Coordinate.LateralOffsetAlpha = 0.0;
		Contact.CentimetresPerSecondAlongCentreline = RaceSpeedCentimetresPerSecond;
		Contact.SlipAmount = 1.0f;
		Contact.ContactWidth = 20.0f;
		Contact.Channel = EAccumulationChannel::SkidMark;
		return Contact;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceFoldedRibbonPaintsIntoTheRowItBelongsTo,
	"RacetrackSurface.Folded.PaintsIntoTheRowItBelongsTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceFoldedRibbonPaintsIntoTheRowItBelongsTo::RunTest(const FString& Parameters)
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

	const FSurfaceTrackRibbon Ribbon = MakeTwoRowRibbon();

	TestEqual(TEXT("The test ribbon folds into two rows"), Ribbon.RowCount(), 2);
	TestEqual(TEXT("Each row is 10000 texels wide"), Ribbon.AtlasWidth(), 10000);
	TestEqual(TEXT("The atlas stacks two bands of 32"), Ribbon.AtlasHeight(), 64);

	TStrongObjectPtr<USplineComponent> Circuit = MakeTestCircuit();
	TStrongObjectPtr<USurfaceAccumulationSubsystem> Subsystem(NewObject<USurfaceAccumulationSubsystem>());
	Subsystem->ConfigureTrackRibbon(Circuit.Get(), Ribbon);

	const double ThreeQuartersOfTheLap = Ribbon.TrackLength * 0.75;

	TArray<FSurfaceBrushStroke> Strokes;
	Subsystem->BuildStrokes(MakeFoldedContact(ThreeQuartersOfTheLap), FoldFrameSeconds, Strokes);

	TestEqual(TEXT("A contact well inside the second row paints one stroke"), Strokes.Num(), 1);

	if (Strokes.Num() != 1)
	{
		return false;
	}

	TestEqual(TEXT("Three quarters of the lap belongs to the second row"), Strokes[0].RowIndex, 1);

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate the folded accumulation buffer."));
		return false;
	}

	TestEqual(TEXT("The buffer remembers how many rows it was folded into"), Buffer->GetAllocatedRowCount(), 2);

	Buffer->PaintStrokes(World, Strokes);
	FlushRenderingCommands();

	const int32 Column = FMath::RoundToInt32(Strokes[0].RegionEnd.X * Ribbon.AtlasWidth()) - 1;

	const TArray<FLinearColor> Band = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Buffer->GetTarget(), Column, 0, 1, Ribbon.AtlasHeight(), false);

	if (Band.Num() != Ribbon.AtlasHeight())
	{
		AddError(FString::Printf(
			TEXT("Readback returned %d texels, expected %d"), Band.Num(), Ribbon.AtlasHeight()));
		Buffer->Release();
		return false;
	}

	const int32 RowHeight = Ribbon.AtlasHeight() / Ribbon.RowCount();

	float FirstRowInk = 0.0f;
	float SecondRowInk = 0.0f;
	double WeightedTexel = 0.0;
	double TotalInk = 0.0;

	for (int32 Texel = 0; Texel < Band.Num(); ++Texel)
	{
		const float Ink = Band[Texel].G;

		WeightedTexel += (Texel + 0.5) * Ink;
		TotalInk += Ink;

		if (Texel < RowHeight)
		{
			FirstRowInk = FMath::Max(FirstRowInk, Ink);
		}
		else
		{
			SecondRowInk = FMath::Max(SecondRowInk, Ink);
		}
	}

	if (TotalInk <= 0.0)
	{
		AddError(TEXT("The column carries no ink at all, so the fold could not be checked."));
		Buffer->Release();
		return false;
	}

	const double InkCentre = WeightedTexel / TotalInk;

	AddInfo(FString::Printf(
		TEXT("Column %d of a %d x %d atlas: first row peaks at %.6f, second row peaks at %.6f, the ink centres "
			"on texel %.2f"),
		Column,
		Ribbon.AtlasWidth(),
		Ribbon.AtlasHeight(),
		FirstRowInk,
		SecondRowInk,
		InkCentre));

	TestTrue(
		FString::Printf(TEXT("The second row carries the mark, peaking at %.6f"), SecondRowInk),
		SecondRowInk > 0.0f);

	TestTrue(
		FString::Printf(
			TEXT("The first row is untouched at the same column, peaking at %.6f against the second row's %.6f"),
			FirstRowInk,
			SecondRowInk),
		FirstRowInk <= SecondRowInk * 0.01f);

	const double ExpectedCentre = Strokes[0].RegionEnd.Y * Ribbon.AtlasHeight();

	TestTrue(
		FString::Printf(
			TEXT("The mark centres where the fold predicted, on texel %.2f against %.2f"),
			InkCentre,
			ExpectedCentre),
		FMath::Abs(InkCentre - ExpectedCentre) <= 1.0);

	Buffer->Release();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceFoldedRibbonKeepsTheTrackEdgeOutOfTheNextRow,
	"RacetrackSurface.Folded.KeepsTheTrackEdgeOutOfTheNextRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceFoldedRibbonKeepsTheTrackEdgeOutOfTheNextRow::RunTest(const FString& Parameters)
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

	const FSurfaceTrackRibbon Ribbon = MakeTwoRowRibbon();

	TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

	if (!Buffer->Allocate(World, Ribbon, RTF_RGBA16f))
	{
		AddError(TEXT("Failed to allocate the folded accumulation buffer."));
		return false;
	}

	FSurfaceBrushStroke Stroke;
	Stroke.RegionStart = Ribbon.RibbonToAtlasInRow(0, 0.2, 1.0);
	Stroke.RegionEnd = Ribbon.RibbonToAtlasInRow(0, 0.2 + 1.0 / Ribbon.AtlasWidth(), 1.0);
	Stroke.WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(60.0));
	Stroke.Intensity = 1.0f;
	Stroke.RowIndex = 0;
	Stroke.Channel = EAccumulationChannel::SkidMark;

	Buffer->PaintStrokes(World, TArray<FSurfaceBrushStroke>({ Stroke }));
	FlushRenderingCommands();

	const int32 Column = FMath::Clamp(
		FMath::RoundToInt32(Stroke.RegionStart.X * Ribbon.AtlasWidth()), 0, Ribbon.AtlasWidth() - 1);

	const TArray<FLinearColor> Band = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
		World, Buffer->GetTarget(), Column, 0, 1, Ribbon.AtlasHeight(), false);

	if (Band.Num() != Ribbon.AtlasHeight())
	{
		AddError(FString::Printf(
			TEXT("Readback returned %d texels, expected %d"), Band.Num(), Ribbon.AtlasHeight()));
		Buffer->Release();
		return false;
	}

	const int32 RowHeight = Ribbon.AtlasHeight() / Ribbon.RowCount();

	float OwnRowInk = 0.0f;
	float NextRowInk = 0.0f;

	for (int32 Texel = 0; Texel < Band.Num(); ++Texel)
	{
		if (Texel < RowHeight)
		{
			OwnRowInk = FMath::Max(OwnRowInk, Band[Texel].G);
		}
		else
		{
			NextRowInk = FMath::Max(NextRowInk, Band[Texel].G);
		}
	}

	AddInfo(FString::Printf(
		TEXT("A 60 cm brush centred on the track edge of row 0: row 0 peaks at %.6f, row 1 peaks at %.6f"),
		OwnRowInk,
		NextRowInk));

	TestTrue(
		FString::Printf(TEXT("The mark lands in its own row, peaking at %.6f"), OwnRowInk),
		OwnRowInk > 0.0f);

	TestTrue(
		FString::Printf(
			TEXT("The half of the brush hanging past the track edge is clipped rather than landing in the next "
				"row, which peaks at %.6f against %.6f"),
			NextRowInk,
			OwnRowInk),
		NextRowInk <= OwnRowInk * 0.01f);

	Buffer->Release();

	return true;
}

#endif
