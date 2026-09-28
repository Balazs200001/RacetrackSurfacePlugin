#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/ScopedCVar.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;

namespace RacetrackSurfaceWriteTest
{
	constexpr double CentimetresPerTexel = 2.5;

	constexpr int32 AlongTrackTexels = 1024;

	constexpr int32 AcrossTrackTexels = 64;

	constexpr float ContactWidthCentimetres = 20.0f;

	constexpr double FramesPerSecond = 60.0;

	constexpr float FrameSeconds = static_cast<float>(1.0 / FramesPerSecond);

	constexpr int32 FrameCount = 24;

	constexpr double FirstContactCentimetres = 250.0;

	constexpr double RacingSpeedCentimetresPerSecond = 3000.0;

	constexpr double BrakingSpeedCentimetresPerSecond = 1500.0;

	constexpr int32 WidthTexels = static_cast<int32>(ContactWidthCentimetres / CentimetresPerTexel);

	constexpr int32 MarkFirstRow = AcrossTrackTexels / 2 - WidthTexels / 2;

	constexpr int32 FirstContactTexel = static_cast<int32>(FirstContactCentimetres / CentimetresPerTexel);

	constexpr float HalfFloatRelativeTolerance = 1.0e-3f;

	struct FMarkLayout
	{
		double SpeedCentimetresPerSecond = 0.0;

		int32 TravelTexels = 0;

		int32 FirstColumn = 0;

		int32 EndColumn = 0;

		int32 PeriodicFirstColumn = 0;

		int32 PeriodicEndColumn = 0;

		float SweptShare() const
		{
			return static_cast<float>(TravelTexels) / (TravelTexels + WidthTexels);
		}

		bool IsInside(int32 Column, int32 Row) const
		{
			return Column >= FirstColumn
				&& Column < EndColumn
				&& Row >= MarkFirstRow
				&& Row < MarkFirstRow + WidthTexels;
		}

		bool IsPeriodic(int32 Column, int32 Row) const
		{
			return IsInside(Column, Row) && Column >= PeriodicFirstColumn && Column < PeriodicEndColumn;
		}
	};

	FMarkLayout LayoutAtSpeed(double SpeedCentimetresPerSecond)
	{
		FMarkLayout Layout;
		Layout.SpeedCentimetresPerSecond = SpeedCentimetresPerSecond;
		Layout.TravelTexels = FMath::RoundToInt32(SpeedCentimetresPerSecond / FramesPerSecond / CentimetresPerTexel);
		Layout.FirstColumn = FirstContactTexel - Layout.TravelTexels - WidthTexels / 2;
		Layout.EndColumn = FirstContactTexel + Layout.TravelTexels * (FrameCount - 1) + WidthTexels / 2;
		Layout.PeriodicFirstColumn = Layout.FirstColumn + Layout.TravelTexels;
		Layout.PeriodicEndColumn = Layout.PeriodicFirstColumn + Layout.TravelTexels * (FrameCount - 1);
		return Layout;
	}

	FSurfaceTrackRibbon MakeWriteRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.CentimetresPerTexel = CentimetresPerTexel;
		Ribbon.TrackLength = AlongTrackTexels * CentimetresPerTexel;
		Ribbon.TrackWidth = AcrossTrackTexels * CentimetresPerTexel;
		return Ribbon;
	}

	USurfaceAccumulationSubsystem* PrepareSubsystemWithoutACentreline(FAutomationTestBase& Test, UWorld*& OutWorld)
	{
		OutWorld = FindRenderableWorld();

		USurfaceAccumulationSubsystem* Subsystem =
			OutWorld != nullptr ? OutWorld->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

		if (Subsystem == nullptr || !Subsystem->IsInitialized())
		{
			Test.AddError(TEXT("No renderable world with an initialised accumulation subsystem."));
			return nullptr;
		}

		Subsystem->ConfigureTrackRibbon(nullptr, MakeWriteRibbon());
		Subsystem->ResetSurface();

		return Subsystem;
	}

	void LayMark(USurfaceAccumulationSubsystem& Subsystem, const FMarkLayout& Layout, float SlipAmount)
	{
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			Subsystem.RegisterSurfaceContactInTrackSpace(
				FirstContactCentimetres + Layout.SpeedCentimetresPerSecond / FramesPerSecond * Frame,
				0.0,
				Layout.SpeedCentimetresPerSecond,
				SlipAmount,
				ContactWidthCentimetres);

			Subsystem.Tick(FrameSeconds);
		}

		FlushRenderingCommands();
	}

	bool ReadWholeTarget(UWorld* World, USurfaceAccumulationSubsystem& Subsystem, TArray<FLinearColor>& OutTexels)
	{
		UTextureRenderTarget2D* Target = Subsystem.GetOrCreateAccumulationTarget();

		return Target != nullptr
			&& Target->SizeX == AlongTrackTexels
			&& Target->SizeY == AcrossTrackTexels
			&& UKismetRenderingLibrary::ReadRenderTargetRaw(World, Target, OutTexels, false)
			&& OutTexels.Num() == AlongTrackTexels * AcrossTrackTexels;
	}

	const FLinearColor& TexelAt(const TArray<FLinearColor>& Texels, int32 Column, int32 Row)
	{
		return Texels[Row * AlongTrackTexels + Column];
	}

	double PeriodicMean(const TArray<FLinearColor>& Texels, const FMarkLayout& Layout, int32 ChannelIndex)
	{
		double Total = 0.0;
		int32 Count = 0;

		for (int32 Row = MarkFirstRow; Row < MarkFirstRow + WidthTexels; ++Row)
		{
			for (int32 Column = Layout.PeriodicFirstColumn; Column < Layout.PeriodicEndColumn; ++Column)
			{
				Total += TexelAt(Texels, Column, Row).Component(ChannelIndex);
				++Count;
			}
		}

		return Count > 0 ? Total / Count : 0.0;
	}

	bool IsNearlyRelative(double Value, double Expected)
	{
		return FMath::Abs(Value - Expected) <= FMath::Abs(Expected) * HalfFloatRelativeTolerance;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceWriteSlipBelowOneWritesRubberOnly,
	"RacetrackSurface.Write.SlipBelowOneWritesRubberOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceWriteSlipBelowOneWritesRubberOnly::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceWriteTest;

	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = nullptr;
	USurfaceAccumulationSubsystem* Subsystem = PrepareSubsystemWithoutACentreline(*this, World);

	if (Subsystem == nullptr)
	{
		return false;
	}

	const FScopedCVar<float> DerivedDeposition(RacetrackSurfaceMapping::DepositionScaleVariableName, 1.0f);
	const FScopedCVar<float> NoDecay(RacetrackSurfaceMapping::DecayRateVariableName, 0.0f);

	constexpr float SlipAmount = 0.5f;

	const FMarkLayout Layout = LayoutAtSpeed(RacingSpeedCentimetresPerSecond);

	LayMark(*Subsystem, Layout, SlipAmount);

	for (const FSurfaceBrushStroke& Stroke : Subsystem->GetStrokesResolvedThisFrame())
	{
		TestEqual(
			TEXT("A contact below full slip resolves to a rubber stroke, with no channel given by the caller"),
			Stroke.Channel,
			EAccumulationChannel::RubberBuildup);
	}

	TArray<FLinearColor> Texels;

	if (!ReadWholeTarget(World, *Subsystem, Texels))
	{
		AddError(TEXT("Could not read the accumulation target back."));
		Subsystem->ResetSurface();
		return false;
	}

	const float DerivedExposure = RacetrackSurfaceMapping::SlipToExposure(SlipAmount, 1.0f);
	const double MeanRubber = PeriodicMean(Texels, Layout, 0);

	int32 SkidTexels = 0;
	int32 MarbleTexels = 0;
	int32 RubberOutsideTheMark = 0;

	for (int32 Row = 0; Row < AcrossTrackTexels; ++Row)
	{
		for (int32 Column = 0; Column < AlongTrackTexels; ++Column)
		{
			const FLinearColor& Texel = TexelAt(Texels, Column, Row);

			SkidTexels += Texel.G != 0.0f ? 1 : 0;
			MarbleTexels += Texel.B != 0.0f ? 1 : 0;
			RubberOutsideTheMark += !Layout.IsInside(Column, Row) && Texel.R != 0.0f ? 1 : 0;
		}
	}

	AddInfo(FString::Printf(
		TEXT("Slip %.2f over %d frames: mean rubber %.7f across %d whole periods of the mark against the derived %.7f, "
			"%d texels of skid, %d of marbles, %d of rubber outside the mark"),
		SlipAmount,
		FrameCount,
		MeanRubber,
		FrameCount - 1,
		DerivedExposure,
		SkidTexels,
		MarbleTexels,
		RubberOutsideTheMark));

	TestTrue(
		FString::Printf(
			TEXT("The mark's mean rubber is the derived exposure per pass, %.7f against %.7f"),
			MeanRubber,
			DerivedExposure),
		IsNearlyRelative(MeanRubber, DerivedExposure));

	TestEqual(TEXT("Nothing lands in the skid channel"), SkidTexels, 0);
	TestEqual(TEXT("Nothing lands in the marble channel"), MarbleTexels, 0);
	TestEqual(TEXT("Rubber lands only under the swept contact"), RubberOutsideTheMark, 0);

	Subsystem->ResetSurface();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceWriteLockupWritesAFullSkidMarkOnly,
	"RacetrackSurface.Write.LockupWritesAFullSkidMarkOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceWriteLockupWritesAFullSkidMarkOnly::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceWriteTest;

	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = nullptr;
	USurfaceAccumulationSubsystem* Subsystem = PrepareSubsystemWithoutACentreline(*this, World);

	if (Subsystem == nullptr)
	{
		return false;
	}

	const FScopedCVar<float> NoDecay(RacetrackSurfaceMapping::DecayRateVariableName, 0.0f);

	const float LockupExposure = RacetrackSurfaceMapping::LockupExposure();

	AddInfo(TEXT("speed cm/s | travel T texels | width W texels | single texels read, predicted E T/(T+W) | double "
		"texels read, predicted 2E T/(T+W) | single share, predicted (T-W)/T | mean, predicted E | thinnest coverage"));

	for (const double Speed : { RacingSpeedCentimetresPerSecond, BrakingSpeedCentimetresPerSecond })
	{
		const FMarkLayout Layout = LayoutAtSpeed(Speed);

		Subsystem->ResetSurface();
		LayMark(*Subsystem, Layout, 1.0f);

		for (const FSurfaceBrushStroke& Stroke : Subsystem->GetStrokesResolvedThisFrame())
		{
			TestEqual(TEXT("A full-slip contact resolves to a skid stroke"), Stroke.Channel, EAccumulationChannel::SkidMark);
		}

		TArray<FLinearColor> Texels;

		if (!ReadWholeTarget(World, *Subsystem, Texels))
		{
			AddError(TEXT("Could not read the accumulation target back."));
			Subsystem->ResetSurface();
			return false;
		}

		const double SingleExposure = LockupExposure * Layout.SweptShare();
		const double DoubleExposure = SingleExposure * 2.0;

		int32 RubberTexels = 0;
		int32 MarbleTexels = 0;
		int32 SkidOutsideTheMark = 0;
		int32 InsideTexels = 0;
		int32 NeitherSingleNorDouble = 0;
		int32 BelowTheTarget = 0;
		int32 PeriodicTexels = 0;
		int32 PeriodicSingles = 0;
		double SingleTotal = 0.0;
		double DoubleTotal = 0.0;
		int32 DoubleCount = 0;
		int32 SingleCount = 0;
		float ThinnestCoverage = 1.0f;

		for (int32 Row = 0; Row < AcrossTrackTexels; ++Row)
		{
			for (int32 Column = 0; Column < AlongTrackTexels; ++Column)
			{
				const FLinearColor& Texel = TexelAt(Texels, Column, Row);

				RubberTexels += Texel.R != 0.0f ? 1 : 0;
				MarbleTexels += Texel.B != 0.0f ? 1 : 0;

				if (!Layout.IsInside(Column, Row))
				{
					SkidOutsideTheMark += Texel.G != 0.0f ? 1 : 0;
					continue;
				}

				++InsideTexels;

				const float Coverage = RacetrackSurfaceMapping::ExposureToCoverage(Texel.G);
				ThinnestCoverage = FMath::Min(ThinnestCoverage, Coverage);
				BelowTheTarget += Coverage < RacetrackSurfaceMapping::LockupTargetCoverage ? 1 : 0;

				const bool bSingle = IsNearlyRelative(Texel.G, SingleExposure);
				const bool bDouble = IsNearlyRelative(Texel.G, DoubleExposure);

				NeitherSingleNorDouble += !bSingle && !bDouble ? 1 : 0;

				if (bSingle)
				{
					SingleTotal += Texel.G;
					++SingleCount;
				}

				if (bDouble)
				{
					DoubleTotal += Texel.G;
					++DoubleCount;
				}

				if (Layout.IsPeriodic(Column, Row))
				{
					++PeriodicTexels;
					PeriodicSingles += bSingle ? 1 : 0;
				}
			}
		}

		const double SingleShare = PeriodicTexels > 0 ? static_cast<double>(PeriodicSingles) / PeriodicTexels : 0.0;
		const double PredictedSingleShare =
			static_cast<double>(Layout.TravelTexels - WidthTexels) / Layout.TravelTexels;
		const double MeanSkid = PeriodicMean(Texels, Layout, 1);

		AddInfo(FString::Printf(
			TEXT("%.0f | %d | %d | %.5f, %.5f | %.5f, %.5f | %.4f, %.4f | %.5f, %.5f | %.5f"),
			Speed,
			Layout.TravelTexels,
			WidthTexels,
			SingleCount > 0 ? SingleTotal / SingleCount : 0.0,
			SingleExposure,
			DoubleCount > 0 ? DoubleTotal / DoubleCount : 0.0,
			DoubleExposure,
			SingleShare,
			PredictedSingleShare,
			MeanSkid,
			LockupExposure,
			ThinnestCoverage));

		const FString AtSpeed = FString::Printf(TEXT(" at %.0f cm/s"), Speed);

		TestEqual(*(TEXT("A lockup lays no rubber") + AtSpeed), RubberTexels, 0);
		TestEqual(*(TEXT("A lockup lays no marbles without a centreline") + AtSpeed), MarbleTexels, 0);
		TestEqual(*(TEXT("Skid lands only under the swept contact") + AtSpeed), SkidOutsideTheMark, 0);

		TestTrue(*(TEXT("The mark covers the texels the layout predicts") + AtSpeed), InsideTexels > 0);

		TestEqual(
			*(TEXT("Every texel of the mark takes one pass or two, and reads E T/(T+W) or twice that") + AtSpeed),
			NeitherSingleNorDouble,
			0);

		TestEqual(
			*(TEXT("Every texel of the mark reads at least the lockup's target coverage on the frame it is laid")
				+ AtSpeed),
			BelowTheTarget,
			0);

		TestTrue(
			*(FString::Printf(TEXT("The single-covered share is (T-W)/T, %.4f against %.4f"), SingleShare,
				PredictedSingleShare) + AtSpeed),
			FMath::IsNearlyEqual(SingleShare, PredictedSingleShare, 1.0e-6));

		TestTrue(
			*(FString::Printf(TEXT("The mark's mean skid is the lockup exposure, %.5f against %.5f"), MeanSkid,
				LockupExposure) + AtSpeed),
			IsNearlyRelative(MeanSkid, LockupExposure));
	}

	Subsystem->ResetSurface();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceWriteLockupIgnoresDepositionScale,
	"RacetrackSurface.Write.LockupIgnoresDepositionScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceWriteLockupIgnoresDepositionScale::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceWriteTest;

	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = nullptr;
	USurfaceAccumulationSubsystem* Subsystem = PrepareSubsystemWithoutACentreline(*this, World);

	if (Subsystem == nullptr)
	{
		return false;
	}

	const FScopedCVar<float> NoDecay(RacetrackSurfaceMapping::DecayRateVariableName, 0.0f);

	constexpr float RaisedDeposition = 32.0f;
	constexpr float RubberSlip = 0.5f;

	const FMarkLayout Layout = LayoutAtSpeed(RacingSpeedCentimetresPerSecond);

	TArray<FLinearColor> LockupAtDerived;
	TArray<FLinearColor> LockupAtRaised;
	TArray<FLinearColor> RubberAtDerived;
	TArray<FLinearColor> RubberAtRaised;

	const auto LayAndRead = [&](float DepositionScale, float SlipAmount, TArray<FLinearColor>& OutTexels)
	{
		const FScopedCVar<float> Deposition(RacetrackSurfaceMapping::DepositionScaleVariableName, DepositionScale);

		Subsystem->ResetSurface();
		LayMark(*Subsystem, Layout, SlipAmount);

		return ReadWholeTarget(World, *Subsystem, OutTexels);
	};

	if (!LayAndRead(1.0f, 1.0f, LockupAtDerived)
		|| !LayAndRead(RaisedDeposition, 1.0f, LockupAtRaised)
		|| !LayAndRead(1.0f, RubberSlip, RubberAtDerived)
		|| !LayAndRead(RaisedDeposition, RubberSlip, RubberAtRaised))
	{
		AddError(TEXT("Could not read the accumulation target back."));
		Subsystem->ResetSurface();
		return false;
	}

	int32 DifferingTexels = 0;

	for (int32 Index = 0; Index < LockupAtDerived.Num(); ++Index)
	{
		DifferingTexels += LockupAtDerived[Index] != LockupAtRaised[Index] ? 1 : 0;
	}

	const double LockupMean = PeriodicMean(LockupAtDerived, Layout, 1);
	const double RubberRatio =
		PeriodicMean(RubberAtRaised, Layout, 0)
		/ FMath::Max(PeriodicMean(RubberAtDerived, Layout, 0), UE_DOUBLE_SMALL_NUMBER);

	AddInfo(FString::Printf(
		TEXT("Lockup mark mean skid %.5f at deposition 1 and at %.0f, %d texels differing. The same mark at slip %.2f "
			"reads %.3f times as much rubber at %.0f as at 1"),
		LockupMean,
		RaisedDeposition,
		DifferingTexels,
		RubberSlip,
		RubberRatio,
		RaisedDeposition));

	TestEqual(
		TEXT("A lockup mark reads the same at raised deposition, texel for texel, because it is one mark rather than "
			"accumulation"),
		DifferingTexels,
		0);

	TestTrue(
		TEXT("The same raised deposition does scale a rubber mark, so the setting was in force when the lockup "
			"ignored it"),
		FMath::IsNearlyEqual(RubberRatio, static_cast<double>(RaisedDeposition), RaisedDeposition * 0.01));

	Subsystem->ResetSurface();

	return true;
}

#endif
