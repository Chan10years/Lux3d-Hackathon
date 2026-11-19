using UnrealBuildTool;

public class TrioCombatEditor : ModuleRules
{
    public TrioCombatEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "InputCore", "TrioCombat", "UnrealEd", "AssetRegistry", "Kismet", "KismetCompiler", "BlueprintGraph", "UMG", "UMGEditor", "PhysicsUtilities", "PhysicsCore", "ImageCore", "RenderCore"
        });
    }
}
