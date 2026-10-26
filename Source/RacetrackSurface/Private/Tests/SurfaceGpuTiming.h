// Copyright 2026 Balázs Fábián. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResources.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace RacetrackSurfaceTest
{
	constexpr int32 TimingSampleCount = 64;

	struct FTimestampPair
	{
		FRenderQueryRHIRef Start;
		FRenderQueryRHIRef End;
		uint64 StartMicroseconds = 0;
		uint64 EndMicroseconds = 0;
		bool bResolved = false;
	};

	inline TSharedRef<FTimestampPair> CreateTimestampPair()
	{
		TSharedRef<FTimestampPair> Queries = MakeShared<FTimestampPair>();

		ENQUEUE_RENDER_COMMAND(RacetrackSurfaceCreateTimestamps)(
			[Queries](FRHICommandListImmediate&)
			{
				Queries->Start = RHICreateRenderQuery(RQT_AbsoluteTime);
				Queries->End = RHICreateRenderQuery(RQT_AbsoluteTime);
			});

		FlushRenderingCommands();

		return Queries;
	}

	inline bool TimeGpuWork(
		const TSharedRef<FTimestampPair>& Queries,
		TFunctionRef<void()> Work,
		double& OutMilliseconds)
	{
		if (!Queries->Start.IsValid() || !Queries->End.IsValid())
		{
			return false;
		}

		ENQUEUE_RENDER_COMMAND(RacetrackSurfaceBeginTiming)(
			[Queries](FRHICommandListImmediate& RHICmdList)
			{
				Queries->bResolved = false;
				RHICmdList.EndRenderQuery(Queries->Start);
			});

		Work();

		ENQUEUE_RENDER_COMMAND(RacetrackSurfaceEndTiming)(
			[Queries](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndRenderQuery(Queries->End);
				RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

				Queries->bResolved =
					RHIGetRenderQueryResult(Queries->Start, Queries->StartMicroseconds, true)
					&& RHIGetRenderQueryResult(Queries->End, Queries->EndMicroseconds, true);
			});

		FlushRenderingCommands();

		if (!Queries->bResolved || Queries->EndMicroseconds < Queries->StartMicroseconds)
		{
			return false;
		}

		OutMilliseconds = static_cast<double>(Queries->EndMicroseconds - Queries->StartMicroseconds) / 1000.0;
		return true;
	}

	inline bool SampleGpuWork(TFunctionRef<void()> Work, double& OutMedian, double& OutMinimum)
	{
		const TSharedRef<FTimestampPair> Queries = CreateTimestampPair();

		TArray<double> Samples;
		Samples.Reserve(TimingSampleCount);

		for (int32 Index = 0; Index < TimingSampleCount; ++Index)
		{
			double Milliseconds = 0.0;
			if (!TimeGpuWork(Queries, Work, Milliseconds))
			{
				return false;
			}

			Samples.Add(Milliseconds);
		}

		Samples.Sort();

		OutMinimum = Samples[0];
		OutMedian = Samples[Samples.Num() / 2];

		return true;
	}
}

#endif
