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
	constexpr int32 BufferResolution = 512;
	constexpr int32 ProfileTexels = 25;
	constexpr double ContactWidthCentimetres = 20.0;
	constexpr float InkThresholdFraction = 0.01f;

	FSurfaceTrackRibbon MakeRibbonAtDensity(double CentimetresPerTexel)
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.CentimetresPerTexel = CentimetresPerTexel;
		Ribbon.TrackLength = BufferResolution * CentimetresPerTexel;
		Ribbon.TrackWidth = BufferResolution * CentimetresPerTexel;
		return Ribbon;
	}

	TArray<FSurfaceBrushStroke> MakeCentredHorizontalStroke(const FSurfaceTrackRibbon& Ribbon)
	{
		FSurfaceBrushStroke Stroke;
		Stroke.RegionStart = FVector2D(0.4, 0.5);
		Stroke.RegionEnd = FVector2D(0.6, 0.5);
		Stroke.WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(ContactWidthCentimetres));
		Stroke.Intensity = 1.0f;
		Stroke.Channel = EAccumulationChannel::SkidMark;

		return TArray<FSurfaceBrushStroke>({ Stroke });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfacePaintedStrokeWidth,
	"RacetrackSurface.Accumulator.PaintedStrokeWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfacePaintedStrokeWidth::RunTest(const FString& Parameters)
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

	AddInfo(TEXT("cm/texel | quad texels | peak | inked texels | inked cm | half-max texels | half-max cm"));

	for (const double CentimetresPerTexel : { 9.765625, 2.44140625 })
	{
		const FSurfaceTrackRibbon Ribbon = MakeRibbonAtDensity(CentimetresPerTexel);

		TStrongObjectPtr<USurfaceAccumulationBuffer> Buffer(NewObject<USurfaceAccumulationBuffer>());

		if (!Buffer->Allocate(World, Ribbon, RTF_RGBA16f))
		{
			AddError(FString::Printf(TEXT("Failed to allocate at %.6f cm per texel"), CentimetresPerTexel));
			continue;
		}

		const TArray<FSurfaceBrushStroke> Strokes = MakeCentredHorizontalStroke(Ribbon);
		const float QuadTexels = Strokes[0].WidthInTexels;

		Buffer->PaintStrokes(World, Strokes);
		FlushRenderingCommands();

		const int32 CentreX = BufferResolution / 2;
		const int32 FirstY = BufferResolution / 2 - ProfileTexels / 2;

		const TArray<FLinearColor> Profile = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
			World, Buffer->GetTarget(), CentreX, FirstY, 1, ProfileTexels, false);

		if (Profile.Num() != ProfileTexels)
		{
			AddError(FString::Printf(
				TEXT("Readback returned %d texels, expected %d"), Profile.Num(), ProfileTexels));
			Buffer->Release();
			continue;
		}

		float Peak = 0.0f;
		for (const FLinearColor& Sample : Profile)
		{
			Peak = FMath::Max(Peak, Sample.G);
		}

		int32 InkedTexels = 0;
		int32 HalfMaximumTexels = 0;
		FString ProfileText;

		for (int32 Index = 0; Index < Profile.Num(); ++Index)
		{
			const float Value = Profile[Index].G;

			if (Value > Peak * InkThresholdFraction)
			{
				++InkedTexels;
			}

			if (Value >= Peak * 0.5f)
			{
				++HalfMaximumTexels;
			}

			if (Value > 0.0f)
			{
				ProfileText += FString::Printf(TEXT("[%d]=%.4f "), FirstY + Index, Value);
			}
		}

		AddInfo(FString::Printf(
			TEXT("%.6f | %.3f | %.4f | %d | %.2f | %d | %.2f"),
			CentimetresPerTexel,
			QuadTexels,
			Peak,
			InkedTexels,
			InkedTexels * CentimetresPerTexel,
			HalfMaximumTexels,
			HalfMaximumTexels * CentimetresPerTexel));

		AddInfo(FString::Printf(TEXT("  profile: %s"), *ProfileText));

		TestTrue(
			FString::Printf(TEXT("The stroke actually painted something at %.6f cm per texel"), CentimetresPerTexel),
			Peak > 0.0f);

		TestTrue(
			FString::Printf(
				TEXT("The mark is no wider than its quad plus a texel of filtering either side: ")
				TEXT("%d inked against %.3f quad texels at %.6f cm per texel"),
				InkedTexels, QuadTexels, CentimetresPerTexel),
			static_cast<double>(InkedTexels) <= QuadTexels + 2.0);

		TestTrue(
			FString::Printf(
				TEXT("The mark measures close to the %.0f cm contact patch it was asked for, got %.2f cm"),
				ContactWidthCentimetres, InkedTexels * CentimetresPerTexel),
			InkedTexels * CentimetresPerTexel <= ContactWidthCentimetres + 2.0 * CentimetresPerTexel);

		Buffer->Release();
	}

	return true;
}

#endif
