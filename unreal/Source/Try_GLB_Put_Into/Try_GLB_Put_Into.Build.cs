using UnrealBuildTool;

public class Try_GLB_Put_Into : ModuleRules
{
	public Try_GLB_Put_Into(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"HTTP", "Json", "JsonUtilities",
			"Slate", "SlateCore", "UMG", "glTFRuntime", "RenderCore", "RHI",
			"ProceduralMeshComponent", "Niagara"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"ApplicationCore",
			"OnlineSubsystem", "OnlineSubsystemNull",
			"Sockets", "Networking"
		});
	}
}
