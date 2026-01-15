// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class FPS_net : ModuleRules
{
    public FPS_net(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // 公共依赖模块（Game 和 Editor 都会用到）
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "EnhancedInput",      // 启用 UE5 Enhanced Input 系统
            "PhysicsCore",        // 可选，但推荐保留（用于碰撞、射线等）
            "HeadMountedDisplay", // 可选，支持 VR，不影响非 VR 项目
            "Sockets",            // ← 新增：用于 TCP/UDP Socket
            "Networking"          // ← 新增：用于 IP 地址、Socket 子系统
        });

        // 私有依赖模块（仅本模块内部使用）
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore"
            // 如果你后续添加 UMG、Niagara、OnlineSubsystem 等，可在此添加
            // 例如："UMG", "Niagara", "OnlineSubsystem"
        });

        // 取消下面的注释可启用 IWYU（Include What You Use）检查
        // bEnforceIWYU = true;
    }
}