#include "SurfaceAccumulationSubsystem.h"

#include "Components/SplineComponent.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceTrackSpace.h"

DEFINE_LOG_CATEGORY(LogRacetrackSurface);

namespace
{
	TAutoConsoleVariable<float> CVarDepositionScale(
		TEXT("r.RacetrackSurface.DepositionScale"),
		1.0f,
		TEXT("Multiplies how much exposure one contact deposits. 1 is the derived rate, at which ")
		TEXT("buildup approaches saturation over roughly 500 contacts. Raise it to watch a session's ")
		TEXT("worth of rubber accumulate in a few laps without time compression."),
		ECVF_Default);
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

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Published saturation constant %.3f to %s"),
		RacetrackSurfaceMapping::SaturationConstant,
		*Collection->GetName());
}

void USurfaceAccumulationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	FlushPendingContacts(DeltaTime);
}

void USurfaceAccumulationSubsystem::ConfigureTrackRibbon(
	const USplineComponent* NewCentreline,
	const FSurfaceTrackRibbon& NewRibbon)
{
	Centreline = NewCentreline;
	Ribbon = NewRibbon;

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Surface ribbon configured: %.1f cm of track at %.1f cm wide, %.4f cm per texel, %d x %d texels, %s"),
		Ribbon.TrackLength,
		Ribbon.TrackWidth,
		Ribbon.CentimetresPerTexel,
		Ribbon.AlongTrackTexels(),
		Ribbon.AcrossTrackTexels(),
		Ribbon.bClosedLoop ? TEXT("closed loop") : TEXT("open ended"));

	if (NewCentreline == nullptr)
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("Surface ribbon configured without a centreline, so world-space contacts cannot be placed"));
	}

	if (!Ribbon.FitsUnfolded())
	{
		UE_LOG(LogRacetrackSurface, Error,
			TEXT("Surface ribbon needs %d texels along the track but a single row holds %d. ")
			TEXT("Folding into rows is not implemented, so this ribbon cannot be allocated."),
			Ribbon.AlongTrackTexels(),
			FSurfaceTrackRibbon::MaximumTexelsPerRow);
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
		++ContactsDiscardedOutsideRibbon;
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
		++ContactsDiscardedOutsideRibbon;
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
	ContactsDiscardedOutsideRibbon = 0;
	ContactsDiscardedWithoutCentreline = 0;

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

	FSurfaceBrushStroke Stroke;

	Stroke.RegionStart = FVector2D(
		Ribbon.NormalisedAlongTrack(FromDistance),
		FSurfaceTrackRibbon::NormalisedAcrossTrack(FromAlpha));

	Stroke.RegionEnd = FVector2D(
		Ribbon.NormalisedAlongTrack(ToDistance),
		FSurfaceTrackRibbon::NormalisedAcrossTrack(ToAlpha));

	Stroke.WidthInTexels = static_cast<float>(Ribbon.WorldDistanceToTexels(Contact.ContactWidth));
	Stroke.Intensity = RacetrackSurfaceMapping::SlipToExposure(
		Contact.SlipAmount, CVarDepositionScale.GetValueOnGameThread());
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

	const double UnwrappedStartDistance = EndDistance - Travel;
	const double StartAlpha = EndAlpha - Contact.LateralOffsetAlphaPerSecond * DeltaSeconds;

	const bool bCrossesSeam = Ribbon.bClosedLoop
		&& (UnwrappedStartDistance < 0.0 || UnwrappedStartDistance > Length);

	if (!bCrossesSeam)
	{
		AppendStroke(
			Contact,
			FMath::Clamp(UnwrappedStartDistance, 0.0, Length),
			StartAlpha,
			EndDistance,
			EndAlpha,
			OutStrokes);

		return;
	}

	const double SeamDistance = Travel > 0.0 ? 0.0 : Length;

	const double SeamFraction = FMath::Abs(Travel) > UE_DOUBLE_SMALL_NUMBER
		? FMath::Clamp((SeamDistance - UnwrappedStartDistance) / Travel, 0.0, 1.0)
		: 0.0;

	const double SeamAlpha = StartAlpha + (EndAlpha - StartAlpha) * SeamFraction;

	if (Travel > 0.0)
	{
		AppendStroke(Contact, UnwrappedStartDistance + Length, StartAlpha, Length, SeamAlpha, OutStrokes);
		AppendStroke(Contact, 0.0, SeamAlpha, EndDistance, EndAlpha, OutStrokes);
	}
	else
	{
		AppendStroke(Contact, UnwrappedStartDistance - Length, StartAlpha, 0.0, SeamAlpha, OutStrokes);
		AppendStroke(Contact, Length, SeamAlpha, EndDistance, EndAlpha, OutStrokes);
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
