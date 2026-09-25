#include "SurfaceAccumulationSubsystem.h"

#include "Components/SplineComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceQueryGather.h"
#include "SurfaceQueryReadback.h"
#include "SurfaceTrackSpace.h"

DEFINE_LOG_CATEGORY(LogRacetrackSurface);

namespace
{
	float PlayingWorldTimeDilation()
	{
		if (GEngine == nullptr)
		{
			return 1.0f;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			const bool bPlaying = Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE;

			if (bPlaying && Context.World() != nullptr && Context.World()->GetWorldSettings() != nullptr)
			{
				return Context.World()->GetWorldSettings()->GetEffectiveTimeDilation();
			}
		}

		return 1.0f;
	}

	void AnnounceOnScreen(FName MessageName, const FString& Text)
	{
		if (GEngine == nullptr)
		{
			return;
		}

		const float MessageSeconds = 6.0f * FMath::Max(PlayingWorldTimeDilation(), UE_SMALL_NUMBER);

		GEngine->AddOnScreenDebugMessage(
			static_cast<uint64>(GetTypeHash(MessageName)),
			MessageSeconds,
			FColor::White,
			Text);
	}

	void AnnounceDepositionScale(IConsoleVariable* Variable)
	{
		AnnounceOnScreen(
			TEXT("RacetrackSurface.DepositionScale"),
			FString::Printf(TEXT("Deposition now %.2fx the derived rate"), Variable->GetFloat()));
	}

	void AnnounceDecayRate(IConsoleVariable* Variable)
	{
		const float Rate = Variable->GetFloat();

		if (Rate <= 0.0f)
		{
			AnnounceOnScreen(
				TEXT("RacetrackSurface.DecayRate"),
				TEXT("Decay now off, so nothing fades and the surface is the accumulation baseline"));
			return;
		}

		AnnounceOnScreen(
			TEXT("RacetrackSurface.DecayRate"),
			FString::Printf(TEXT("Decay now %.2fx the calibrated fade"), Rate));
	}

	TAutoConsoleVariable<float> CVarDepositionScale(
		TEXT("r.RacetrackSurface.DepositionScale"),
		1.0f,
		TEXT("Multiplies how much exposure one contact deposits. 1 is the derived rate, at which ")
		TEXT("buildup approaches saturation over roughly 500 contacts. Raise it to watch a session's ")
		TEXT("worth of rubber accumulate in a few laps without time compression."),
		FConsoleVariableDelegate::CreateStatic(&AnnounceDepositionScale),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarDecayRate(
		TEXT("r.RacetrackSurface.DecayRate"),
		0.0f,
		TEXT("Multiplies the decay rate constant derived from the half-life in SurfaceAccumulationMapping.h. ")
		TEXT("0 skips the decay pass entirely, which is the state every measurement before C10 was taken in. ")
		TEXT("1 is the derived half-life, measured against dilated world time so a lap loses the same rubber ")
		TEXT("whatever speed you watch it at."),
		FConsoleVariableDelegate::CreateStatic(&AnnounceDecayRate),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarMarbleSweepRetention(
		TEXT("r.RacetrackSurface.MarbleSweepRetention"),
		RacetrackSurfaceMapping::MarbleSweepRetention,
		TEXT("The fraction of the marbles under a tyre that survive one pass over them. 1 sweeps nothing ")
		TEXT("and leaves marbles lying on the racing line where cars actually drive, which is wrong; 0 ")
		TEXT("scrubs a texel clean the first time any wheel crosses it. The racing line ends up clear ")
		TEXT("because that is where the traffic is, not because anything reserves a gap for it."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarQueryContactPatchWidth(
		TEXT("r.RacetrackSurface.QueryContactPatchWidth"),
		static_cast<float>(RacetrackSurfaceQuery::DefaultContactPatchWidth),
		TEXT("How wide a patch, in centimetres, a surface query averages over. 20 is a tyre contact ")
		TEXT("patch and is 6.4 texels at the production density, so the query integrates what a tyre ")
		TEXT("stands on rather than one texel of noise. Setting this to one texel's width makes the ")
		TEXT("query compute exactly what the display material computes at SurfaceBlurTexels 1, which ")
		TEXT("is how the two reads are checked against each other."),
		ECVF_Default);

	constexpr int32 QuerySubmissionStampCount = 32;

	TAutoConsoleVariable<float> CVarDecayStepsPerHalfLife(
		TEXT("r.RacetrackSurface.DecayStepsPerHalfLife"),
		RacetrackSurfaceMapping::DecayStepsPerHalfLife,
		TEXT("How many decay passes make up one half-life. The retention each pass applies is the same ")
		TEXT("whatever the rate, so this trades pass frequency against how finely the fade is quantised. ")
		TEXT("Raising it far past the default makes each retention too close to 1 to survive half-float ")
		TEXT("rounding, at which point the pass costs its full price and changes nothing."),
		ECVF_Default);
}

float USurfaceAccumulationSubsystem::GetDepositionScale()
{
	return CVarDepositionScale.GetValueOnGameThread();
}

float USurfaceAccumulationSubsystem::GetDecayRate()
{
	return CVarDecayRate.GetValueOnGameThread();
}

float USurfaceAccumulationSubsystem::GetMarbleSweepRetention()
{
	return FMath::Clamp(CVarMarbleSweepRetention.GetValueOnGameThread(), 0.0f, 1.0f);
}

float USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife()
{
	return FMath::Max(CVarDecayStepsPerHalfLife.GetValueOnGameThread(), 1.0f);
}

float USurfaceAccumulationSubsystem::GetQueryContactPatchWidth()
{
	return FMath::Max(CVarQueryContactPatchWidth.GetValueOnGameThread(), 0.0f);
}

void USurfaceAccumulationSubsystem::SetDecayHalfLifeSeconds(float Seconds)
{
	DecayHalfLifeSeconds = FMath::Max(Seconds, UE_SMALL_NUMBER);

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Decay half-life set to %.1f s of track time, so one step of %.0f retains %.6f"),
		DecayHalfLifeSeconds,
		GetDecayStepsPerHalfLife(),
		RacetrackSurfaceMapping::DecayRetentionForProgress(1.0f / GetDecayStepsPerHalfLife()));
}

void USurfaceAccumulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	EnsureQueryReadback();

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Surface ribbon awaiting a centreline, defaults %.1f cm of track at %.1f cm wide, %.4f cm per texel"),
		Ribbon.TrackLength,
		Ribbon.TrackWidth,
		Ribbon.CentimetresPerTexel);
}

void USurfaceAccumulationSubsystem::Deinitialize()
{
	PendingContacts.Empty();
	StrokesResolvedThisFrame.Empty();
	Centreline.Reset();

	QuerySlots.Empty();
	ResolvedQuerySlots.Empty();
	QuerySubmissionStamps.Empty();

	if (QueryReadback.IsValid())
	{
		QueryReadback->ReleaseOnRenderThread();
		QueryReadback.Reset();
	}

	if (Buffer != nullptr)
	{
		Buffer->Release();
		Buffer = nullptr;
	}

	Super::Deinitialize();
}

bool USurfaceAccumulationSubsystem::EnsureBuffer()
{
	const bool bFirstAllocation = Buffer == nullptr;

	if (bFirstAllocation)
	{
		Buffer = NewObject<USurfaceAccumulationBuffer>(this);
	}

	if (Buffer == nullptr || !Buffer->Allocate(this, Ribbon))
	{
		return false;
	}

	if (bFirstAllocation)
	{
		PublishRibbonToMaterials();
	}

	return true;
}

UTextureRenderTarget2D* USurfaceAccumulationSubsystem::GetOrCreateAccumulationTarget()
{
	return EnsureBuffer() ? Buffer->GetTarget() : nullptr;
}

void USurfaceAccumulationSubsystem::PublishRibbonToMaterials()
{
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(
		nullptr,
		RacetrackSurfaceMapping::ParameterCollectionAssetPath);

	if (Collection == nullptr)
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("Parameter collection %s is missing, so materials cannot resolve the saturation constant"),
			RacetrackSurfaceMapping::ParameterCollectionAssetPath);
		return;
	}

	UKismetMaterialLibrary::SetScalarParameterValue(
		this,
		Collection,
		RacetrackSurfaceMapping::SaturationConstantParameterName,
		RacetrackSurfaceMapping::SaturationConstant);

	UKismetMaterialLibrary::SetScalarParameterValue(
		this,
		Collection,
		RacetrackSurfaceMapping::AccumulationRowCountParameterName,
		static_cast<float>(Ribbon.RowCount()));

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Published saturation constant %.3f and a row count of %d to %s"),
		RacetrackSurfaceMapping::SaturationConstant,
		Ribbon.RowCount(),
		*Collection->GetName());
}

void USurfaceAccumulationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	ApplyDecayWhenAStepHasElapsed(DeltaTime);
	FlushPendingContacts(DeltaTime);
	ResolveSurfaceQueries();
}

void USurfaceAccumulationSubsystem::ApplyDecayWhenAStepHasElapsed(float DeltaSeconds)
{
	const float Rate = GetDecayRate();

	if (Rate <= 0.0f)
	{
		DecayProgressInHalfLives = 0.0f;
		return;
	}

	DecayProgressInHalfLives +=
		RacetrackSurfaceMapping::ElapsedToDecayProgress(DeltaSeconds, Rate, DecayHalfLifeSeconds);

	if (DecayProgressInHalfLives < 1.0f / GetDecayStepsPerHalfLife())
	{
		return;
	}

	if (Buffer == nullptr || !Buffer->IsAllocated())
	{
		DecayProgressInHalfLives = 0.0f;
		return;
	}

	Buffer->ApplyDecay(this, RacetrackSurfaceMapping::DecayRetentionForProgress(DecayProgressInHalfLives));

	DecayProgressInHalfLives = 0.0f;
	++DecayPassCount;
}

int32 USurfaceAccumulationSubsystem::GetContactsDiscardedOutsideRibbon() const
{
	int32 Total = 0;

	for (const int32 Discarded : ContactsDiscardedOutsideRibbon)
	{
		Total += Discarded;
	}

	return Total;
}

int32 USurfaceAccumulationSubsystem::GetContactsDiscardedOutsideRibbonInChannel(
	EAccumulationChannel Channel) const
{
	const int32 Index = static_cast<int32>(Channel);

	return Index >= 0 && Index < AccumulationChannelCount
		? ContactsDiscardedOutsideRibbon[Index]
		: 0;
}

void USurfaceAccumulationSubsystem::RecordContactDiscardedOutsideRibbon(EAccumulationChannel Channel)
{
	const int32 Index = static_cast<int32>(Channel);

	if (Index >= 0 && Index < AccumulationChannelCount)
	{
		++ContactsDiscardedOutsideRibbon[Index];
	}
}

void USurfaceAccumulationSubsystem::ConfigureTrackRibbon(
	const USplineComponent* NewCentreline,
	const FSurfaceTrackRibbon& NewRibbon)
{
	Centreline = NewCentreline;
	Ribbon = NewRibbon;

	InvalidateQueryCache();

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Surface ribbon configured: %.1f cm of track at %.1f cm wide, %.4f cm per texel, ")
		TEXT("%d x %d texels folded into %d rows of %d, allocating %d x %d, %s"),
		Ribbon.TrackLength,
		Ribbon.TrackWidth,
		Ribbon.CentimetresPerTexel,
		Ribbon.AlongTrackTexels(),
		Ribbon.AcrossTrackTexels(),
		Ribbon.RowCount(),
		Ribbon.TexelsPerRow(),
		Ribbon.AtlasWidth(),
		Ribbon.AtlasHeight(),
		Ribbon.bClosedLoop ? TEXT("closed loop") : TEXT("open ended"));

	if (NewCentreline == nullptr)
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("Surface ribbon configured without a centreline, so world-space contacts cannot be placed"));
	}

	if (!Ribbon.FitsTheTextureCap())
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("Surface ribbon folds to %d x %d, which exceeds the %d texel limit in one dimension. ")
			TEXT("This ribbon cannot be allocated at %.4f cm per texel."),
			Ribbon.AtlasWidth(),
			Ribbon.AtlasHeight(),
			FSurfaceTrackRibbon::MaximumTexelsPerRow,
			Ribbon.CentimetresPerTexel);
	}

	PublishRibbonToMaterials();
}

bool USurfaceAccumulationSubsystem::RegisterSurfaceContact(const FSurfaceContact& Contact)
{
	const USplineComponent* Spline = Centreline.Get();

	if (Spline == nullptr)
	{
		++ContactsDiscardedWithoutCentreline;
		return false;
	}

	FSurfaceTrackContact TrackContact;

	const ETrackSpaceStatus Status = RacetrackSurfaceTrackSpace::WorldToTrack(
		*Spline,
		Ribbon.TrackWidth,
		Contact.WorldPosition,
		TrackContact.Coordinate);

	if (Status != ETrackSpaceStatus::Valid)
	{
		RecordContactDiscardedOutsideRibbon(Contact.Channel);
		return false;
	}

	RacetrackSurfaceTrackSpace::WorldVelocityToTrackRates(
		*Spline,
		Ribbon.TrackWidth,
		TrackContact.Coordinate,
		Contact.Velocity,
		TrackContact.CentimetresPerSecondAlongCentreline,
		TrackContact.LateralOffsetAlphaPerSecond);

	TrackContact.SlipAmount = Contact.SlipAmount;
	TrackContact.ContactWidth = Contact.ContactWidth;
	TrackContact.Channel = Contact.Channel;

	PendingContacts.Add(TrackContact);

	return true;
}

bool USurfaceAccumulationSubsystem::RegisterSurfaceContact(
	const FVector& WorldPosition,
	const FVector& Velocity,
	float SlipAmount,
	float ContactWidth,
	EAccumulationChannel Channel)
{
	FSurfaceContact Contact;
	Contact.WorldPosition = WorldPosition;
	Contact.Velocity = Velocity;
	Contact.SlipAmount = SlipAmount;
	Contact.ContactWidth = ContactWidth;
	Contact.Channel = Channel;

	return RegisterSurfaceContact(Contact);
}

bool USurfaceAccumulationSubsystem::RegisterSurfaceContactInTrackSpace(const FSurfaceTrackContact& Contact)
{
	if (!Ribbon.ContainsLateralOffset(Contact.Coordinate.LateralOffsetAlpha))
	{
		RecordContactDiscardedOutsideRibbon(Contact.Channel);
		return false;
	}

	PendingContacts.Add(Contact);

	return true;
}

bool USurfaceAccumulationSubsystem::RegisterSurfaceContactInTrackSpace(
	double DistanceAlongCentreline,
	double LateralOffsetAlpha,
	double CentimetresPerSecondAlongCentreline,
	float SlipAmount,
	float ContactWidth,
	EAccumulationChannel Channel)
{
	FSurfaceTrackContact Contact;
	Contact.Coordinate.DistanceAlongCentreline = DistanceAlongCentreline;
	Contact.Coordinate.LateralOffsetAlpha = LateralOffsetAlpha;
	Contact.CentimetresPerSecondAlongCentreline = CentimetresPerSecondAlongCentreline;
	Contact.SlipAmount = SlipAmount;
	Contact.ContactWidth = ContactWidth;
	Contact.Channel = Channel;

	return RegisterSurfaceContactInTrackSpace(Contact);
}

void USurfaceAccumulationSubsystem::EnsureQueryReadback()
{
	if (QueryReadback.IsValid())
	{
		return;
	}

	QueryReadback = MakeShared<FSurfaceQueryReadback, ESPMode::ThreadSafe>(
		FSurfaceQueryReadback::GetConfiguredRingDepth());

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Surface query readback ring built with %d slots"),
		QueryReadback->GetRingDepth());
}

void USurfaceAccumulationSubsystem::InvalidateQueryCache()
{
	ResolvedQuerySlots.Reset();
	QuerySubmissionStamps.Reset();
	ResolvedQueryFrame = 0;
	QueryFrameCounter = 0;
	ResolvedQueryWorldTimeSeconds = 0.0f;
	GatherTargetWidthInFlight = 0;
	bHasResolvedQuery = false;

	for (FSurfaceQuerySlot& Slot : QuerySlots)
	{
		Slot.FirstSubmittedFrame = TNumericLimits<uint64>::Max();
	}

	if (QueryReadback.IsValid())
	{
		QueryReadback->Invalidate();
	}
}

float USurfaceAccumulationSubsystem::GetWorldTimeSecondsForStaleness() const
{
	const UWorld* World = GetWorld();

	return World != nullptr ? World->GetTimeSeconds() : 0.0f;
}

FSurfaceQueryHandle USurfaceAccumulationSubsystem::AcquireSurfaceQuery()
{
	int32 SlotIndex = INDEX_NONE;

	for (int32 Index = 0; Index < QuerySlots.Num(); ++Index)
	{
		if (!QuerySlots[Index].bInUse)
		{
			SlotIndex = Index;
			break;
		}
	}

	if (SlotIndex == INDEX_NONE)
	{
		SlotIndex = QuerySlots.AddDefaulted();
	}

	FSurfaceQuerySlot& Slot = QuerySlots[SlotIndex];

	++Slot.Generation;
	Slot.bInUse = true;
	Slot.bPositioned = false;
	Slot.bOutsideRibbon = false;
	Slot.FirstSubmittedFrame = TNumericLimits<uint64>::Max();
	Slot.Coordinate = FSurfaceTrackCoordinate();

	FSurfaceQueryHandle Handle;
	Handle.SlotIndex = SlotIndex;
	Handle.Generation = Slot.Generation;

	return Handle;
}

void USurfaceAccumulationSubsystem::ReleaseSurfaceQuery(const FSurfaceQueryHandle& Handle)
{
	int32 SlotIndex = INDEX_NONE;

	if (!ResolveQuerySlot(Handle, SlotIndex))
	{
		return;
	}

	FSurfaceQuerySlot& Slot = QuerySlots[SlotIndex];

	Slot.bInUse = false;
	Slot.bPositioned = false;
	Slot.bOutsideRibbon = false;
	Slot.FirstSubmittedFrame = TNumericLimits<uint64>::Max();
}

bool USurfaceAccumulationSubsystem::ResolveQuerySlot(const FSurfaceQueryHandle& Handle, int32& OutSlotIndex) const
{
	if (!QuerySlots.IsValidIndex(Handle.SlotIndex))
	{
		return false;
	}

	const FSurfaceQuerySlot& Slot = QuerySlots[Handle.SlotIndex];

	if (!Slot.bInUse || Slot.Generation != Handle.Generation)
	{
		return false;
	}

	OutSlotIndex = Handle.SlotIndex;

	return true;
}

int32 USurfaceAccumulationSubsystem::GetActiveSurfaceQueryCount() const
{
	int32 Active = 0;

	for (const FSurfaceQuerySlot& Slot : QuerySlots)
	{
		Active += Slot.bInUse ? 1 : 0;
	}

	return Active;
}

uint64 USurfaceAccumulationSubsystem::GetQueryLatencyInFrames() const
{
	return bHasResolvedQuery && QueryFrameCounter >= ResolvedQueryFrame
		? QueryFrameCounter - ResolvedQueryFrame
		: 0;
}

FSurfaceStateSample USurfaceAccumulationSubsystem::QuerySurfaceStateInTrackSpace(
	const FSurfaceQueryHandle& Handle,
	const FSurfaceTrackCoordinate& Coordinate)
{
	FSurfaceStateSample Unusable;

	int32 SlotIndex = INDEX_NONE;

	if (!ResolveQuerySlot(Handle, SlotIndex))
	{
		return Unusable;
	}

	EnsureQueryReadback();

	FSurfaceQuerySlot& Slot = QuerySlots[SlotIndex];

	Slot.Coordinate = Coordinate;
	Slot.bPositioned = true;
	Slot.bOutsideRibbon = !Ribbon.ContainsLateralOffset(Coordinate.LateralOffsetAlpha);

	return BuildSurfaceStateSample(SlotIndex);
}

FSurfaceStateSample USurfaceAccumulationSubsystem::QuerySurfaceState(
	const FSurfaceQueryHandle& Handle,
	const FVector& WorldPosition)
{
	const USplineComponent* Spline = Centreline.Get();

	if (Spline == nullptr)
	{
		FSurfaceStateSample WithoutACentreline;
		WithoutACentreline.Status = ESurfaceSampleStatus::OutsideRegion;
		return WithoutACentreline;
	}

	FSurfaceTrackCoordinate Coordinate;

	const ETrackSpaceStatus Status = RacetrackSurfaceTrackSpace::WorldToTrack(
		*Spline,
		Ribbon.TrackWidth,
		WorldPosition,
		Coordinate);

	if (Status != ETrackSpaceStatus::Valid)
	{
		int32 SlotIndex = INDEX_NONE;

		if (ResolveQuerySlot(Handle, SlotIndex))
		{
			QuerySlots[SlotIndex].bOutsideRibbon = true;
		}

		FSurfaceStateSample OffTheRibbon;
		OffTheRibbon.Status = ESurfaceSampleStatus::OutsideRegion;
		return OffTheRibbon;
	}

	return QuerySurfaceStateInTrackSpace(Handle, Coordinate);
}

FSurfaceStateSample USurfaceAccumulationSubsystem::BuildSurfaceStateSample(int32 SlotIndex) const
{
	FSurfaceStateSample Sample;

	const FSurfaceQuerySlot& Slot = QuerySlots[SlotIndex];

	if (Slot.bOutsideRibbon)
	{
		Sample.Status = ESurfaceSampleStatus::OutsideRegion;
		return Sample;
	}

	const bool bResolvedSinceThisSlotWasAcquired =
		bHasResolvedQuery
		&& ResolvedQuerySlots.IsValidIndex(SlotIndex)
		&& Slot.FirstSubmittedFrame != TNumericLimits<uint64>::Max()
		&& ResolvedQueryFrame >= Slot.FirstSubmittedFrame;

	if (!bResolvedSinceThisSlotWasAcquired)
	{
		Sample.Status = ESurfaceSampleStatus::NotYetResolved;
		return Sample;
	}

	const FLinearColor Exposure = ResolvedQuerySlots[SlotIndex] / RacetrackSurfaceQuery::TotalTapWeight;

	Sample.RubberBuildup = RacetrackSurfaceMapping::ExposureToCoverage(Exposure.R);
	Sample.SkidMark = RacetrackSurfaceMapping::ExposureToCoverage(Exposure.G);
	Sample.Marbles = RacetrackSurfaceMapping::ExposureToCoverage(Exposure.B);
	Sample.Status = ESurfaceSampleStatus::Valid;
	Sample.SecondsSinceResolved =
		FMath::Max(GetWorldTimeSecondsForStaleness() - ResolvedQueryWorldTimeSeconds, 0.0f);

	return Sample;
}

void USurfaceAccumulationSubsystem::RecordQuerySubmission(uint64 Frame)
{
	if (QuerySubmissionStamps.Num() != QuerySubmissionStampCount)
	{
		QuerySubmissionStamps.SetNum(QuerySubmissionStampCount);
	}

	FQuerySubmissionStamp& Stamp =
		QuerySubmissionStamps[static_cast<int32>(Frame % QuerySubmissionStampCount)];

	Stamp.Frame = Frame;
	Stamp.WorldTimeSeconds = GetWorldTimeSecondsForStaleness();
}

bool USurfaceAccumulationSubsystem::FindQuerySubmissionTime(uint64 Frame, float& OutWorldTimeSeconds) const
{
	const int32 Index = static_cast<int32>(Frame % QuerySubmissionStampCount);

	if (!QuerySubmissionStamps.IsValidIndex(Index) || QuerySubmissionStamps[Index].Frame != Frame)
	{
		return false;
	}

	OutWorldTimeSeconds = QuerySubmissionStamps[Index].WorldTimeSeconds;

	return true;
}

void USurfaceAccumulationSubsystem::ResolveSurfaceQueries()
{
	if (GetActiveSurfaceQueryCount() == 0)
	{
		return;
	}

	EnsureQueryReadback();

	if (!QueryReadback.IsValid())
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("No surface query readback ring could be built, so every query in this world will "
				"answer NotYetResolved forever"));

		return;
	}

	TArray<FLinearColor> Resolved;
	uint64 ResolvedFrame = 0;

	if (QueryReadback->CopyResolved(Resolved, ResolvedFrame) && ResolvedFrame >= ResolvedQueryFrame)
	{
		ResolvedQuerySlots = MoveTemp(Resolved);
		ResolvedQueryFrame = ResolvedFrame;
		bHasResolvedQuery = true;

		if (!FindQuerySubmissionTime(ResolvedFrame, ResolvedQueryWorldTimeSeconds))
		{
			ResolvedQueryWorldTimeSeconds = GetWorldTimeSecondsForStaleness();
		}
	}

	if (Buffer == nullptr || !Buffer->IsAllocated())
	{
		QueryReadback->Drain();
		return;
	}

	TArray<FSurfaceQueryTap> Taps;
	int32 SlotCount = 0;

	const uint64 SubmittingFrame = QueryFrameCounter + 1;
	const double ContactPatchWidth = GetQueryContactPatchWidth();

	for (int32 Index = 0; Index < QuerySlots.Num(); ++Index)
	{
		FSurfaceQuerySlot& Slot = QuerySlots[Index];

		if (!Slot.bInUse || !Slot.bPositioned || Slot.bOutsideRibbon)
		{
			continue;
		}

		RacetrackSurfaceQuery::BuildTaps(Ribbon, Index, Slot.Coordinate, ContactPatchWidth, Taps);

		if (Slot.FirstSubmittedFrame == TNumericLimits<uint64>::Max())
		{
			Slot.FirstSubmittedFrame = SubmittingFrame;
		}

		SlotCount = Index + 1;
	}

	if (Taps.Num() == 0)
	{
		QueryReadback->Drain();
		return;
	}

	const UTextureRenderTarget2D* GatherTarget = Buffer->EnsureGatherTarget(this, SlotCount);

	if (GatherTarget == nullptr)
	{
		return;
	}

	if (GatherTarget->SizeX != GatherTargetWidthInFlight)
	{
		GatherTargetWidthInFlight = GatherTarget->SizeX;

		QueryReadback->RebuildForANewSourceSize();

		UE_LOG(LogRacetrackSurface, Log,
			TEXT("Query gather target resized to %d slots, so the readback staging textures are rebuilt. ")
			TEXT("FRHIGPUTextureReadback reuses its staging texture and assumes the source never changes ")
			TEXT("size, so a resize has to drop the ring rather than copy a wider source into it."),
			GatherTarget->SizeX);
	}

	QueryFrameCounter = SubmittingFrame;

	Buffer->GatherTaps(this, Taps, SlotCount);

	RecordQuerySubmission(QueryFrameCounter);

	QueryReadback->Submit(Buffer->GetGatherTarget(), SlotCount, QueryFrameCounter);
}

void USurfaceAccumulationSubsystem::ResetSurface()
{
	PendingContacts.Reset();
	StrokesResolvedThisFrame.Reset();
	FMemory::Memzero(ContactsDiscardedOutsideRibbon);
	ContactsDiscardedWithoutCentreline = 0;
	DecayProgressInHalfLives = 0.0f;
	DecayPassCount = 0;

	InvalidateQueryCache();

	if (Buffer != nullptr)
	{
		Buffer->Clear(this);
	}
}

void USurfaceAccumulationSubsystem::AppendStroke(
	const FSurfaceTrackContact& Contact,
	double FromDistance,
	double FromAlpha,
	double ToDistance,
	double ToAlpha,
	TArray<FSurfaceBrushStroke>& OutStrokes) const
{
	if (!Ribbon.ContainsLateralOffset(FromAlpha) && !Ribbon.ContainsLateralOffset(ToAlpha))
	{
		return;
	}

	const double FromAlongTrack = Ribbon.NormalisedAlongTrack(FromDistance);
	const double ToAlongTrack = Ribbon.NormalisedAlongTrack(ToDistance);
	const int32 RowIndex = Ribbon.RowIndexAt((FromAlongTrack + ToAlongTrack) * 0.5);

	FSurfaceBrushStroke Stroke;

	Stroke.RegionStart = Ribbon.RibbonToAtlasInRow(
		RowIndex,
		FromAlongTrack,
		FSurfaceTrackRibbon::NormalisedAcrossTrack(FromAlpha));

	Stroke.RegionEnd = Ribbon.RibbonToAtlasInRow(
		RowIndex,
		ToAlongTrack,
		FSurfaceTrackRibbon::NormalisedAcrossTrack(ToAlpha));

	Stroke.WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(Contact.ContactWidth));
	Stroke.Intensity = RacetrackSurfaceMapping::SlipToExposure(Contact.SlipAmount, GetDepositionScale());
	Stroke.RowIndex = RowIndex;
	Stroke.Channel = Contact.Channel;

	OutStrokes.Add(Stroke);
}

void USurfaceAccumulationSubsystem::BuildStrokes(
	const FSurfaceTrackContact& Contact,
	float DeltaSeconds,
	TArray<FSurfaceBrushStroke>& OutStrokes) const
{
	const double Length = FMath::Max(Ribbon.TrackLength, UE_DOUBLE_SMALL_NUMBER);

	const double EndDistance = Ribbon.WrapDistance(Contact.Coordinate.DistanceAlongCentreline);
	const double EndAlpha = Contact.Coordinate.LateralOffsetAlpha;

	const double Travel = FMath::Clamp(
		Contact.CentimetresPerSecondAlongCentreline * DeltaSeconds,
		-Length,
		Length);

	const double StartDistance = Ribbon.bClosedLoop
		? EndDistance - Travel
		: FMath::Clamp(EndDistance - Travel, 0.0, Length);

	const double LateralExtrapolationSeconds = FMath::Min(
		static_cast<double>(DeltaSeconds),
		RacetrackSurfaceMapping::MaximumLateralExtrapolationSeconds);

	const double StartAlpha = EndAlpha - Contact.LateralOffsetAlphaPerSecond * LateralExtrapolationSeconds;

	const double Span = EndDistance - StartDistance;
	const double RowLength = Ribbon.RowLength();

	TArray<double, TInlineAllocator<8>> Boundaries;
	Boundaries.Add(0.0);

	if (FMath::Abs(Span) > UE_DOUBLE_SMALL_NUMBER)
	{
		const double Step = Span > 0.0 ? 1.0 : -1.0;

		const double FirstRow = Span > 0.0
			? FMath::FloorToDouble(StartDistance / RowLength) + 1.0
			: FMath::CeilToDouble(StartDistance / RowLength) - 1.0;

		for (double Row = FirstRow;
			Span > 0.0 ? Row * RowLength < EndDistance : Row * RowLength > EndDistance;
			Row += Step)
		{
			const double Fraction = (Row * RowLength - StartDistance) / Span;

			if (Fraction > UE_DOUBLE_SMALL_NUMBER && Fraction < 1.0 - UE_DOUBLE_SMALL_NUMBER)
			{
				Boundaries.Add(Fraction);
			}
		}
	}

	Boundaries.Add(1.0);

	for (int32 Piece = 0; Piece + 1 < Boundaries.Num(); ++Piece)
	{
		const double FromFraction = Boundaries[Piece];
		const double ToFraction = Boundaries[Piece + 1];

		const double FromDistance = StartDistance + Span * FromFraction;
		const double ToDistance = StartDistance + Span * ToFraction;

		const double LapsBack = FMath::FloorToDouble((FromDistance + ToDistance) * 0.5 / Length);

		AppendStroke(
			Contact,
			FromDistance - LapsBack * Length,
			StartAlpha + (EndAlpha - StartAlpha) * FromFraction,
			ToDistance - LapsBack * Length,
			StartAlpha + (EndAlpha - StartAlpha) * ToFraction,
			OutStrokes);
	}
}

void USurfaceAccumulationSubsystem::FlushPendingContacts(float DeltaSeconds)
{
	StrokesResolvedThisFrame.Reset(PendingContacts.Num());

	for (const FSurfaceTrackContact& Contact : PendingContacts)
	{
		BuildStrokes(Contact, DeltaSeconds, StrokesResolvedThisFrame);
	}

	PendingContacts.Reset();

	if (StrokesResolvedThisFrame.Num() > 0 && EnsureBuffer())
	{
		Buffer->SweepStrokes(
			this,
			StrokesResolvedThisFrame,
			EAccumulationChannel::Marbles,
			GetMarbleSweepRetention());

		Buffer->PaintStrokes(this, StrokesResolvedThisFrame);
	}
}
