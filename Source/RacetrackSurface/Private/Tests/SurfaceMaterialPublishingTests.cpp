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
using RacetrackSurfaceTest::MakeTestCircuit;
using RacetrackSurfaceTest::MakeTestRibbon;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSurfacePublishesSaturationToMaterials,
	"RacetrackSurface.Materials.PublishesSaturation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSurfacePublishesSaturationToMaterials::RunTest(const FString& Parameters)
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

	UKismetMaterialLibrary::SetScalarParameterValue(
		World, Collection, RacetrackSurfaceMapping::SaturationConstantParameterName, 0.0f);

	const TStrongObjectPtr<USplineComponent> Circuit = MakeTestCircuit();

	Subsystem->ConfigureTrackRibbon(Circuit.Get(), MakeTestRibbon(*Circuit));

	const float PublishedSaturationConstant = UKismetMaterialLibrary::GetScalarParameterValue(
		World, Collection, RacetrackSurfaceMapping::SaturationConstantParameterName);

	TestEqual(
		TEXT("The saturation constant reaches the parameter collection unchanged"),
		PublishedSaturationConstant,
		RacetrackSurfaceMapping::SaturationConstant);

	TestTrue(
		TEXT("The subsystem holds the centreline it was configured with"),
		Subsystem->GetCentreline() == Circuit.Get());

	TestTrue(
		TEXT("The subsystem holds the ribbon it was configured with"),
		FMath::IsNearlyEqual(Subsystem->GetRibbon().TrackLength, Circuit->GetSplineLength(), 1.0));

	return true;
}

#endif
