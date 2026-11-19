#pragma once

#include "CoreMinimal.h"
#include "NetSessionSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "RoomCodeSubsystem.generated.h"

class SWidget;
class SEditableTextBox;
class IInputProcessor;
class UUserWidget;
class UFont;
class USoundBase;
struct FKeyEvent;

/**
 * 《异界：溯源信标》—— 纯新版 SuoMao 视觉风格 6 位房号局域网联机与选人大厅子系统。
 * 流程：
 *   WBP_Mode (01 信标争夺 -> 前往)
 *     -> SuoMao 房间匹配弹窗（创建房间生成 6 位码 / 输入 6 位码或 IP 局域网加入）
 *     -> 进房后自动进入原生 WBP_CharSel（选人阶段：开放薇奥/莱卡/凯恩，禁用未开放柯巴）
 *     -> 满 2 人后由房主点击开始对局 -> 全员同步进入 8 分钟 Lvl_ZZZ 战场！
 */
UCLASS()
class TRY_GLB_PUT_INTO_API URoomCodeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** 房主：一键创建局域网 6 位房号房间并进入选人阶段 */
	UFUNCTION(BlueprintCallable, Category = "RoomCode")
	void HostRoom();

	/** 客户端：输入 6 位房号（或房主 IP）局域网搜房并加入 */
	UFUNCTION(BlueprintCallable, Category = "RoomCode")
	void JoinRoom(const FString& RoomCode);

	/** 房主：人数 >= 2 时开启 8 分钟对局 */
	UFUNCTION(BlueprintCallable, Category = "RoomCode")
	void StartMatch();

	/** 双方：离开当前房间并返回模式选择页 */
	UFUNCTION(BlueprintCallable, Category = "RoomCode")
	void LeaveCurrentRoom();

	/** 当前 6 位房号 */
	UPROPERTY(BlueprintReadOnly, Category = "RoomCode")
	FString CurrentRoomCode;

	/** 当前状态提示文字 */
	UPROPERTY(BlueprintReadOnly, Category = "RoomCode")
	FString StatusMessage = TEXT("点击【创建房间】生成 6 位房号，或输入队友房号加入");

	/** 探测到的本机局域网地址 ip:port */
	UPROPERTY(BlueprintReadOnly, Category = "RoomCode")
	FString LocalConnectionAddress;

	FString GetForgeServiceUrl() const { return SelectedForgeUrl; }
	int32 GetSelectedCharacterIndex() const { return SelectedCharacterIndex; }
	bool IsMatchMenuOpen() const { return bMatchRoomExpanded; }

private:
	UFUNCTION()
	void OnModeGoClicked();

	UFUNCTION()
	void OnCharSelLockClicked();

	UFUNCTION()
	void OnCharSelBackClicked();

	UFUNCTION()
	void HandleNetSessionStateChanged(ENetSessionState NewState, ENetSessionError LastError);

	void PlaySuoMaoSfx(const TCHAR* AssetPath) const;
	FSlateFontInfo GetSuoMaoFont(bool bDisplay, int32 Size) const;
	void SwitchSuoMaoScreen(UUserWidget* RootWidget, const TCHAR* ScreenName) const;
	void SyncFrontendWidgets(UWorld* World);
	void EnsureOverlayMounted(UWorld* World);
	void UpdateMouseAndWorldState(UWorld* World);
	void ToggleMatchRoomMenu();
	void CloseRoomEntryModal();
	void CopyRoomCodeToClipboard();
	bool HandleRoomKey(const FKeyEvent& Event);
	FString DetectLocalAddress(UWorld* World) const;
	FString GetLocalLanIpOnly() const;
	int32 GetConnectedPlayerCount(UWorld* World) const;
	bool IsInNetworkRoom(UWorld* World) const;

	// Legacy AutoLAN / HTTP backend compatibility (only engaged when -AutoLAN or -BackendUrl is explicitly used)
	void PrepareAutoRoom(bool bHost, const FString& Code);
	void CreatePreparedRoom();
	void QueryPreparedRoom(const FString& Code);
	void LocalAction(const FString& Action, const FString& Body = TEXT("{}"));
	void PublishReadyRoom();
	void OnHostRoomResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void OnJoinRoomResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, FString RequestedCode);
	void OnReserveSeatResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, FString TargetConnection, FString RoomCode);
	void OnStartMatchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);

	FString BackendUrl;
	FString CurrentManageToken;
	FString CurrentReservationId;
	FString ProtocolVersion = TEXT("1.0.0");
	FString PendingJoinCode;
	FString LocalServiceToken;
	FString SelectedForgeUrl;
	FString DiscoveredConnection;

	bool bAutoLAN = false;
	bool bLocalPreparing = false;
	bool bPublishWhenReady = false;
	bool bRoomEntryModalOpen = false;
	bool bReturnToModeOnFront = false;
	bool bMatchRoomExpanded = false;
	bool bSavedMatchCursor = false;
	bool bClientReadyLocked = false;
	bool bOverlayMounted = false;
	bool bWorldAutoNavigated = false;
	int32 SelectedCharacterIndex = 0; // 0=薇奥(Miyabi), 1=莱卡(Yousun), 2=凯恩(MyCharacter)
	double CopiedFeedbackUntil = 0.0;

	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> LocalPreparation;
	TSharedPtr<SWidget> OverlayWidget;
	TSharedPtr<SEditableTextBox> CodeInputBox;
	TSharedPtr<IInputProcessor> RoomKeyProcessor;
	UPROPERTY(Transient) TObjectPtr<UFont> DisplayFontAsset;
	UPROPERTY(Transient) TObjectPtr<UFont> MediumFontAsset;
	TWeakObjectPtr<UWorld> LastInitializedWorld;
	TWeakObjectPtr<UUserWidget> HookedModeWidget;
	TWeakObjectPtr<UUserWidget> HookedCharSelWidget;
	FDelegateHandle TickDelegateHandle;

#if !UE_BUILD_SHIPPING
	void TickE2ERoomTest(UWorld* World, float DeltaTime);
	FString E2ERole;
	FString E2EShotDir;
	int32 E2EStep = 0;
	double E2EStepSince = 0.0;
#endif
};
