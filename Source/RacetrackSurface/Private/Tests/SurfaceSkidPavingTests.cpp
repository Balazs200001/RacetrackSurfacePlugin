// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeSquareTestRibbon;

namespace RacetrackSurfacePavingTest
{
	constexpr int32 Resolution = 256;

	constexpr int32 MarkRow = 100;

	constexpr int32 OnTheLineColumn = 64;

	constexpr int32 OffTheLineColumn = 192;

	constexpr double MarkLengthInTexels = 16.0;

	constexpr float MarkWidthInTexels = 8.0f;

	constexpr double TrafficLengthInTexels = 48.0;

	constexpr float TrafficWidthInTexels = 48.0f;

	constexpr float CalibratedApexExposure = 1.0f;

	constexpr float RelativeTolerance = 0.03f;

	constexpr float RubberHoldTolerance = 0.01f;

	const int32 CheckpointPasses[] = { 64, 128, 256 };

	class FScopedPavingDecayRate
	{
	public:
		explicit FScopedPavingDecayRate(float Rate)
			: Variable(IConsoleManager::Get().FindConsoleVariable(RacetrackSurfaceMapping::DecayRateVariableName))
			, RateToRestore(Variable != nullptr ? Variable->GetFloat() : 0.0f)
		{
			if (Variable != nullptr)
			{
				Variable->Set(Rate);
			}
		}

		~FScopedPavingDecayRate()
		{
			if (Variable != nullptr)
			{
				Variable->Set(RateToRestore);
			}
		}

		bool IsBound() const
		{
			return Variable != nullptr;
		}

	private:
		IConsoleVariable* Variable;
		float RateToRestore;
	};

	double TexelCentre(int32 Texel)
	{
		return (Texel + 0.5) / Resolution;
	}

	FSurfaceBrushStroke MakeStrokeCentredOn(
		int32 Column,
		double LengthInTexels,
		float WidthInTexels,
		float Intensity,
		EAccumulationChannel Channel)
	{
		const double HalfLength = LengthInTexels * 0.5 / Resolution;

		FSurfaceBrushStroke Stroke;
		Stroke.RegionStart = FVector2D(TexelCentre(Column) - HalfLength, TexelCentre(MarkRow));
		Stroke.RegionEnd = FVector2D(TexelCentre(Column) + HalfLength, TexelCentre(MarkRow));
		Stroke.WidthInTexels = WidthInTexels;
		Stroke.Intensity = Intensity;
		Stroke.Channel = Channel;
		return Stroke;
	}

	float SweptShare(double LengthInTexels, float WidthInTexels)
	{
		return static_cast<float>(LengthInTexels / (LengthInTexels + WidthInTexels));
	}

	FSurfaceBrushStroke MakeTrafficOverTheLine(float CentreExposure)
	{
		return MakeStrokeCentredOn(
			OnTheLineColumn,
			TrafficLengthInTexels,
			TrafficWidthInTexels,
			CentreExposure / SweptShare(TrafficLengthInTexels, TrafficWidthInTexels),
			EAccumulationChannel::RubberBuildup);
	}

	FLinearColor ReadTexel(UObject* WorldContext, UTextureRenderTarget2D* Target, int32 Column)
	{
		const TArray<FLinearColor> Texels = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
			WorldContext, Target, Column, MarkRow, 1, 1, false);

		return Texels.Num() == 1 ? Texels[0] : FLinearColor(-1.0f, -1.0f, -1.0f, -1.0f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceSkidOnTheLinePavesOverAndSkidOffItPersists,
	"RacetrackSurface.Decay.SkidOnTheLinePavesOverAndSkidOffItPersists",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceSkidOnTheLinePavesOverAndSkidOffItPersists::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfacePavingTest;

	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = FindRenderableWorld();

	USurfaceAccumulationSubsystem* Subsystem =
		World != nullptr ? World->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

	if (Subsystem == nullptr)
	{
		AddError(TEXT("No world or subsystem available."));
		return false;
	}

	Subsystem->ConfigureTrackRibbon(nullptr, MakeSquareTestRibbon(Resolution));
	Subsystem->ResetSurface();

	UTextureRenderTarget2D* Target = Subsystem->GetOrCreateAccumulationTarget();
	USurfaceAccumulationBuffer* Buffer = Subsystem->GetBuffer();

	if (Target == nullptr || Buffer == nullptr || !Buffer->CanDecayInPlace())
	{
		AddError(TEXT("No accumulation target that can decay in place."));
		return false;
	}

	const FScopedPavingDecayRate ScopedRate(1.0f);

	if (!ScopedRate.IsBound())
	{
		AddError(TEXT("The decay rate console variable is not registered."));
		return false;
	}

	const float StepsPerHalfLife = USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife();
	const float HalfLifeSeconds = Subsystem->GetDecayHalfLifeSeconds();
	const float TickSeconds = HalfLifeSeconds / StepsPerHalfLife;
	const float StepHalfLives = RacetrackSurfaceMapping::ElapsedToDecayProgress(TickSeconds, 1.0f, HalfLifeSeconds);
	const float TrafficPerPass =
		CalibratedApexExposure * (1.0f - RacetrackSurfaceMapping::DecayRetentionForProgress(StepHalfLives));

	Buffer->PaintStrokes(World, { MakeTrafficOverTheLine(CalibratedApexExposure) });

	Buffer->PaintStrokes(World, {
		MakeStrokeCentredOn(
			OnTheLineColumn, MarkLengthInTexels, MarkWidthInTexels, 1.0f, EAccumulationChannel::SkidMark),
		MakeStrokeCentredOn(
			OffTheLineColumn, MarkLengthInTexels, MarkWidthInTexels, 1.0f, EAccumulationChannel::SkidMark) });

	FlushRenderingCommands();

	const FLinearColor OnTheLineLaid = ReadTexel(World, Target, OnTheLineColumn);
	const FLinearColor OffTheLineLaid = ReadTexel(World, Target, OffTheLineColumn);

	AddInfo(FString::Printf(
		TEXT("Laid: the mark under traffic holds skid %.6f on rubber %.6f, the bare mark holds skid %.6f on "
			"rubber %.6f. Each pass is %.6f half-lives, and traffic lays %.6f over the line between passes"),
		OnTheLineLaid.G,
		OnTheLineLaid.R,
		OffTheLineLaid.G,
		OffTheLineLaid.R,
		StepHalfLives,
		TrafficPerPass));

	TestTrue(TEXT("Both marks were laid"), OnTheLineLaid.G > 0.1f && OffTheLineLaid.G > 0.1f);

	TestEqual(
		TEXT("The two marks start identical, so any difference later is the surface's doing"),
		OnTheLineLaid.G,
		OffTheLineLaid.G);

	TestTrue(
		TEXT("The line starts at the calibrated apex exposure"),
		FMath::IsNearlyEqual(OnTheLineLaid.R, CalibratedApexExposure, CalibratedApexExposure * RubberHoldTolerance));

	TestEqual(TEXT("The bare mark has no rubber under it"), OffTheLineLaid.R, 0.0f);

	const int32 PassesBefore = Subsystem->GetDecayPassCount();
	int32 PassesRun = 0;
	int32 NextCheckpoint = 0;

	AddInfo(TEXT("Passes | half-lives | rubber under the line | on the line kept, predicted | off the line kept, "
		"predicted | ratio"));

	while (NextCheckpoint < static_cast<int32>(UE_ARRAY_COUNT(CheckpointPasses)))
	{
		Subsystem->Tick(TickSeconds);
		Buffer->PaintStrokes(World, { MakeTrafficOverTheLine(TrafficPerPass) });
		++PassesRun;

		if (PassesRun != CheckpointPasses[NextCheckpoint])
		{
			continue;
		}

		++NextCheckpoint;

		FlushRenderingCommands();

		const FLinearColor OnTheLine = ReadTexel(World, Target, OnTheLineColumn);
		const FLinearColor OffTheLine = ReadTexel(World, Target, OffTheLineColumn);

		const int32 PassesApplied = Subsystem->GetDecayPassCount() - PassesBefore;
		const float HalfLives = PassesApplied * StepHalfLives;

		const float OnTheLineKept = OnTheLine.G / OnTheLineLaid.G;
		const float OffTheLineKept = OffTheLine.G / OffTheLineLaid.G;

		const float OnTheLinePredicted =
			RacetrackSurfaceMapping::SkidRetentionForProgress(HalfLives, CalibratedApexExposure);
		const float OffTheLinePredicted = RacetrackSurfaceMapping::SkidRetentionForProgress(HalfLives, 0.0f);

		AddInfo(FString::Printf(
			TEXT("%d | %.4f | %.6f | %.4f, %.4f | %.4f, %.4f | %.3f"),
			PassesApplied,
			HalfLives,
			OnTheLine.R,
			OnTheLineKept,
			OnTheLinePredicted,
			OffTheLineKept,
			OffTheLinePredicted,
			OffTheLineKept / FMath::Max(OnTheLineKept, UE_SMALL_NUMBER)));

		TestEqual(
			*FString::Printf(TEXT("Every tick of %d fired exactly one decay pass"), PassesRun),
			PassesApplied,
			PassesRun);

		TestTrue(
			*FString::Printf(
				TEXT("After %d passes the traffic still holds the line at the calibrated apex exposure"),
				PassesApplied),
			FMath::IsNearlyEqual(OnTheLine.R, CalibratedApexExposure, CalibratedApexExposure * RubberHoldTolerance));

		TestEqual(
			*FString::Printf(TEXT("After %d passes the bare mark still has no rubber under it"), PassesApplied),
			OffTheLine.R,
			0.0f);

		TestTrue(
			*FString::Printf(
				TEXT("After %d passes the mark under traffic keeps what 1 + k times the shared rate predicts, "
					"within 3 %%"),
				PassesApplied),
			FMath::Abs(OnTheLineKept - OnTheLinePredicted) <= OnTheLinePredicted * RelativeTolerance);

		TestTrue(
			*FString::Printf(
				TEXT("After %d passes the bare mark keeps what the shared rate predicts, within 3 %%, so the "
					"coupling reduces to C10's pass where there is no rubber"),
				PassesApplied),
			FMath::Abs(OffTheLineKept - OffTheLinePredicted) <= OffTheLinePredicted * RelativeTolerance);

		TestTrue(
			*FString::Printf(TEXT("After %d passes the mark under traffic has faded further"), PassesApplied),
			OnTheLineKept < OffTheLineKept);
	}

	Subsystem->ResetSurface();

	return true;
}

#endif
