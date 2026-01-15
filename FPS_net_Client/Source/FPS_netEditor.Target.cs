using UnrealBuildTool;
using System.Collections.Generic;

public class FPS_netEditorTarget : TargetRules
{
    public FPS_netEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V6; // UE5.7 最新V6版本，解决升级提示
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        bOverrideBuildEnvironment = true; // 解决安装版引擎冲突
        ExtraModuleNames.AddRange(new string[] { "FPS_net" });
    }
}