using UnrealBuildTool;
using System.Collections.Generic;

public class FPS_netTarget : TargetRules
{
    public FPS_netTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V6; // 同步升级为V6，统一配置
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        bOverrideBuildEnvironment = true; // 解决安装版引擎冲突
        ExtraModuleNames.AddRange(new string[] { "FPS_net" });
    }
}