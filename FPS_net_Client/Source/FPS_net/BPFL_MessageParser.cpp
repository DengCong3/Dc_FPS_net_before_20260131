#include "BPFL_MessageParser.h"
#include "Kismet/KismetSystemLibrary.h"

// 字符串分割实现（逻辑不变）
TArray<FString> UBPFL_MessageParser::SplitString(const FString& SourceStr, const FString& Delimiter)
{
	TArray<FString> ResultParts;
	if (SourceStr.IsEmpty() || Delimiter.IsEmpty())
	{
		return ResultParts;
	}
	SourceStr.ParseIntoArray(ResultParts, *Delimiter, true);
	return ResultParts;
}

// 解析Pos同步消息（适配ani_id：每个玩家字段从8个增至9个）
void UBPFL_MessageParser::Parse_PosMessage(const FString& InServerMsg, TArray<FPlayerPosData>& OutPlayerDatas, bool& bParseSuccess)
{
	OutPlayerDatas.Empty();
	bParseSuccess = false;
	TArray<FString> MsgParts = SplitString(InServerMsg);

	// 协议格式校验：头部为"pos" + 总人数 + N*9个字段（ID+x+y+z+roll+pitch+yaw+HP+ani_id）
	if (MsgParts.Num() < 11 || !MsgParts[0].Equals(TEXT("pos"), ESearchCase::IgnoreCase))
	{
		UKismetSystemLibrary::PrintString(nullptr, TEXT("❌ Pos解析失败：格式错误（需包含ani_id）"), true, true, FLinearColor::Red, 2.0f);
		return;
	}

	int32 TotalPlayers = FCString::Atoi(*MsgParts[1]);
	// 字段数量校验：2（头部） + 玩家数*9（每个玩家9个字段）
	if (TotalPlayers <= 0 || MsgParts.Num() < 2 + TotalPlayers * 9)
	{
		UKismetSystemLibrary::PrintString(nullptr, TEXT("❌ Pos解析失败：玩家数量/字段不匹配（需包含ani_id）"), true, true, FLinearColor::Red, 2.0f);
		return;
	}

	// 解析每个玩家数据（新增ani_id解析）
	for (int32 i = 0; i < TotalPlayers; ++i)
	{
		int32 StartIdx = 2 + i * 9; // 每个玩家占9个字段，起始索引偏移修改
		if (StartIdx + 8 >= MsgParts.Num()) // 需取到StartIdx+8（ani_id）
		{
			UKismetSystemLibrary::PrintString(nullptr, FString::Printf(TEXT("❌ 玩家%d数据不完整（缺少ani_id），跳过"), i + 1), true, true, FLinearColor::Yellow, 2.0f);
			continue;
		}

		FPlayerPosData PlayerData;
		PlayerData.PlayerID = FCString::Atoi(*MsgParts[StartIdx]);
		PlayerData.Pos.X = FCString::Atof(*MsgParts[StartIdx + 1]);
		PlayerData.Pos.Y = FCString::Atof(*MsgParts[StartIdx + 2]);
		PlayerData.Pos.Z = FCString::Atof(*MsgParts[StartIdx + 3]);
		PlayerData.Rot.Roll = FCString::Atof(*MsgParts[StartIdx + 4]);
		PlayerData.Rot.Pitch = FCString::Atof(*MsgParts[StartIdx + 5]);
		PlayerData.Rot.Yaw = FCString::Atof(*MsgParts[StartIdx + 6]);
		PlayerData.HP = FCString::Atoi(*MsgParts[StartIdx + 7]);
		// 新增：解析ani_id
		PlayerData.ani_id = FCString::Atoi(*MsgParts[StartIdx + 8]);

		// 过滤无效数据
		if (PlayerData.PlayerID <= 0 || (PlayerData.Pos.IsZero() && PlayerData.Rot.IsZero()))
		{
			continue;
		}

		OutPlayerDatas.Add(PlayerData);
	}

	bParseSuccess = !OutPlayerDatas.IsEmpty();
	//
	//UKismetSystemLibrary::PrintString(nullptr, FString::Printf(TEXT("✅ Pos解析成功：%d个玩家（含ani_id）"), OutPlayerDatas.Num()), true, true, FLinearColor::Green, 2.0f);
}

// 解析PlayerID消息（逻辑不变）
void UBPFL_MessageParser::Parse_IDMessage(const FString& InServerMsg, int32& OutPlayerID, bool& bParseSuccess)
{
	OutPlayerID = 0;
	bParseSuccess = false;
	TArray<FString> MsgParts = SplitString(InServerMsg);

	if (MsgParts.Num() != 2 || !MsgParts[0].Equals(TEXT("ID"), ESearchCase::IgnoreCase))
	{
		UKismetSystemLibrary::PrintString(nullptr, TEXT("❌ ID解析失败：格式错误"), true, true, FLinearColor::Red, 2.0f);
		return;
	}

	OutPlayerID = FCString::Atoi(*MsgParts[1]);
	if (OutPlayerID <= 0)
	{
		UKismetSystemLibrary::PrintString(nullptr, TEXT("❌ ID解析失败：无效ID"), true, true, FLinearColor::Red, 2.0f);
		return;
	}

	bParseSuccess = true;
	//
	//UKismetSystemLibrary::PrintString(nullptr, FString::Printf(TEXT("✅ ID解析成功：PlayerID=%d"), OutPlayerID), true, true, FLinearColor::Green, 2.0f);
}