// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceTest
{
	constexpr int32 CpuTimingSampleCount = 64;

	inline void SampleCpuWork(TFunctionRef<void()> Work, double& OutMedian, double& OutMinimum)
	{
		Work();

		TArray<double> Samples;
		Samples.Reserve(CpuTimingSampleCount);

		for (int32 Index = 0; Index < CpuTimingSampleCount; ++Index)
		{
			const double Start = FPlatformTime::Seconds();
			Work();
			Samples.Add((FPlatformTime::Seconds() - Start) * 1000.0);
		}

		Samples.Sort();

		OutMinimum = Samples[0];
		OutMedian = Samples[Samples.Num() / 2];
	}
}

#endif
