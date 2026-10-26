// Copyright 2026 Balázs Fábián. All Rights Reserved.

using UnrealBuildTool;

public class RacetrackSurfaceEditor : ModuleRules
{
    public RacetrackSurfaceEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "UnrealEd",
            "RacetrackSurface"
        });
    }
}