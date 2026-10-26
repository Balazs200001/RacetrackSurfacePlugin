// Copyright 2026 Balázs Fábián. All Rights Reserved.

using UnrealBuildTool;

public class RacetrackSurface : ModuleRules
{
	public RacetrackSurface(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"RHI",
				"RenderCore"
			}
			);
	}
}
