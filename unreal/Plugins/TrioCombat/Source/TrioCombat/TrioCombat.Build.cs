using UnrealBuildTool;

public class TrioCombat : ModuleRules
{
    public TrioCombat(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG", "NiagaraAnimNotifies" });
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "AIModule", "InputCore", "Niagara" });
    }
}
