// Copyright 2026 Balázs Fábián. All Rights Reserved.

#include "SurfaceQueryReadback.h"

#include "Engine/TextureRenderTarget2D.h"
#include "HAL/IConsoleManager.h"
#include "Math/Float16Color.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "SurfaceAccumulationSubsystem.h"
#include "TextureResource.h"

namespace
{
	TAutoConsoleVariable<int32> CVarQueryReadbackRingDepth(
		TEXT("r.RacetrackSurface.QueryReadbackRingDepth"),
		4,
		TEXT("How many gather readbacks may be in flight at once. Four is what Nanite streaming, Lumen ")
		TEXT("surface cache feedback and distance field streaming each independently settled on, and no ")
		TEXT("engine constant ties them together, so it is convention rather than a derived number. Too ")
		TEXT("few drops submissions while the GPU is behind; too many spends staging memory and returns ")
		TEXT("staler answers than necessary."),
		ECVF_Default);
}

int32 FSurfaceQueryReadback::GetConfiguredRingDepth()
{
	return FMath::Clamp(CVarQueryReadbackRingDepth.GetValueOnGameThread(), 1, 16);
}

FSurfaceQueryReadback::FSurfaceQueryReadback(int32 InRingDepth)
	: RingDepth(FMath::Clamp(InRingDepth, 1, 16))
{
	Ring.SetNum(RingDepth);
}

FSurfaceQueryReadback::~FSurfaceQueryReadback() = default;

void FSurfaceQueryReadback::Submit(UTextureRenderTarget2D* GatherTarget, int32 SlotCount, uint64 SubmittedOnFrame)
{
	if (GatherTarget == nullptr || SlotCount <= 0)
	{
		return;
	}

	FTextureRenderTargetResource* Resource = GatherTarget->GameThread_GetRenderTargetResource();

	if (Resource == nullptr)
	{
		return;
	}

	TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Self = AsShared();

	ENQUEUE_RENDER_COMMAND(RacetrackSurfaceQuerySubmit)(
		[Self, Resource, SlotCount, SubmittedOnFrame](FRHICommandListImmediate& RHICmdList)
		{
			Self->DrainOnRenderThread();
			Self->EnqueueOnRenderThread(RHICmdList, Resource, SlotCount, SubmittedOnFrame);
		});
}

void FSurfaceQueryReadback::Drain()
{
	TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Self = AsShared();

	ENQUEUE_RENDER_COMMAND(RacetrackSurfaceQueryDrain)(
		[Self](FRHICommandListImmediate&)
		{
			Self->DrainOnRenderThread();
		});
}

void FSurfaceQueryReadback::Invalidate()
{
	Epoch.Increment();

	{
		FScopeLock Lock(&ResolvedSection);

		ResolvedSlots.Reset();
		ResolvedFrame = 0;
		bHasResolved = false;
	}

}

void FSurfaceQueryReadback::RebuildForANewSourceSize()
{
	Invalidate();

	StagingRebuilds.Increment();

	TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Self = AsShared();

	ENQUEUE_RENDER_COMMAND(RacetrackSurfaceQueryReadbackRebuild)(
		[Self](FRHICommandListImmediate&)
		{
			Self->Ring.Reset();
			Self->Ring.SetNum(Self->RingDepth);
		});
}

void FSurfaceQueryReadback::ReleaseOnRenderThread()
{
	TSharedRef<FSurfaceQueryReadback, ESPMode::ThreadSafe> Self = AsShared();

	ENQUEUE_RENDER_COMMAND(RacetrackSurfaceQueryReadbackRelease)(
		[Self](FRHICommandListImmediate&)
		{
			Self->Ring.Empty();
		});
}

bool FSurfaceQueryReadback::CopyResolved(TArray<FLinearColor>& OutSlots, uint64& OutSubmittedOnFrame) const
{
	FScopeLock Lock(&ResolvedSection);

	if (!bHasResolved)
	{
		return false;
	}

	OutSlots = ResolvedSlots;
	OutSubmittedOnFrame = ResolvedFrame;

	return true;
}

void FSurfaceQueryReadback::DrainOnRenderThread()
{
	const int32 CurrentEpoch = Epoch.GetValue();

	int32 Newest = INDEX_NONE;

	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		FRingSlot& Slot = Ring[Index];

		if (!Slot.bInFlight || !Slot.Readback.IsValid() || !Slot.Readback->IsReady())
		{
			continue;
		}

		Slot.bInFlight = false;

		if (Slot.Epoch != CurrentEpoch)
		{
			ResolvesDiscardedFromAnOldEpoch.Increment();
			continue;
		}

		if (Newest == INDEX_NONE || Slot.SubmittedOnFrame > Ring[Newest].SubmittedOnFrame)
		{
			Newest = Index;
		}
	}

	if (Newest == INDEX_NONE)
	{
		return;
	}

	FRingSlot& Freshest = Ring[Newest];

	int32 RowPitchInPixels = 0;
	int32 BufferHeight = 0;

	const FFloat16Color* Pixels =
		static_cast<const FFloat16Color*>(Freshest.Readback->Lock(RowPitchInPixels, &BufferHeight));

	if (Pixels == nullptr)
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("A query gather readback reported ready and then mapped to nothing, so the surface "
				"query has no values for the frame it submitted"));

		return;
	}

	{
		FScopeLock Lock(&ResolvedSection);

		ResolvedSlots.SetNum(Freshest.SlotCount);

		for (int32 Slot = 0; Slot < Freshest.SlotCount; ++Slot)
		{
			ResolvedSlots[Slot] = Pixels[Slot].GetFloats();
		}

		ResolvedFrame = Freshest.SubmittedOnFrame;
		bHasResolved = true;
	}

	Freshest.Readback->Unlock();

	ResolveCount.Increment();
}

void FSurfaceQueryReadback::EnqueueOnRenderThread(
	FRHICommandListImmediate& RHICmdList,
	FTextureRenderTargetResource* Resource,
	int32 SlotCount,
	uint64 SubmittedOnFrame)
{
	int32 Free = INDEX_NONE;

	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		if (!Ring[Index].bInFlight)
		{
			Free = Index;
			break;
		}
	}

	if (Free == INDEX_NONE)
	{
		SubmissionsDroppedWhileFull.Increment();
		return;
	}

	FRHITexture* Texture = Resource->GetRenderTargetTexture();

	if (Texture == nullptr)
	{
		SubmissionsDroppedWhileFull.Increment();
		return;
	}

	FRingSlot& Slot = Ring[Free];

	if (!Slot.Readback.IsValid())
	{
		Slot.Readback = MakeUnique<FRHIGPUTextureReadback>(FName(TEXT("RacetrackSurfaceQueryGather")));
	}

	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));

	Slot.Readback->EnqueueCopy(RHICmdList, Texture, FResolveRect());

	RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));

	Slot.SubmittedOnFrame = SubmittedOnFrame;
	Slot.SlotCount = SlotCount;
	Slot.Epoch = Epoch.GetValue();
	Slot.bInFlight = true;

	SubmissionsAccepted.Increment();
}
