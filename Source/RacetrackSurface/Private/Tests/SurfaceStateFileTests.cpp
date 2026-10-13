#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopedCVar.h"
#include "HAL/FileManager.h"
#include "Components/SplineComponent.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceStateTest
{
	constexpr float FrameSeconds = 1.0f / 60.0f;

	constexpr int32 FrameCount = 24;

	constexpr double FirstContactCentimetres = 1000.0;

	constexpr double SpeedCentimetresPerSecond = 3000.0;

	constexpr float ContactWidthCentimetres = 20.0f;

	constexpr float RubberSlip = 0.5f;

	constexpr float LockupSlip = 1.0f;

	USurfaceAccumulationSubsystem* FindInitialisedSubsystem(FAutomationTestBase& Test, UWorld*& OutWorld)
	{
		OutWorld = RacetrackSurfaceTest::FindRenderableWorld();

		USurfaceAccumulationSubsystem* Subsystem =
			OutWorld != nullptr ? OutWorld->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

		if (Subsystem == nullptr || !Subsystem->IsInitialized())
		{
			Test.AddError(TEXT("No renderable world with an initialised accumulation subsystem."));
			return nullptr;
		}

		return Subsystem;
	}

	void LayRubberBesideALockup(USurfaceAccumulationSubsystem& Subsystem, double RubberAlpha, double LockupAlpha)
	{
		const FScopedCVar<float> NoDecay(RacetrackSurfaceMapping::DecayRateVariableName, 0.0f);

		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			const double Distance = FirstContactCentimetres + SpeedCentimetresPerSecond * FrameSeconds * Frame;

			Subsystem.RegisterSurfaceContactInTrackSpace(
				Distance, RubberAlpha, SpeedCentimetresPerSecond, RubberSlip, ContactWidthCentimetres);

			Subsystem.RegisterSurfaceContactInTrackSpace(
				Distance, LockupAlpha, SpeedCentimetresPerSecond, LockupSlip, ContactWidthCentimetres);

			Subsystem.Tick(FrameSeconds);
		}
	}

	void BankDecayProgressWithoutAPass(USurfaceAccumulationSubsystem& Subsystem)
	{
		const FScopedCVar<float> Decay(RacetrackSurfaceMapping::DecayRateVariableName, 1.0f);
		const float HalfLifeToRestore = Subsystem.GetDecayHalfLifeSeconds();

		Subsystem.SetDecayHalfLifeSeconds(RacetrackSurfaceMapping::FallbackDecayHalfLifeSeconds);
		Subsystem.Tick(FrameSeconds);
		Subsystem.SetDecayHalfLifeSeconds(HalfLifeToRestore);
	}

	bool ReadWholeTarget(UWorld* World, USurfaceAccumulationSubsystem& Subsystem, TArray<FLinearColor>& OutTexels)
	{
		FlushRenderingCommands();

		UTextureRenderTarget2D* Target = Subsystem.GetOrCreateAccumulationTarget();

		return Target != nullptr && UKismetRenderingLibrary::ReadRenderTargetRaw(World, Target, OutTexels, false);
	}

	int32 CountTexelsDiffering(const TArray<FLinearColor>& Expected, const TArray<FLinearColor>& Actual)
	{
		if (Expected.Num() != Actual.Num())
		{
			return FMath::Max(Expected.Num(), Actual.Num());
		}

		int32 Differing = 0;

		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			Differing += FMemory::Memcmp(&Expected[Index], &Actual[Index], sizeof(FLinearColor)) != 0 ? 1 : 0;
		}

		return Differing;
	}

	FString StateFilePath(const TCHAR* TestName)
	{
		return FPaths::Combine(FPaths::AutomationTransientDir(), FString::Printf(TEXT("%s.surfacestate"), TestName));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStateSurvivesASaveAndALoadIntoAFreshTarget,
	"RacetrackSurface.State.SurvivesASaveAndALoadIntoAFreshTarget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStateSurvivesASaveAndALoadIntoAFreshTarget::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceStateTest;

	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = nullptr;
	USurfaceAccumulationSubsystem* Subsystem = FindInitialisedSubsystem(*this, World);

	if (Subsystem == nullptr)
	{
		return false;
	}

	const TStrongObjectPtr<USplineComponent> Circuit = RacetrackSurfaceTest::MakeTestCircuit();
	const FSurfaceTrackRibbon Ribbon = RacetrackSurfaceTest::MakeTestRibbon(*Circuit);
	const FString FilePath = StateFilePath(TEXT("SurvivesASaveAndALoadIntoAFreshTarget"));

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), Ribbon);
	Subsystem->ResetSurface();

	LayRubberBesideALockup(*Subsystem, -0.3, 0.3);
	BankDecayProgressWithoutAPass(*Subsystem);

	const float SavedDecayProgress = Subsystem->GetDecayProgressInHalfLives();

	TArray<FLinearColor> Saved;

	if (!ReadWholeTarget(World, *Subsystem, Saved))
	{
		AddError(TEXT("Could not read the painted target back before saving."));
		Subsystem->ResetSurface();
		return false;
	}

	int32 RubberTexels = 0;
	int32 SkidTexels = 0;
	int32 MarbleTexels = 0;

	for (const FLinearColor& Texel : Saved)
	{
		RubberTexels += Texel.R != 0.0f ? 1 : 0;
		SkidTexels += Texel.G != 0.0f ? 1 : 0;
		MarbleTexels += Texel.B != 0.0f ? 1 : 0;
	}

	const UTextureRenderTarget2D* SavedTarget = Subsystem->GetOrCreateAccumulationTarget();

	TestTrue(TEXT("The painted surface saves to a file"), Subsystem->SaveSurfaceState(FilePath));

	FSurfaceTrackRibbon Narrower = Ribbon;
	Narrower.TrackWidth = Ribbon.TrackWidth * 0.5;

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), Narrower);
	Subsystem->GetOrCreateAccumulationTarget();
	Subsystem->ConfigureTrackRibbon(Circuit.Get(), Ribbon);

	TestTrue(TEXT("The file loads into the ribbon it was saved from"), Subsystem->LoadSurfaceState(FilePath));

	const UTextureRenderTarget2D* LoadedTarget = Subsystem->GetOrCreateAccumulationTarget();

	TArray<FLinearColor> Loaded;

	if (!ReadWholeTarget(World, *Subsystem, Loaded))
	{
		AddError(TEXT("Could not read the loaded target back."));
		IFileManager::Get().Delete(*FilePath);
		Subsystem->ResetSurface();
		return false;
	}

	const int32 Differing = CountTexelsDiffering(Saved, Loaded);
	const int64 FileBytes = IFileManager::Get().FileSize(*FilePath);
	const int64 RawBytes = static_cast<int64>(Saved.Num()) * static_cast<int64>(sizeof(FLinearColor));

	AddInfo(FString::Printf(
		TEXT("Saved %d x %d texels with %d of rubber, %d of skid and %d of marbles, and decay progress %.9f half-lives. ")
		TEXT("The file is %lld bytes against %lld raw. After loading into a fresh target, %d texels differ and the ")
		TEXT("decay progress is %.9f"),
		Ribbon.AtlasWidth(),
		Ribbon.AtlasHeight(),
		RubberTexels,
		SkidTexels,
		MarbleTexels,
		SavedDecayProgress,
		FileBytes,
		RawBytes,
		Differing,
		Subsystem->GetDecayProgressInHalfLives()));

	TestTrue(TEXT("Rubber was laid before saving"), RubberTexels > 0);
	TestTrue(TEXT("A lockup was laid before saving"), SkidTexels > 0);
	TestTrue(TEXT("Marbles were thrown before saving"), MarbleTexels > 0);
	TestTrue(TEXT("Decay progress was banked before saving"), SavedDecayProgress > 0.0f);

	TestTrue(TEXT("The file was loaded into a newly allocated target, not the one it was saved from"),
		LoadedTarget != SavedTarget);

	TestEqual(TEXT("Every texel of the loaded target is bit-identical to the saved one"), Differing, 0);

	TestEqual(TEXT("The banked decay progress comes back with the texels"),
		Subsystem->GetDecayProgressInHalfLives(),
		SavedDecayProgress);

	TestTrue(TEXT("The file is smaller than the raw texels"), FileBytes > 0 && FileBytes < RawBytes);

	IFileManager::Get().Delete(*FilePath);
	Subsystem->ResetSurface();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceStateRejectsAFileSavedForAnotherRibbon,
	"RacetrackSurface.State.RejectsAFileSavedForAnotherRibbon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceStateRejectsAFileSavedForAnotherRibbon::RunTest(const FString& Parameters)
{
	using namespace RacetrackSurfaceStateTest;

	if (!FApp::CanEverRender())
	{
		AddWarning(TEXT("Skipped: build cannot render."));
		return true;
	}

	UWorld* World = nullptr;
	USurfaceAccumulationSubsystem* Subsystem = FindInitialisedSubsystem(*this, World);

	if (Subsystem == nullptr)
	{
		return false;
	}

	const TStrongObjectPtr<USplineComponent> Circuit = RacetrackSurfaceTest::MakeTestCircuit();
	const FSurfaceTrackRibbon SavedRibbon = RacetrackSurfaceTest::MakeTestRibbon(*Circuit);
	const FString FilePath = StateFilePath(TEXT("RejectsAFileSavedForAnotherRibbon"));
	const FString MissingPath = StateFilePath(TEXT("RejectsAFileSavedForAnotherRibbonMissing"));

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), SavedRibbon);
	Subsystem->ResetSurface();
	LayRubberBesideALockup(*Subsystem, -0.3, 0.3);

	TestTrue(TEXT("The surface on the first ribbon saves to a file"), Subsystem->SaveSurfaceState(FilePath));

	FSurfaceTrackRibbon LiveRibbon = SavedRibbon;
	LiveRibbon.TrackWidth = SavedRibbon.TrackWidth * 0.5;

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), LiveRibbon);
	Subsystem->ResetSurface();
	LayRubberBesideALockup(*Subsystem, 0.5, -0.5);
	BankDecayProgressWithoutAPass(*Subsystem);

	const float LiveDecayProgress = Subsystem->GetDecayProgressInHalfLives();

	TArray<FLinearColor> Before;

	if (!ReadWholeTarget(World, *Subsystem, Before))
	{
		AddError(TEXT("Could not read the live target back before loading."));
		IFileManager::Get().Delete(*FilePath);
		Subsystem->ResetSurface();
		return false;
	}

	IFileManager::Get().Delete(*MissingPath);

	AddExpectedMessagePlain(TEXT("so it is not loaded"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("No surface state file could be read"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("Could not load a surface state"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	TestFalse(TEXT("A file saved for a wider ribbon is not loaded"), Subsystem->LoadSurfaceState(FilePath));
	TestFalse(TEXT("A missing file is not loaded"), Subsystem->LoadSurfaceState(MissingPath));

	TArray<FLinearColor> After;

	if (!ReadWholeTarget(World, *Subsystem, After))
	{
		AddError(TEXT("Could not read the live target back after the rejected loads."));
		IFileManager::Get().Delete(*FilePath);
		Subsystem->ResetSurface();
		return false;
	}

	const int32 Differing = CountTexelsDiffering(Before, After);

	AddInfo(FString::Printf(
		TEXT("A file saved for a %d x %d atlas was refused by a %d x %d one. Afterwards %d texels differ, and the decay ")
		TEXT("progress is %.9f against %.9f before"),
		SavedRibbon.AtlasWidth(),
		SavedRibbon.AtlasHeight(),
		LiveRibbon.AtlasWidth(),
		LiveRibbon.AtlasHeight(),
		Differing,
		Subsystem->GetDecayProgressInHalfLives(),
		LiveDecayProgress));

	TestEqual(TEXT("The rejected loads leave every texel of the live surface as it was"), Differing, 0);

	TestEqual(TEXT("The rejected loads leave the banked decay progress as it was"),
		Subsystem->GetDecayProgressInHalfLives(),
		LiveDecayProgress);

	IFileManager::Get().Delete(*FilePath);
	Subsystem->ResetSurface();

	return true;
}

#endif
