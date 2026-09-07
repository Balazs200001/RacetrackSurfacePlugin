using UnrealBuildTool;

public class RacetrackSurfaceEditor: ModuleRules
{
    public RacetrackSurfaceEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PrivateDependencyModuleNames.AddRange(new string[] {"Core", "CoreUObject", "Engine"});
    }
}
