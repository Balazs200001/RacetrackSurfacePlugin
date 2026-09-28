#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceFanPrecisionTest
{
	constexpr int32 FanBufferTexels = 128;

	constexpr int32 FanBandCount = 8;

	constexpr int32 FanBandPitchTexels = FanBufferTexels / FanBandCount;

	constexpr float FanBandWidthTexels = 8.0f;

	constexpr int32 FanPassCount = 4096;

	constexpr float FanFullCorneringSlip = 0.5f;

	constexpr float FanExposurePerUnitSlip = 5.0e-5f;

	constexpr double FanCorneringSpeedCentimetresPerSecond = 3000.0 * 0.55;

	constexpr double FanFramesPerSecond = 60.0;

	constexpr double FanContactWidthCentimetres = 20.0;

	constexpr float FanRelativeTolerance = 1.0e-3f;

	constexpr int32 HalfFloatMantissaBits = 10;

	float PeakIncrementPerPass()
	{
		const double Travel = FanCorneringSpeedCentimetresPerSecond / FanFramesPerSecond;
		const double SweptShare = Travel / (Travel + FanContactWidthCentimetres);

		return static_cast<float>(FanFullCorneringSlip * FanExposurePerUnitSlip * SweptShare);
	}

	float BandIncrementPerPass(int32 Band)
	{
		return PeakIncrementPerPass() * static_cast<float>(FanBandCount - Band) / FanBandCount;
	}

	int32 BandCentreColumn(int32 Band)
	{
		return Band * FanBandPitchTexels + FanBandPitchTexels / 2;
	}

	double SmallestPowerOfTwoAtLeast(double Value)
	{
		return FMath::Pow(2.0, FMath::CeilToDouble(FMath::Log2(Value)));
	}

	double SmallestPowerOfTwoAbove(double Value)
	{
		const double AtLeast = SmallestPowerOfTwoAtLeast(Value);
		return AtLeast > Value ? AtLeast : AtLeast * 2.0;
	}

	double HalfFloatPlateauRoundingToNearest(double Increment)
	{
		return SmallestPowerOfTwoAtLeast(Increment * FMath::Pow(2.0, HalfFloatMantissaBits + 1));
	}

	double HalfFloatPlateauTruncating(double Increment)
	{
		return SmallestPowerOfTwoAbove(Increment * FMath::Pow(2.0, HalfFloatMantissaBits));
	}

	TArray<FSurfaceBrushStroke> MakeFanProfilePasses()
	{
		TArray<FSurfaceBrushStroke> Strokes;
		Strokes.Reserve(FanBandCount * FanPassCount);

		for (int32 Pass = 0; Pass < FanPassCount; ++Pass)
		{
			for (int32 Band = 0; Band < FanBandCount; ++Band)
			{
				const FVector2D Centre(
					(BandCentreColumn(Band) + 0.5) / FanBufferTexels,
					(FanBufferTexels / 2 + 0.5) / FanBufferTexels);

				FSurfaceBrushStroke Stroke;
				Stroke.RegionStart = Centre;
				Stroke.RegionEnd = Centre;
				Stroke.WidthInTexels = FanBandWidthTexels;
				Stroke.Intensity = BandIncrementPerPass(Band);
				Stroke.Channel = EAccumulationChannel::Marbles;

				Strokes.Add(Stroke);
			}
		}

		return Strokes;
	}

	bool PaintAndReadBands(
		UWorld* World,
		USurfaceAccumulationBuffer& Buffer,
		const TArray<FSurfaceBrushStroke>& Passes,
		TArray<float>& OutStored)
	{
		Buffer.Clear(World);
		Buffer.PaintStrokes(World, Passes);
		FlushRenderingCommands();

		const TArray<FLinearColor> Row = UKismetRenderingLibrary::ReadRenderTargetRawPixelArea(
			World, Buffer.GetTarget(), 0, FanBufferTexels / 2, FanBufferTexels, 1, false);

		if (Row.Num() != FanBufferTexels)
		{
			return false;
		}

		OutStored.Reset(FanBandCount);

		for (int32 Band = 0; Band < FanBandCount; ++Band)
		{
			OutStored.Add(Row[BandCentreColumn(Band)].B);
		}

		return true;
	}

	const TCHAR* FormatName(ETextureRenderTargetFormat Format)
	{
		switch (Format)
		{
		case RTF_RGBA16f:
			return TEXT("RGBA16f");
		case RTF_RGBA32f:
			return TEXT("RGBA32f");
		default:
			return TEXT("another format");
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceMarbleFanSurvivesTheBufferFormat,
	"RacetrackSurface.Marbles.FanSurvivesTheBufferFormat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceMarbleFanSurvivesTheBufferFormat::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceFanPrecisionTest;

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

	TStrongObjectPtr<USurfaceAccumulationBuffer> Production(NewObject<USurfaceAccumulationBuffer>());

	if (!Production->Allocate(World, FanBufferTexels, FanBufferTexels))
	{
		AddError(TEXT("Failed to allocate a buffer in the production format."));
		return false;
	}

	const ETextureRenderTargetFormat ProductionFormat = Production->GetAllocatedFormat();
	const ETextureRenderTargetFormat OtherFormat = ProductionFormat == RTF_RGBA32f ? RTF_RGBA16f : RTF_RGBA32f;

	TStrongObjectPtr<USurfaceAccumulationBuffer> Other(NewObject<USurfaceAccumulationBuffer>());

	if (!Other->Allocate(World, FanBufferTexels, FanBufferTexels, OtherFormat))
	{
		AddError(FString::Printf(TEXT("Failed to allocate a buffer in %s."), FormatName(OtherFormat)));
		Production->Release();
		return false;
	}

	const TArray<FSurfaceBrushStroke> Passes = MakeFanProfilePasses();

	TArray<float> StoredInProduction;
	TArray<float> StoredInOther;

	if (!PaintAndReadBands(World, *Production, Passes, StoredInProduction)
		|| !PaintAndReadBands(World, *Other, Passes, StoredInOther))
	{
		AddError(TEXT("Readback did not return the painted row."));
		Production->Release();
		Other->Release();
		return false;
	}

	const TArray<float>& StoredIn16f = ProductionFormat == RTF_RGBA16f ? StoredInProduction : StoredInOther;
	const TArray<float>& StoredIn32f = ProductionFormat == RTF_RGBA16f ? StoredInOther : StoredInProduction;

	AddInfo(FString::Printf(
		TEXT("Production format %s. %d additive passes per band, peak increment %.4e per pass: slip %.2f x %.1e x "
			"the swept share at %.0f cm/s and %.0f fps"),
		FormatName(ProductionFormat),
		FanPassCount,
		PeakIncrementPerPass(),
		FanFullCorneringSlip,
		FanExposurePerUnitSlip,
		FanCorneringSpeedCentimetresPerSecond,
		FanFramesPerSecond));

	AddInfo(TEXT("band | increment | float sum | RGBA16f stored | half-float plateau, nearest | half-float plateau, "
		"truncating | RGBA32f stored"));

	int32 ProductionBandsOutOfTolerance = 0;

	for (int32 Band = 0; Band < FanBandCount; ++Band)
	{
		const double Increment = BandIncrementPerPass(Band);
		const double FloatSum = Increment * FanPassCount;

		AddInfo(FString::Printf(
			TEXT("%d | %.4e | %.6f | %.6f | %.6f | %.6f | %.6f"),
			Band,
			Increment,
			FloatSum,
			StoredIn16f[Band],
			HalfFloatPlateauRoundingToNearest(Increment),
			HalfFloatPlateauTruncating(Increment),
			StoredIn32f[Band]));

		ProductionBandsOutOfTolerance +=
			FMath::Abs(StoredInProduction[Band] - FloatSum) > FloatSum * FanRelativeTolerance ? 1 : 0;
	}

	TestEqual(
		*FString::Printf(
			TEXT("Every band of the fan's profile, added %d times in the production format %s, reads back within %.1f "
				"%% of the float sum, so the fan's ramp survives the buffer rather than terracing"),
			FanPassCount,
			FormatName(ProductionFormat),
			FanRelativeTolerance * 100.0f),
		ProductionBandsOutOfTolerance,
		0);

	Production->Release();
	Other->Release();

	return true;
}

#endif
