#include "SurfaceAccumulationSubsystem.h"

#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "SurfaceAccumulationBuffer.h"
#include "SurfaceAccumulationMapping.h"

DEFINE_LOG_CATEGORY(LogRacetrackSurface);

void USurfaceAccumulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Surface region centred at (%.1f, %.1f), %.1f cm square, %d texels, %.2f cm per texel"),
		Region.WorldCentre.X,
		Region.WorldCentre.Y,
		Region.WorldSize,
		Region.ResolutionTexels,
		Region.CentimetresPerTexel());
}

void USurfaceAccumulationSubsystem::Deinitialize()
{
	PendingContacts.Empty();
	StrokesResolvedThisFrame.Empty();

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

	if (Buffer == nullptr || !Buffer->Allocate(this, Region))
	{
		return false;
	}

	if (bFirstAllocation)
	{
		PublishRegionToMaterials();
	}

	return true;
}

UTextureRenderTarget2D* USurfaceAccumulationSubsystem::GetOrCreateAccumulationTarget()
{
	return EnsureBuffer() ? Buffer->GetTarget() : nullptr;
}

void USurfaceAccumulationSubsystem::PublishRegionToMaterials()
{
	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(
		nullptr,
		RacetrackSurfaceMapping::ParameterCollectionAssetPath);

	if (Collection == nullptr)
	{
		UE_LOG(LogRacetrackSurface, Warning,
			TEXT("Parameter collection %s is missing, so materials cannot resolve the region or the saturation constant"),
			RacetrackSurfaceMapping::ParameterCollectionAssetPath);
		return;
	}

	UKismetMaterialLibrary::SetVectorParameterValue(
		this,
		Collection,
		RacetrackSurfaceMapping::RegionCentreParameterName,
		FLinearColor(
			static_cast<float>(Region.WorldCentre.X),
			static_cast<float>(Region.WorldCentre.Y),
			0.0f,
			0.0f));

	UKismetMaterialLibrary::SetScalarParameterValue(
		this,
		Collection,
		RacetrackSurfaceMapping::RegionWorldSizeParameterName,
		static_cast<float>(Region.WorldSize));

	UKismetMaterialLibrary::SetScalarParameterValue(
		this,
		Collection,
		RacetrackSurfaceMapping::SaturationConstantParameterName,
		RacetrackSurfaceMapping::SaturationConstant);

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Published region centre (%.1f, %.1f), world size %.1f and saturation constant %.3f to %s"),
		Region.WorldCentre.X,
		Region.WorldCentre.Y,
		Region.WorldSize,
		RacetrackSurfaceMapping::SaturationConstant,
		*Collection->GetName());
}

void USurfaceAccumulationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	FlushPendingContacts(DeltaTime);
}

void USurfaceAccumulationSubsystem::RegisterSurfaceContact(const FSurfaceContact& Contact)
{
	PendingContacts.Add(Contact);
}

void USurfaceAccumulationSubsystem::RegisterSurfaceContact(
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

	RegisterSurfaceContact(Contact);
}

void USurfaceAccumulationSubsystem::ConfigureRegion(const FSurfaceRegion& NewRegion)
{
	Region = NewRegion;

	UE_LOG(LogRacetrackSurface, Log,
		TEXT("Surface region reconfigured to %.1f cm square at %d texels, %.2f cm per texel"),
		Region.WorldSize,
		Region.ResolutionTexels,
		Region.CentimetresPerTexel());

	PublishRegionToMaterials();
}

void USurfaceAccumulationSubsystem::ResetSurface()
{
	PendingContacts.Reset();
	StrokesResolvedThisFrame.Reset();
	ContactsDiscardedOutsideRegion = 0;

	if (Buffer != nullptr)
	{
		Buffer->Clear(this);
	}
}

bool USurfaceAccumulationSubsystem::BuildStroke(
	const FSurfaceContact& Contact,
	float DeltaSeconds,
	FSurfaceBrushStroke& OutStroke) const
{
	const FVector SweptStart = Contact.WorldPosition - Contact.Velocity * DeltaSeconds;

	const FVector2D NormalisedStart = Region.WorldToNormalised(SweptStart);
	const FVector2D NormalisedEnd = Region.WorldToNormalised(Contact.WorldPosition);

	if (!Region.ContainsNormalised(NormalisedStart) && !Region.ContainsNormalised(NormalisedEnd))
	{
		return false;
	}

	OutStroke.RegionStart = NormalisedStart;
	OutStroke.RegionEnd = NormalisedEnd;
	OutStroke.WidthInTexels = static_cast<float>(Region.WorldDistanceToTexels(Contact.ContactWidth));
	OutStroke.Intensity = FMath::Clamp(Contact.SlipAmount, 0.0f, 1.0f);
	OutStroke.Channel = Contact.Channel;

	return true;
}

void USurfaceAccumulationSubsystem::FlushPendingContacts(float DeltaSeconds)
{
	StrokesResolvedThisFrame.Reset(PendingContacts.Num());

	for (const FSurfaceContact& Contact : PendingContacts)
	{
		FSurfaceBrushStroke Stroke;
		if (BuildStroke(Contact, DeltaSeconds, Stroke))
		{
			StrokesResolvedThisFrame.Add(Stroke);
		}
		else
		{
			++ContactsDiscardedOutsideRegion;
		}
	}

	PendingContacts.Reset();

	if (StrokesResolvedThisFrame.Num() > 0 && EnsureBuffer())
	{
		Buffer->PaintStrokes(this, StrokesResolvedThisFrame);
	}
}
