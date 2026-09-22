#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "SurfaceAccumulationMapping.h"
#include "SurfaceAccumulationSubsystem.h"
#include "Tests/RacetrackSurfaceTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

using RacetrackSurfaceTest::FindRenderableWorld;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfacePublishesRegionAndSaturationToMaterials,
	"RacetrackSurface.Materials.PublishesRegionAndSaturation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfacePublishesRegionAndSaturationToMaterials::RunTest(const FString& Parameters)
{
	UWorld* World = FindRenderableWorld();

	if (World == nullptr)
	{
		AddError(TEXT("No editor, game or PIE world available."));
		return false;
	}

	UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(
		nullptr,
		RacetrackSurfaceMapping::ParameterCollectionAssetPath);

	if (Collection == nullptr)
	{
		AddError(FString::Printf(
			TEXT("Parameter collection %s could not be loaded."),
			RacetrackSurfaceMapping::ParameterCollectionAssetPath));
		return false;
	}

	USurfaceAccumulationSubsystem* Subsystem = World->GetSubsystem<USurfaceAccumulationSubsystem>();

	if (Subsystem == nullptr)
	{
		AddError(TEXT("Could not resolve USurfaceAccumulationSubsystem from the world."));
		return false;
	}

	FSurfaceRegion Region = RacetrackSurfaceTest::MakeTestRegion();
	Region.WorldCentre = FVector2D(1234.5, -6789.0);
	Region.WorldSize = 30000.0;

	Subsystem->ConfigureRegion(Region);

	const FLinearColor PublishedCentre = UKismetMaterialLibrary::GetVectorParameterValue(
		World, Collection, RacetrackSurfaceMapping::RegionCentreParameterName);

	TestTrue(
		TEXT("Region centre X reaches the parameter collection"),
		FMath::IsNearlyEqual(PublishedCentre.R, static_cast<float>(Region.WorldCentre.X), 0.01f));

	TestTrue(
		TEXT("Region centre Y reaches the parameter collection"),
		FMath::IsNearlyEqual(PublishedCentre.G, static_cast<float>(Region.WorldCentre.Y), 0.01f));

	const float PublishedWorldSize = UKismetMaterialLibrary::GetScalarParameterValue(
		World, Collection, RacetrackSurfaceMapping::RegionWorldSizeParameterName);

	TestTrue(
		TEXT("Region world size reaches the parameter collection"),
		FMath::IsNearlyEqual(PublishedWorldSize, static_cast<float>(Region.WorldSize), 0.01f));

	const float PublishedSaturationConstant = UKismetMaterialLibrary::GetScalarParameterValue(
		World, Collection, RacetrackSurfaceMapping::SaturationConstantParameterName);

	TestEqual(
		TEXT("The saturation constant reaches the parameter collection unchanged"),
		PublishedSaturationConstant,
		RacetrackSurfaceMapping::SaturationConstant);

	return true;
}

#endif
