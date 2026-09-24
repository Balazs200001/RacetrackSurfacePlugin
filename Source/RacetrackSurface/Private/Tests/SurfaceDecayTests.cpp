#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;
using RacetrackSurfaceTest::MakeSquareTestRibbon;

namespace
{
	class FScopedDecayRate
	{
	public:
		explicit FScopedDecayRate(float Rate)
			: Variable(IConsoleManager::Get().FindConsoleVariable(
				RacetrackSurfaceMapping::DecayRateVariableName))
			, RateToRestore(Variable != nullptr ? Variable->GetFloat() : 0.0f)
		{
			if (Variable != nullptr)
			{
				Variable->Set(Rate);
			}
		}

		~FScopedDecayRate()
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

	USurfaceAccumulationSubsystem* PrepareSubsystemWithASmallBuffer(UWorld* World)
	{
		USurfaceAccumulationSubsystem* Subsystem =
			World != nullptr ? World->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

		if (Subsystem == nullptr)
		{
			return nullptr;
		}

		Subsystem->ConfigureTrackRibbon(nullptr, MakeSquareTestRibbon(256));
		Subsystem->ResetSurface();

		return Subsystem->GetOrCreateAccumulationTarget() != nullptr ? Subsystem : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayRateOfZeroNeverRunsThePass,
	"RacetrackSurface.Decay.RateOfZeroNeverRunsThePass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayRateOfZeroNeverRunsThePass::RunTest(const FString& Parameters)
{
	USurfaceAccumulationSubsystem* Subsystem = PrepareSubsystemWithASmallBuffer(FindRenderableWorld());

	if (Subsystem == nullptr)
	{
		AddError(TEXT("No world, subsystem or accumulation target available."));
		return false;
	}

	const FScopedDecayRate Rate(0.0f);

	if (!Rate.IsBound())
	{
		AddError(TEXT("The decay rate console variable is not registered."));
		return false;
	}

	constexpr float TickSeconds = 1.0f;
	constexpr int32 TickCount = 3600;

	for (int32 Tick = 0; Tick < TickCount; ++Tick)
	{
		Subsystem->Tick(TickSeconds);
	}

	AddInfo(FString::Printf(
		TEXT("Ticked %.0f s of track time at a rate of zero and ran %d decay passes, holding %.6f "
			"half-lives of progress"),
		TickCount * TickSeconds,
		Subsystem->GetDecayPassCount(),
		Subsystem->GetDecayProgressInHalfLives()));

	TestEqual(
		TEXT("An hour of track time at a rate of zero runs the decay pass no times at all, so every "
			"measurement taken before decay existed still describes this configuration"),
		Subsystem->GetDecayPassCount(),
		0);

	TestEqual(
		TEXT("A rate of zero banks no progress either, so raising the rate later starts from now rather "
			"than discharging everything that was skipped"),
		Subsystem->GetDecayProgressInHalfLives(),
		0.0f);

	Subsystem->ResetSurface();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayBanksProgressWithoutLosingAny,
	"RacetrackSurface.Decay.BanksProgressWithoutLosingAny",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayBanksProgressWithoutLosingAny::RunTest(const FString& Parameters)
{
	USurfaceAccumulationSubsystem* Subsystem = PrepareSubsystemWithASmallBuffer(FindRenderableWorld());

	if (Subsystem == nullptr)
	{
		AddError(TEXT("No world, subsystem or accumulation target available."));
		return false;
	}

	constexpr float Rate = 4.0f;
	const FScopedDecayRate ScopedRate(Rate);

	if (!ScopedRate.IsBound())
	{
		AddError(TEXT("The decay rate console variable is not registered."));
		return false;
	}

	const float Step = 1.0f / USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife();
	const float TickSeconds =
		Subsystem->GetDecayHalfLifeSeconds()
		/ (Rate * USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife() * 4.0f);

	constexpr int32 TickCount = 100;

	for (int32 Tick = 0; Tick < TickCount; ++Tick)
	{
		Subsystem->Tick(TickSeconds);
	}

	const float TotalProgress = RacetrackSurfaceMapping::ElapsedToDecayProgress(
		TickSeconds * TickCount,
		Rate,
		Subsystem->GetDecayHalfLifeSeconds());

	const float Remaining = Subsystem->GetDecayProgressInHalfLives();
	const int32 Passes = Subsystem->GetDecayPassCount();
	const int32 ExpectedPasses = FMath::FloorToInt32(TotalProgress / Step);

	AddInfo(FString::Printf(
		TEXT("%d ticks of %.4f s at %.2fx banked %.6f half-lives, ran %d passes against %d expected, and "
			"holds %.6f of a %.6f step. Each pass retains %.6f"),
		TickCount,
		TickSeconds,
		Rate,
		TotalProgress,
		Passes,
		ExpectedPasses,
		Remaining,
		Step,
		RacetrackSurfaceMapping::DecayRetentionForProgress(Step)));

	TestTrue(
		TEXT("The accumulator never holds a whole step without spending it, so the pass cannot fall "
			"behind the clock"),
		Remaining < Step);

	TestTrue(TEXT("The accumulator never goes negative"), Remaining >= 0.0f);

	TestTrue(
		TEXT("The number of passes follows the banked progress rather than the number of ticks"),
		FMath::Abs(Passes - ExpectedPasses) <= 1);

	TestTrue(
		TEXT("Progress is spent rather than lost: what was banked, less what is still held, is at least "
			"what the passes that ran were entitled to"),
		TotalProgress - Remaining >= Passes * Step - UE_SMALL_NUMBER);

	Subsystem->ResetSurface();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayNeverAllocatesABufferOfItsOwn,
	"RacetrackSurface.Decay.NeverAllocatesABufferOfItsOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayNeverAllocatesABufferOfItsOwn::RunTest(const FString& Parameters)
{
	UWorld* World = FindRenderableWorld();

	USurfaceAccumulationSubsystem* Subsystem =
		World != nullptr ? World->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

	if (Subsystem == nullptr)
	{
		AddError(TEXT("No world or subsystem available."));
		return false;
	}

	USurfaceAccumulationBuffer* Buffer = Subsystem->GetBuffer();

	if (Buffer != nullptr)
	{
		Buffer->Release();
	}

	const FScopedDecayRate ScopedRate(20.0f);

	if (!ScopedRate.IsBound())
	{
		AddError(TEXT("The decay rate console variable is not registered."));
		return false;
	}

	Subsystem->ResetSurface();

	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Subsystem->Tick(1.0f);
	}

	const USurfaceAccumulationBuffer* AfterTicking = Subsystem->GetBuffer();

	AddInfo(FString::Printf(
		TEXT("Ticked 600 s at 20x with nothing painted and ran %d decay passes against a buffer that is %s"),
		Subsystem->GetDecayPassCount(),
		AfterTicking != nullptr && AfterTicking->IsAllocated() ? TEXT("allocated") : TEXT("unallocated")));

	TestFalse(
		TEXT("Decay does not allocate an accumulation target of its own, because a surface nothing has "
			"painted on has nothing to fade and the target is over a hundred megabytes"),
		AfterTicking != nullptr && AfterTicking->IsAllocated());

	TestEqual(
		TEXT("No decay pass runs while there is no buffer to run it against"),
		Subsystem->GetDecayPassCount(),
		0);

	Subsystem->ResetSurface();

	return true;
}

#endif
