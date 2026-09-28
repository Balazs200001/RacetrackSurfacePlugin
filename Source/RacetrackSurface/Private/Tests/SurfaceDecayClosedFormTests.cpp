#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeSquareTestRibbon;

namespace RacetrackSurfaceClosedFormTest
{
	constexpr int32 Resolution = 256;

	constexpr float FloatRelativeTolerance = 1.0e-5f;

	constexpr float FloatAbsoluteTolerance = 1.0e-9f;

	constexpr double HalfLifeDriftTolerance = 1.0e-4;

	FSurfaceBrushStroke MakeStroke(
		const FVector2D& Start,
		const FVector2D& End,
		float WidthInTexels,
		float Intensity,
		EAccumulationChannel Channel)
	{
		FSurfaceBrushStroke Stroke;
		Stroke.RegionStart = Start;
		Stroke.RegionEnd = End;
		Stroke.WidthInTexels = WidthInTexels;
		Stroke.Intensity = Intensity;
		Stroke.Channel = Channel;
		return Stroke;
	}

	TArray<FSurfaceBrushStroke> MakeCrossedChannels()
	{
		TArray<FSurfaceBrushStroke> Strokes;

		const float RubberIntensities[] = { 0.1f, 0.6f, 2.0f, 6.0f };

		for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(RubberIntensities)); ++Index)
		{
			const double AcrossTrack = 0.17 + 0.21 * Index;

			Strokes.Add(MakeStroke(
				FVector2D(0.05, AcrossTrack),
				FVector2D(0.95, AcrossTrack),
				40.0f,
				RubberIntensities[Index],
				EAccumulationChannel::RubberBuildup));
		}

		const double SkidColumns[] = { 0.23, 0.49, 0.77 };

		for (const double AlongTrack : SkidColumns)
		{
			Strokes.Add(MakeStroke(
				FVector2D(AlongTrack, 0.05),
				FVector2D(AlongTrack, 0.95),
				36.0f,
				1.5f,
				EAccumulationChannel::SkidMark));
		}

		Strokes.Add(MakeStroke(
			FVector2D(0.1, 0.1),
			FVector2D(0.9, 0.9),
			48.0f,
			0.8f,
			EAccumulationChannel::Marbles));

		return Strokes;
	}

	TArray<FLinearColor> ReadWholeTarget(UObject* WorldContext, UTextureRenderTarget2D* Target)
	{
		return UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
			WorldContext, Target, 0, 0, Resolution, Resolution, false);
	}

	bool Agrees(float Measured, float Expected)
	{
		return FMath::Abs(Measured - Expected)
			<= FMath::Max(FMath::Abs(Expected) * FloatRelativeTolerance, FloatAbsoluteTolerance);
	}

	struct FRepeatedPassDrift
	{
		int32 ValuesCompared = 0;
		double MeanRelativeDrift = 0.0;
		double WorstRelativeDrift = 0.0;
	};

	FRepeatedPassDrift MeasureDriftOverHalfLife(const TArray<FLinearColor>& Before, const TArray<FLinearColor>& After)
	{
		FRepeatedPassDrift Drift;
		double Sum = 0.0;

		for (int32 Index = 0; Index < Before.Num() && Index < After.Num(); ++Index)
		{
			const float Channels[2][2] = { { Before[Index].R, After[Index].R }, { Before[Index].B, After[Index].B } };

			for (const auto& Pair : Channels)
			{
				if (Pair[0] < 0.01f)
				{
					continue;
				}

				const double Relative = Pair[1] / (Pair[0] * 0.5) - 1.0;

				Sum += Relative;
				Drift.WorstRelativeDrift = FMath::Max(Drift.WorstRelativeDrift, FMath::Abs(Relative));
				++Drift.ValuesCompared;
			}
		}

		Drift.MeanRelativeDrift = Drift.ValuesCompared > 0 ? Sum / Drift.ValuesCompared : 0.0;

		return Drift;
	}

	struct FPassComparison
	{
		int32 TexelsCompared = 0;
		int32 TexelsWithRubberAndSkid = 0;
		int32 Disagreements = 0;
		float LargestMultiplier = 1.0f;
		float WorstRelativeError = 0.0f;
		FString FirstDisagreement;
	};

	FPassComparison CompareAgainstClosedForm(
		const TArray<FLinearColor>& Before,
		const TArray<FLinearColor>& After,
		float HalfLivesElapsed)
	{
		FPassComparison Comparison;

		const float SharedRetention = RacetrackSurfaceMapping::DecayRetentionForProgress(HalfLivesElapsed);

		for (int32 Index = 0; Index < Before.Num() && Index < After.Num(); ++Index)
		{
			const FLinearColor& From = Before[Index];
			const FLinearColor& To = After[Index];

			const float SkidRetention = RacetrackSurfaceMapping::SkidRetentionForProgress(HalfLivesElapsed, From.R);

			const FLinearColor Expected(
				From.R * SharedRetention,
				From.G * SkidRetention,
				From.B * SharedRetention,
				From.A);

			++Comparison.TexelsCompared;

			if (From.R > 0.0f && From.G > 0.0f)
			{
				++Comparison.TexelsWithRubberAndSkid;
				Comparison.LargestMultiplier = FMath::Max(
					Comparison.LargestMultiplier,
					RacetrackSurfaceMapping::SkidDecayRateMultiplier(From.R));
			}

			const float Channels[4][2] = {
				{ To.R, Expected.R },
				{ To.G, Expected.G },
				{ To.B, Expected.B },
				{ To.A, Expected.A } };

			bool bTexelAgrees = true;

			for (const auto& Pair : Channels)
			{
				if (Pair[1] > FloatAbsoluteTolerance)
				{
					Comparison.WorstRelativeError = FMath::Max(
						Comparison.WorstRelativeError,
						FMath::Abs(Pair[0] - Pair[1]) / Pair[1]);
				}

				bTexelAgrees &= Agrees(Pair[0], Pair[1]);
			}

			if (!bTexelAgrees)
			{
				if (Comparison.Disagreements == 0)
				{
					Comparison.FirstDisagreement = FString::Printf(
						TEXT("texel (%d, %d) went from %s to %s against an expected %s"),
						Index % Resolution,
						Index / Resolution,
						*From.ToString(),
						*To.ToString(),
						*Expected.ToString());
				}

				++Comparison.Disagreements;
			}
		}

		return Comparison;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceCoupledPassMatchesTheClosedFormAtEveryTexel,
	"RacetrackSurface.Decay.CoupledPassMatchesTheClosedFormAtEveryTexel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceCoupledPassMatchesTheClosedFormAtEveryTexel::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceClosedFormTest;

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

	if (!Buffer->Allocate(World, MakeSquareTestRibbon(Resolution)) || !Buffer->CanDecayInPlace())
	{
		AddError(TEXT("Failed to allocate an accumulation buffer that can decay in place."));
		return false;
	}

	Buffer->Clear(World);
	Buffer->PaintStrokes(World, MakeCrossedChannels());
	FlushRenderingCommands();

	const float Steps[] = { 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife, 0.25f };

	for (const float HalfLivesElapsed : Steps)
	{
		const TArray<FLinearColor> Before = ReadWholeTarget(World, Buffer->GetTarget());

		if (!Buffer->ApplyDecay(World, HalfLivesElapsed))
		{
			AddError(TEXT("The decay pass was not issued."));
			Buffer->Release();
			return false;
		}

		FlushRenderingCommands();

		const TArray<FLinearColor> After = ReadWholeTarget(World, Buffer->GetTarget());

		if (Before.Num() != Resolution * Resolution || After.Num() != Resolution * Resolution)
		{
			AddError(TEXT("Readback did not return the whole target."));
			Buffer->Release();
			return false;
		}

		const FPassComparison Comparison = CompareAgainstClosedForm(Before, After, HalfLivesElapsed);

		AddInfo(FString::Printf(
			TEXT("One pass of %.6f half-lives: %d texels compared, %d carrying both rubber and skid, skid "
				"decaying up to %.2f times the shared rate. %d disagree with the C++ closed form beyond "
				"%.0e relative; the worst relative error on any channel is %.3e"),
			HalfLivesElapsed,
			Comparison.TexelsCompared,
			Comparison.TexelsWithRubberAndSkid,
			Comparison.LargestMultiplier,
			Comparison.Disagreements,
			FloatRelativeTolerance,
			Comparison.WorstRelativeError));

		if (Comparison.Disagreements > 0)
		{
			AddInfo(FString::Printf(TEXT("First disagreement: %s"), *Comparison.FirstDisagreement));
		}

		TestTrue(
			*FString::Printf(
				TEXT("At %.6f half-lives the fixture exercises the coupling on many texels, not a handful"),
				HalfLivesElapsed),
			Comparison.TexelsWithRubberAndSkid > 1000);

		TestTrue(
			*FString::Printf(
				TEXT("At %.6f half-lives the rubber on the fixture reaches exposures where the coupling is "
					"strong"),
				HalfLivesElapsed),
			Comparison.LargestMultiplier > 4.0f);

		TestEqual(
			*FString::Printf(
				TEXT("At %.6f half-lives every texel's rubber, skid, marbles and alpha match the C++ closed "
					"form to float precision, so the shader and SkidRetentionForProgress cannot silently "
					"disagree, and nothing in the pass rounds the stored value to a coarser format"),
				HalfLivesElapsed),
			Comparison.Disagreements,
			0);
	}

	const float StepHalfLives = 1.0f / RacetrackSurfaceMapping::DecayStepsPerHalfLife;
	const int32 StepsInAHalfLife = FMath::RoundToInt32(RacetrackSurfaceMapping::DecayStepsPerHalfLife);

	Buffer->Clear(World);
	Buffer->PaintStrokes(World, MakeCrossedChannels());
	FlushRenderingCommands();

	const TArray<FLinearColor> BeforeHalfLife = ReadWholeTarget(World, Buffer->GetTarget());

	for (int32 Step = 0; Step < StepsInAHalfLife; ++Step)
	{
		Buffer->ApplyDecay(World, StepHalfLives);
	}

	FlushRenderingCommands();

	const FRepeatedPassDrift CoupledDrift =
		MeasureDriftOverHalfLife(BeforeHalfLife, ReadWholeTarget(World, Buffer->GetTarget()));

	AddInfo(FString::Printf(
		TEXT("%d coupled passes of one step each, over %d rubber and marble values: they kept %.4f %% more than "
			"half on average, worst %.4f %% either way"),
		StepsInAHalfLife,
		CoupledDrift.ValuesCompared,
		CoupledDrift.MeanRelativeDrift * 100.0,
		CoupledDrift.WorstRelativeDrift * 100.0));

	TestTrue(
		*FString::Printf(
			TEXT("A half-life of coupled passes leaves half of every rubber and marble value, within %.2f %% on "
				"average, so the calibrated half-life is the one the buffer actually decays at"),
			HalfLifeDriftTolerance * 100.0),
		CoupledDrift.ValuesCompared > 1000 && FMath::Abs(CoupledDrift.MeanRelativeDrift) < HalfLifeDriftTolerance);

	TStrongObjectPtr<USurfaceAccumulationBuffer> CanvasBuffer(NewObject<USurfaceAccumulationBuffer>());

	if (CanvasBuffer->Allocate(World, MakeSquareTestRibbon(Resolution)))
	{
		const float Retention = RacetrackSurfaceMapping::DecayRetentionForProgress(StepHalfLives);

		CanvasBuffer->Clear(World);
		CanvasBuffer->PaintStrokes(World, MakeCrossedChannels());
		FlushRenderingCommands();

		const TArray<FLinearColor> Before = ReadWholeTarget(World, CanvasBuffer->GetTarget());

		for (int32 Step = 0; Step < StepsInAHalfLife; ++Step)
		{
			RacetrackSurfaceTest::DrawUniformDecayQuadAsShippedInC10(World, CanvasBuffer->GetTarget(), Retention);
		}

		FlushRenderingCommands();

		const FRepeatedPassDrift CanvasDrift =
			MeasureDriftOverHalfLife(Before, ReadWholeTarget(World, CanvasBuffer->GetTarget()));

		AddInfo(FString::Printf(
			TEXT("%d C10 quads of one step each on the same fixture, for comparison: they kept %.4f %% more than "
				"half on average, worst %.4f %% either way, over %d values"),
			StepsInAHalfLife,
			CanvasDrift.MeanRelativeDrift * 100.0,
			CanvasDrift.WorstRelativeDrift * 100.0,
			CanvasDrift.ValuesCompared));

		CanvasBuffer->Release();
	}

	Buffer->Release();

	return true;
}

#endif
