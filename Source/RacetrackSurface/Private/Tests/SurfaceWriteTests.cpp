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

	constexpr double SpeedCentimetresPerSecond = 3000.0;

	constexpr float FrameSeconds = 1.0f / 60.0f;

	constexpr int32 FrameCount = 24;

	constexpr double FirstContactCentimetres = 250.0;

	constexpr double TravelTexels = SpeedCentimetresPerSecond / 60.0 / CentimetresPerTexel;

	constexpr double WidthTexels = ContactWidthCentimetres / CentimetresPerTexel;

	constexpr int32 MarkFirstRow = static_cast<int32>(AcrossTrackTexels / 2 - WidthTexels / 2);

	constexpr int32 MarkRowCount = static_cast<int32>(WidthTexels);

	constexpr int32 FirstContactTexel = static_cast<int32>(FirstContactCentimetres / CentimetresPerTexel);

	constexpr int32 MarkFirstColumn = static_cast<int32>(FirstContactTexel - TravelTexels - WidthTexels / 2);

	constexpr int32 MarkEndColumn =
		static_cast<int32>(FirstContactTexel + TravelTexels * (FrameCount - 1) + WidthTexels / 2);

	constexpr int32 PeriodicFirstColumn = static_cast<int32>(MarkFirstColumn + TravelTexels);

	constexpr int32 PeriodicEndColumn = static_cast<int32>(PeriodicFirstColumn + TravelTexels * (FrameCount - 1));

	FSurfaceTrackRibbon MakeWriteRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.CentimetresPerTexel = CentimetresPerTexel;
		Ribbon.TrackLength = AlongTrackTexels * CentimetresPerTexel;
		Ribbon.TrackWidth = AcrossTrackTexels * CentimetresPerTexel;
		return Ribbon;
	}

	bool IsInsideTheMark(int32 Column, int32 Row)
	{
		return Column >= MarkFirstColumn
			&& Column < MarkEndColumn
			&& Row >= MarkFirstRow
			&& Row < MarkFirstRow + MarkRowCount;
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

	void LayMarkAtSlip(USurfaceAccumulationSubsystem& Subsystem, float SlipAmount)
	{
		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			Subsystem.RegisterSurfaceContactInTrackSpace(
				FirstContactCentimetres + SpeedCentimetresPerSecond / 60.0 * Frame,
				0.0,
				SpeedCentimetresPerSecond,
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

	LayMarkAtSlip(*Subsystem, SlipAmount);

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

	double PeriodicTotal = 0.0;
	int32 PeriodicTexels = 0;
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
			RubberOutsideTheMark += !IsInsideTheMark(Column, Row) && Texel.R != 0.0f ? 1 : 0;

			if (Row >= MarkFirstRow && Row < MarkFirstRow + MarkRowCount
				&& Column >= PeriodicFirstColumn && Column < PeriodicEndColumn)
			{
				PeriodicTotal += Texel.R;
				++PeriodicTexels;
			}
		}
	}

	const double PeriodicMean = PeriodicTexels > 0 ? PeriodicTotal / PeriodicTexels : 0.0;

	AddInfo(FString::Printf(
		TEXT("Slip %.2f over %d frames: mean rubber %.7f across %d whole periods of the mark against the derived %.7f, "
			"%d texels of skid, %d of marbles, %d of rubber outside the mark"),
		SlipAmount,
		FrameCount,
		PeriodicMean,
		FrameCount - 1,
		DerivedExposure,
		SkidTexels,
		MarbleTexels,
		RubberOutsideTheMark));

	TestTrue(
		FString::Printf(
			TEXT("The mark's mean rubber is the derived exposure per pass, %.7f against %.7f"),
			PeriodicMean,
			DerivedExposure),
		FMath::IsNearlyEqual(PeriodicMean, static_cast<double>(DerivedExposure), DerivedExposure * 1.0e-3));

	TestEqual(TEXT("Nothing lands in the skid channel"), SkidTexels, 0);
	TestEqual(TEXT("Nothing lands in the marble channel"), MarbleTexels, 0);
	TestEqual(TEXT("Rubber lands only under the swept contact"), RubberOutsideTheMark, 0);

	Subsystem->ResetSurface();

	return true;
}

#endif
