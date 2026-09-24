#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FRHICommandListImmediate;
class FRHIGPUTextureReadback;
class FTextureRenderTargetResource;
class UTextureRenderTarget2D;

class FSurfaceQueryReadback : public TSharedFromThis<FSurfaceQueryReadback, ESPMode::ThreadSafe>
{
public:
	static int32 GetConfiguredRingDepth();

	explicit FSurfaceQueryReadback(int32 InRingDepth);

	~FSurfaceQueryReadback();

	void Submit(UTextureRenderTarget2D* GatherTarget, int32 SlotCount, uint64 SubmittedOnFrame);

	void Drain();

	void Invalidate();

	bool CopyResolved(TArray<FLinearColor>& OutSlots, uint64& OutSubmittedOnFrame) const;

	void ReleaseOnRenderThread();

	int32 GetEpoch() const
	{
		return Epoch.GetValue();
	}

	int32 GetResolvesDiscardedFromAnOldEpoch() const
	{
		return ResolvesDiscardedFromAnOldEpoch.GetValue();
	}

	int32 GetRingDepth() const
	{
		return RingDepth;
	}

	int32 GetSubmissionsAccepted() const
	{
		return SubmissionsAccepted.GetValue();
	}

	int32 GetSubmissionsDroppedWhileFull() const
	{
		return SubmissionsDroppedWhileFull.GetValue();
	}

	int32 GetResolveCount() const
	{
		return ResolveCount.GetValue();
	}

private:
	struct FRingSlot
	{
		TUniquePtr<FRHIGPUTextureReadback> Readback;

		uint64 SubmittedOnFrame = 0;

		int32 SlotCount = 0;

		int32 Epoch = 0;

		bool bInFlight = false;
	};

	void DrainOnRenderThread();

	void EnqueueOnRenderThread(
		FRHICommandListImmediate& RHICmdList,
		FTextureRenderTargetResource* Resource,
		int32 SlotCount,
		uint64 SubmittedOnFrame);

	int32 RingDepth = 0;

	TArray<FRingSlot> Ring;

	mutable FCriticalSection ResolvedSection;

	TArray<FLinearColor> ResolvedSlots;

	uint64 ResolvedFrame = 0;

	bool bHasResolved = false;

	FThreadSafeCounter Epoch;

	FThreadSafeCounter SubmissionsAccepted;

	FThreadSafeCounter SubmissionsDroppedWhileFull;

	FThreadSafeCounter ResolveCount;

	FThreadSafeCounter ResolvesDiscardedFromAnOldEpoch;
};
