#include "Misc/AutomationTest.h"
#include "SurfaceQueryGather.h"
#include "SurfaceTrackRibbon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FSurfaceTrackRibbon MakeDemoRibbon()
	{
		FSurfaceTrackRibbon Ribbon;
		Ribbon.TrackLength = 180001.0;
		Ribbon.TrackWidth = 800.0;
		Ribbon.CentimetresPerTexel = 3.125;
		Ribbon.bClosedLoop = true;
		return Ribbon;
	}

	FSurfaceTrackCoordinate MakeCoordinate(double LapFraction, double LateralOffsetAlpha, double TrackLength)
	{
		FSurfaceTrackCoordinate Coordinate;
		Coordinate.DistanceAlongCentreline = LapFraction * TrackLength;
		Coordinate.LateralOffsetAlpha = LateralOffsetAlpha;
		return Coordinate;
	}

	float SumWeights(const TArray<FSurfaceQueryTap>& Taps)
	{
		float Total = 0.0f;

		for (const FSurfaceQueryTap& Tap : Taps)
		{
			Total += Tap.Weight;
		}

		return Total;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryTapsSumToTheTentTotal,
	"RacetrackSurface.Query.TapsSumToTheTentTotal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryTapsSumToTheTentTotal::RunTest(const FString& Parameters)
{
	const FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();

	TArray<FSurfaceQueryTap> Taps;

	RacetrackSurfaceQuery::BuildTaps(
		Ribbon,
		0,
		MakeCoordinate(0.37, 0.25, Ribbon.TrackLength),
		RacetrackSurfaceQuery::DefaultContactPatchWidth,
		Taps);

	TestEqual(
		TEXT("Nine tent taps each split into two along-track samples"),
		Taps.Num(),
		RacetrackSurfaceQuery::TapsPerQueryPoint);

	TestTrue(
		FString::Printf(
			TEXT("The weights sum to the tent total, %.6f against %.6f"),
			SumWeights(Taps),
			RacetrackSurfaceQuery::TotalTapWeight),
		FMath::IsNearlyEqual(SumWeights(Taps), RacetrackSurfaceQuery::TotalTapWeight, 1.0e-3f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryTapsStayInsideTheAtlas,
	"RacetrackSurface.Query.TapsStayInsideTheAtlas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryTapsStayInsideTheAtlas::RunTest(const FString& Parameters)
{
	const FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();

	const double LapFractions[] = { 0.0, 0.25, 0.5, 0.75, 0.999999 };
	const double Alphas[] = { -1.0, -0.5, 0.0, 0.5, 1.0 };

	int32 Checked = 0;

	for (const double LapFraction : LapFractions)
	{
		for (const double Alpha : Alphas)
		{
			TArray<FSurfaceQueryTap> Taps;

			RacetrackSurfaceQuery::BuildTaps(
				Ribbon,
				0,
				MakeCoordinate(LapFraction, Alpha, Ribbon.TrackLength),
				RacetrackSurfaceQuery::DefaultContactPatchWidth,
				Taps);

			for (const FSurfaceQueryTap& Tap : Taps)
			{
				if (Tap.AtlasUV.X < 0.0 || Tap.AtlasUV.X > 1.0 || Tap.AtlasUV.Y < 0.0 || Tap.AtlasUV.Y > 1.0)
				{
					AddError(FString::Printf(
						TEXT("A tap at lap %.6f, alpha %.2f left the atlas at UV %.6f, %.6f"),
						LapFraction,
						Alpha,
						Tap.AtlasUV.X,
						Tap.AtlasUV.Y));

					return false;
				}

				++Checked;
			}
		}
	}

	AddInfo(FString::Printf(TEXT("%d taps across the lap seam and both track edges stayed inside the atlas"), Checked));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryTapsCollapseToTheQueriedTexelAtZeroPatchWidth,
	"RacetrackSurface.Query.TapsCollapseToTheQueriedTexelAtZeroPatchWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryTapsCollapseToTheQueriedTexelAtZeroPatchWidth::RunTest(const FString& Parameters)
{
	const FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();

	const FSurfaceTrackCoordinate Coordinate = MakeCoordinate(0.6, 0.0, Ribbon.TrackLength);

	TArray<FSurfaceQueryTap> Taps;

	RacetrackSurfaceQuery::BuildTaps(Ribbon, 0, Coordinate, 0.0, Taps);

	const FVector2D Expected = Ribbon.RibbonToAtlas(
		Ribbon.NormalisedAlongTrack(Coordinate.DistanceAlongCentreline),
		FSurfaceTrackRibbon::NormalisedAcrossTrack(Coordinate.LateralOffsetAlpha));

	const double HalfTexel = 0.5 / Ribbon.AtlasWidth();

	double Weighted = 0.0;

	for (const FSurfaceQueryTap& Tap : Taps)
	{
		Weighted += Tap.AtlasUV.X * Tap.Weight;

		TestTrue(
			FString::Printf(
				TEXT("Across-track collapses onto the queried band, %.6f against %.6f"),
				Tap.AtlasUV.Y,
				Expected.Y),
			FMath::Abs(Tap.AtlasUV.Y - Expected.Y) < 1.0e-6);
	}

	const double Centre = Weighted / RacetrackSurfaceQuery::TotalTapWeight;

	TestTrue(
		FString::Printf(
			TEXT("With no patch the taps average back onto the queried texel, %.8f against %.8f, "
				"inside the half texel of %.8f the texel-centre snap introduces"),
			Centre,
			Expected.X,
			HalfTexel),
		FMath::Abs(Centre - Expected.X) <= HalfTexel);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryTapsSpanTheContactPatchNotOneTexel,
	"RacetrackSurface.Query.TapsSpanTheContactPatchNotOneTexel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryTapsSpanTheContactPatchNotOneTexel::RunTest(const FString& Parameters)
{
	const FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();

	TArray<FSurfaceQueryTap> Taps;

	RacetrackSurfaceQuery::BuildTaps(
		Ribbon,
		0,
		MakeCoordinate(0.5, 0.0, Ribbon.TrackLength),
		RacetrackSurfaceQuery::DefaultContactPatchWidth,
		Taps);

	double Lowest = TNumericLimits<double>::Max();
	double Highest = TNumericLimits<double>::Lowest();

	for (const FSurfaceQueryTap& Tap : Taps)
	{
		const double BandPosition = Tap.AtlasUV.Y * Ribbon.RowCount();
		const double AcrossTrack = BandPosition - FMath::FloorToDouble(BandPosition);

		Lowest = FMath::Min(Lowest, AcrossTrack);
		Highest = FMath::Max(Highest, AcrossTrack);
	}

	const double SpanInTexels = (Highest - Lowest) * Ribbon.AcrossTrackTexels();
	const double PatchInTexels =
		RacetrackSurfaceQuery::DefaultContactPatchWidth / Ribbon.SafeCentimetresPerTexel();

	AddInfo(FString::Printf(
		TEXT("A %.1f cm patch at %.4f cm per texel spans %.4f texels across the ribbon, against the %.4f it covers. "
			"Half a lap is a row boundary on a %d row ribbon, so this also confirms the across-track span "
			"survives an along-track pair folding into two different rows"),
		RacetrackSurfaceQuery::DefaultContactPatchWidth,
		Ribbon.CentimetresPerTexel,
		SpanInTexels,
		PatchInTexels,
		Ribbon.RowCount()));

	TestTrue(
		TEXT("The gather averages more than one texel, which is F1's exit condition"),
		SpanInTexels > 1.0);

	TestTrue(
		FString::Printf(
			TEXT("The span is the contact patch, %.4f texels against %.4f"),
			SpanInTexels,
			PatchInTexels),
		FMath::Abs(SpanInTexels - PatchInTexels) < 1.0e-3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryTapsWrapAcrossTheLapSeam,
	"RacetrackSurface.Query.TapsWrapAcrossTheLapSeam",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryTapsWrapAcrossTheLapSeam::RunTest(const FString& Parameters)
{
	const FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();

	TArray<FSurfaceQueryTap> Taps;

	RacetrackSurfaceQuery::BuildTaps(
		Ribbon,
		0,
		MakeCoordinate(0.0, 0.0, Ribbon.TrackLength),
		RacetrackSurfaceQuery::DefaultContactPatchWidth,
		Taps);

	const int32 LastRow = Ribbon.RowCount() - 1;

	bool bFoundFirstRow = false;
	bool bFoundLastRow = false;

	for (const FSurfaceQueryTap& Tap : Taps)
	{
		if (Tap.Weight <= 0.0f)
		{
			continue;
		}

		const int32 Row = FMath::Clamp(
			FMath::FloorToInt32(Tap.AtlasUV.Y * Ribbon.RowCount()),
			0,
			LastRow);

		bFoundFirstRow |= Row == 0;
		bFoundLastRow |= Row == LastRow;
	}

	TestTrue(TEXT("Taps at the start of the lap reach the first row"), bFoundFirstRow);

	TestTrue(
		TEXT("Taps at the start of the lap wrap back into the last row, which is the surface "
			"physically behind them on a closed loop"),
		bFoundLastRow);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryAnOpenRibbonClampsWhereAClosedOneWraps,
	"RacetrackSurface.Query.AnOpenRibbonClampsWhereAClosedOneWraps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryAnOpenRibbonClampsWhereAClosedOneWraps::RunTest(const FString& Parameters)
{
	FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();
	Ribbon.bClosedLoop = false;

	TArray<FSurfaceQueryTap> Taps;

	RacetrackSurfaceQuery::BuildTaps(
		Ribbon,
		0,
		MakeCoordinate(0.0, 0.0, Ribbon.TrackLength),
		RacetrackSurfaceQuery::DefaultContactPatchWidth,
		Taps);

	for (const FSurfaceQueryTap& Tap : Taps)
	{
		if (Tap.Weight <= 0.0f)
		{
			continue;
		}

		const int32 Row = FMath::FloorToInt32(Tap.AtlasUV.Y * Ribbon.RowCount());

		if (Row != 0)
		{
			AddError(FString::Printf(
				TEXT("An open ribbon wrapped a tap at the start of the track into row %d at UV %.6f, %.6f"),
				Row,
				Tap.AtlasUV.X,
				Tap.AtlasUV.Y));

			return false;
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceQueryTapsUseThePaddedLapLengthTheShaderUses,
	"RacetrackSurface.Query.TapsUseThePaddedLapLengthTheShaderUses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceQueryTapsUseThePaddedLapLengthTheShaderUses::RunTest(const FString& Parameters)
{
	const FSurfaceTrackRibbon Ribbon = MakeDemoRibbon();

	const int32 PaddedLapTexels = Ribbon.RowCount() * Ribbon.AtlasWidth();

	TestNotEqual(
		TEXT("This ribbon really does pad, so the test can tell the two lap lengths apart"),
		PaddedLapTexels,
		Ribbon.AlongTrackTexels());

	AddInfo(FString::Printf(
		TEXT("The demo ribbon is %d x %d in %d rows: the shader's lap is %d texels and "
			"AlongTrackTexels reports %d, a %d texel pad from DivideAndRoundUp"),
		Ribbon.AtlasWidth(),
		Ribbon.AtlasHeight(),
		Ribbon.RowCount(),
		PaddedLapTexels,
		Ribbon.AlongTrackTexels(),
		PaddedLapTexels - Ribbon.AlongTrackTexels()));

	TArray<FSurfaceQueryTap> Taps;

	RacetrackSurfaceQuery::BuildTaps(
		Ribbon,
		0,
		MakeCoordinate((PaddedLapTexels - 0.5) / PaddedLapTexels, 0.0, Ribbon.TrackLength),
		0.0,
		Taps);

	const double ExpectedU = (PaddedLapTexels - 1 - (Ribbon.RowCount() - 1) * Ribbon.AtlasWidth() + 0.5)
		/ Ribbon.AtlasWidth();

	TestTrue(
		FString::Printf(
			TEXT("The last texel of the padded lap reads at U %.8f, against %.8f from the shader's arithmetic"),
			Taps[0].AtlasUV.X,
			ExpectedU),
		FMath::Abs(Taps[0].AtlasUV.X - ExpectedU) < 1.0e-6);

	return true;
}

#endif
