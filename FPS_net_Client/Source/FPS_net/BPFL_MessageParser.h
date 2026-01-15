#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Math/Vector.h"
#include "Math/Rotator.h"
#include "BPFL_MessageParser.generated.h"

// 玩家位置数据结构体（新增ani_id：0=Idle 1=Move）
USTRUCT(BlueprintType, Category = "PlayerSync")
struct FPS_NET_API FPlayerPosData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "PlayerSync")
	int32 PlayerID = 0;

	UPROPERTY(BlueprintReadWrite, Category = "PlayerSync")
	FVector Pos = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "PlayerSync")
	FRotator Rot = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadWrite, Category = "PlayerSync")
	int32 HP = 100;

	// 新增：动画状态ID（0=Idle 1=Move 预留2=Fire 3=Damage）
	UPROPERTY(BlueprintReadWrite, Category = "PlayerSync")
	int32 ani_id = 0;
};

UCLASS()
class FPS_NET_API UBPFL_MessageParser : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	// 解析Pos同步消息（适配ani_id字段）
	UFUNCTION(BlueprintPure, Category = "MessageParser", meta = (DisplayName = "解析Pos同步消息"))
	static void Parse_PosMessage(const FString& InServerMsg, TArray<FPlayerPosData>& OutPlayerDatas, bool& bParseSuccess);

	// 解析PlayerID消息（逻辑不变）
	UFUNCTION(BlueprintPure, Category = "MessageParser", meta = (DisplayName = "解析PlayerID消息"))
	static void Parse_IDMessage(const FString& InServerMsg, int32& OutPlayerID, bool& bParseSuccess);

private:
	// 字符串分割工具（内部实现）
	static TArray<FString> SplitString(const FString& SourceStr, const FString& Delimiter = TEXT("|"));
};