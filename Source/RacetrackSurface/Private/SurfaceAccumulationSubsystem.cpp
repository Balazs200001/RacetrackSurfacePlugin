#include "SurfaceAccumulationSubsystem.h"

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

	Super::Deinitialize();
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
}

void USurfaceAccumulationSubsystem::ResetSurface()
{
	PendingContacts.Reset();
	StrokesResolvedThisFrame.Reset();
	ContactsDiscardedOutsideRegion = 0;
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
}
