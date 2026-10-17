#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopedCVar.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"
#include "UObject/StrongObjectPtr.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayConsoleThenCodeSetsBothTakeEffect,
	"RacetrackSurface.Decay.ConsoleThenCodeSetsBothTakeEffect",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayConsoleThenCodeSetsBothTakeEffect::RunTest(const FString& Parameters)
{
	UWorld* World = FindRenderableWorld();

	USurfaceAccumulationSubsystem* Subsystem =
		World != nullptr ? World->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

	IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(RacetrackSurfaceMapping::DecayRateVariableName);

	if (Subsystem == nullptr || Variable == nullptr)
	{
		AddError(TEXT("No world, subsystem or decay rate console variable available."));
		return false;
	}

	constexpr float ConsoleRate = 3.0f;
	constexpr float CodeRate = 7.0f;

	const float RateToRestore = Subsystem->GetDecayRate();

	AddExpectedMessagePlain(
		TEXT("was ignored as it is lower priority"),
		ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains,
		1);

	Variable->Set(ConsoleRate, ECVF_SetByConsole);
	const float RateAfterConsole = Subsystem->GetDecayRate();

	Variable->Set(CodeRate, ECVF_SetByCode);
	const float VariableAfterItsOwnCodeWrite = Variable->GetFloat();

	Subsystem->SetDecayRate(CodeRate);
	const float RateAfterCode = Subsystem->GetDecayRate();

	Variable->Set(ConsoleRate, ECVF_SetByConsole);
	const float RateAfterConsoleAgain = Subsystem->GetDecayRate();

	Variable->Unset(ECVF_SetByConsole);
	Subsystem->SetDecayRate(RateToRestore);

	AddInfo(FString::Printf(
		TEXT("Console %.1f gave the subsystem %.1f. A code write of %.1f to the variable left it at %.1f. "
			"The setter at %.1f gave %.1f. Console %.1f again, the variable's value unchanged, gave %.1f"),
		ConsoleRate,
		RateAfterConsole,
		CodeRate,
		VariableAfterItsOwnCodeWrite,
		CodeRate,
		RateAfterCode,
		ConsoleRate,
		RateAfterConsoleAgain));

	TestEqual(TEXT("A console write reaches the subsystem"), RateAfterConsole, ConsoleRate);

	TestEqual(
		TEXT("A code write to the console variable after a console write is ignored, which is the lockout a "
			"setter that wrote the variable would hit"),
		VariableAfterItsOwnCodeWrite,
		ConsoleRate);

	TestEqual(TEXT("The subsystem's setter takes effect after a console write"), RateAfterCode, CodeRate);

	TestEqual(
		TEXT("A console write takes effect after a code write, even one repeating the variable's own value"),
		RateAfterConsoleAgain,
		ConsoleRate);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayRateSetFromCodeDrivesThePass,
	"RacetrackSurface.Decay.RateSetFromCodeDrivesThePass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayRateSetFromCodeDrivesThePass::RunTest(const FString& Parameters)
{
	USurfaceAccumulationSubsystem* Subsystem = PrepareSubsystemWithASmallBuffer(FindRenderableWorld());

	if (Subsystem == nullptr)
	{
		AddError(TEXT("No world, subsystem or accumulation target available."));
		return false;
	}

	const float RateToRestore = Subsystem->GetDecayRate();

	constexpr float Rate = 4.0f;
	Subsystem->SetDecayRate(Rate);

	const float Step = 1.0f / USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife();
	const float TickSeconds =
		Subsystem->GetDecayHalfLifeSeconds()
		/ (Rate * USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife() * 4.0f);

	constexpr int32 TickCount = 101;

	for (int32 Tick = 0; Tick < TickCount; ++Tick)
	{
		Subsystem->Tick(TickSeconds);
	}

	const float TotalProgress = RacetrackSurfaceMapping::ElapsedToDecayProgress(
		TickSeconds * TickCount,
		Rate,
		Subsystem->GetDecayHalfLifeSeconds());

	const int32 PassesAtRate = Subsystem->GetDecayPassCount();
	const int32 ExpectedPasses = FMath::FloorToInt32(TotalProgress / Step);
	const float BankedBeforeStopping = Subsystem->GetDecayProgressInHalfLives();

	Subsystem->SetDecayRate(0.0f);
	const float BankedAfterStopping = Subsystem->GetDecayProgressInHalfLives();

	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Subsystem->Tick(1.0f);
	}

	const int32 PassesWhileStopped = Subsystem->GetDecayPassCount() - PassesAtRate;

	Subsystem->SetDecayRate(-2.0f);
	const float RateAfterANegativeSet = Subsystem->GetDecayRate();

	Subsystem->SetDecayRate(RateToRestore);
	Subsystem->ResetSurface();

	AddInfo(FString::Printf(
		TEXT("Set from code to %.1fx: %d passes against %d expected, %.6f half-lives banked. Set to 0: "
			"%.6f banked, %d passes over 600 s. Set to -2: reads %.1f"),
		Rate,
		PassesAtRate,
		ExpectedPasses,
		BankedBeforeStopping,
		BankedAfterStopping,
		PassesWhileStopped,
		RateAfterANegativeSet));

	TestTrue(
		TEXT("A rate set from code drives the pass, as many passes as the banked progress pays for"),
		PassesAtRate > 0 && FMath::Abs(PassesAtRate - ExpectedPasses) <= 1);

	TestTrue(
		TEXT("Some progress was banked when the rate was stopped, so clearing it is observable"),
		BankedBeforeStopping > 0.0f);

	TestEqual(TEXT("Setting the rate to 0 clears the banked progress at once"), BankedAfterStopping, 0.0f);

	TestEqual(TEXT("No pass runs while the rate set from code is 0"), PassesWhileStopped, 0);

	TestEqual(TEXT("A negative rate is held as 0"), RateAfterANegativeSet, 0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfaceDecayRateIsHeldPerSubsystem,
	"RacetrackSurface.Decay.RateIsHeldPerSubsystem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfaceDecayRateIsHeldPerSubsystem::RunTest(const FString& Parameters)
{
	UWorld* World = FindRenderableWorld();

	USurfaceAccumulationSubsystem* InTheWorld =
		World != nullptr ? World->GetSubsystem<USurfaceAccumulationSubsystem>() : nullptr;

	if (InTheWorld == nullptr)
	{
		AddError(TEXT("No world or subsystem available."));
		return false;
	}

	const TStrongObjectPtr<USurfaceAccumulationSubsystem> Another(NewObject<USurfaceAccumulationSubsystem>());

	const float RateToRestore = InTheWorld->GetDecayRate();

	InTheWorld->SetDecayRate(2.0f);
	Another->SetDecayRate(5.0f);

	const float InTheWorldFromCode = InTheWorld->GetDecayRate();
	const float AnotherFromCode = Another->GetDecayRate();

	float InTheWorldFromTheVariable = 0.0f;
	float AnotherFromTheVariable = 0.0f;

	{
		const FScopedCVar<float> VariableRate(RacetrackSurfaceMapping::DecayRateVariableName, 9.0f);

		InTheWorldFromTheVariable = InTheWorld->GetDecayRate();
		AnotherFromTheVariable = Another->GetDecayRate();
	}

	InTheWorld->SetDecayRate(RateToRestore);

	AddInfo(FString::Printf(
		TEXT("From code: %.1f and %.1f. From the console variable at 9: %.1f and %.1f"),
		InTheWorldFromCode,
		AnotherFromCode,
		InTheWorldFromTheVariable,
		AnotherFromTheVariable));

	TestEqual(TEXT("A code write sets the rate of the subsystem it is made on"), InTheWorldFromCode, 2.0f);

	TestEqual(TEXT("A code write leaves every other subsystem's rate alone"), AnotherFromCode, 5.0f);

	TestEqual(TEXT("The console variable reaches the world's subsystem"), InTheWorldFromTheVariable, 9.0f);

	TestEqual(TEXT("The console variable reaches every other live subsystem too"), AnotherFromTheVariable, 9.0f);

	return true;
}

#endif
