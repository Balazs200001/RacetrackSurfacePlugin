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

float USurfaceAccumulationSubsystem::GetDecayStepsPerHalfLife()
{
	return FMath::Max(CVarDecayStepsPerHalfLife.GetValueOnGameThread(), 1.0f);
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

void USurfaceAccumulationSubsystem::ResetSurface()
{
	PendingContacts.Reset();
	StrokesResolvedThisFrame.Reset();
	FMemory::Memzero(ContactsDiscardedOutsideRibbon);
	ContactsDiscardedWithoutCentreline = 0;
	DecayProgressInHalfLives = 0.0f;
	DecayPassCount = 0;

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

	const double StartAlpha = EndAlpha - Contact.LateralOffsetAlphaPerSecond * DeltaSeconds;

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
		Buffer->PaintStrokes(this, StrokesResolvedThisFrame);
	}
}
