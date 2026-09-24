#include "SurfaceAccumulationSubsystem.h"

#include "Components/SplineComponent.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceTrackSpace.h"

DEFINE_LOG_CATEGORY(LogRacetrackSurface);

namespace
{
	void AnnounceOnScreen(FName MessageName, const FString& Text)
	{
		if (GEngine != nullptr)
		{
			GEngine->AddOnScreenDebugMessage(
				static_cast<uint64>(GetTypeHash(MessageName)),
				6.0f,
				FColor::White,
				Text);
		}
	}

	void AnnounceDepositionScale(IConsoleVariable* Variable)
	{
		AnnounceOnScreen(
			TEXT("RacetrackSurface.DepositionScale"),
			FString::Printf(TEXT("Deposition now %.2fx the derived rate"), Variable->GetFloat()));
	}

	TAutoConsoleVariable<float> CVarDepositionScale(
		TEXT("r.RacetrackSurface.DepositionScale"),
		1.0f,
		TEXT("Multiplies how much exposure one contact deposits. 1 is the derived rate, at which ")
		TEXT("buildup approaches saturation over roughly 500 contacts. Raise it to watch a session's ")
		TEXT("worth of rubber accumulate in a few laps without time compression."),
		FConsoleVariableDelegate::CreateStatic(&AnnounceDepositionScale),
		ECVF_Default);
}

float USurfaceAccumulationSubsystem::GetDepositionScale()
{
	return CVarDepositionScale.GetValueOnGameThread();
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

	FlushPendingContacts(DeltaTime);
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
