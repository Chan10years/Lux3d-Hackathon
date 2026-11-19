#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "NetSessionSubsystem.generated.h"

class FOnlineSessionSearch;
class IOnlineIdentity;

DECLARE_LOG_CATEGORY_EXTERN(LogNetSession, Log, All);

/**
 * 显式会话状态机状态。
 * Idle        可发起开房/搜索/调试直连
 * Creating    正在创建会话
 * Finding     正在搜索局域网会话
 * Joining     正在加入会话
 * InLobby     已在大厅（ServerTravel 到达，或客户端连接成功）
 * Traveling   ServerTravel / 客户端连接进行中；会话跨地图保持
 * InMatch     已进入对局地图
 * Leaving     正在退出并销毁会话
 * Error       失败终态，可经重试入口（CreateRoom / FindAndJoinRoom / LeaveRoom）或 ResetSessionState 恢复
 */
UENUM(BlueprintType)
enum class ENetSessionState : uint8
{
	Idle UMETA(DisplayName = "Idle"),
	Creating UMETA(DisplayName = "Creating"),
	Finding UMETA(DisplayName = "Finding"),
	Joining UMETA(DisplayName = "Joining"),
	InLobby UMETA(DisplayName = "InLobby"),
	Traveling UMETA(DisplayName = "Traveling"),
	InMatch UMETA(DisplayName = "InMatch"),
	Leaving UMETA(DisplayName = "Leaving"),
	Error UMETA(DisplayName = "Error")
};

/** 可操作的失败码（供日志 / 未来 UI 显示重试或返回入口）。 */
UENUM(BlueprintType)
enum class ENetSessionError : uint8
{
	None UMETA(DisplayName = "None"),
	/** 本地身份未就绪，禁止开房/加入。 */
	IdentityNotReady UMETA(DisplayName = "IdentityNotReady"),
	/** OnlineSubsystem / 会话接口不可用。 */
	SubsystemUnavailable UMETA(DisplayName = "SubsystemUnavailable"),
	/** 状态机拒绝（重复点击、已有会话或进行中的异步操作）。 */
	BusyState UMETA(DisplayName = "BusyState"),
	CreateFailed UMETA(DisplayName = "CreateFailed"),
	FindFailed UMETA(DisplayName = "FindFailed"),
	/** 未发现任何局域网会话（可重试 NetFindJoinRoom）。 */
	NoSessionsFound UMETA(DisplayName = "NoSessionsFound"),
	JoinFailed UMETA(DisplayName = "JoinFailed"),
	DestroyFailed UMETA(DisplayName = "DestroyFailed"),
	/** 未能解析会话连接地址。 */
	ResolveConnectFailed UMETA(DisplayName = "ResolveConnectFailed"),
	/** ServerTravel 失败或客户端连接失败（可重试或返回入口）。 */
	TravelFailed UMETA(DisplayName = "TravelFailed"),
	/** 已在房的稳定状态（InLobby/InMatch）连接丢失，需清理本地会话后重试。 */
	Disconnected UMETA(DisplayName = "Disconnected"),
	InternalError UMETA(DisplayName = "InternalError")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnNetSessionStateChanged, ENetSessionState, NewState, ENetSessionError, LastError);

/**
 * 跨地图持久的 Null 局域网会话子系统（最多 4 人，房主权威 Listen Server）。
 *
 * 设计要点：
 * - 挂在 GameInstance 上：地图切换（ServerTravel / 客户端连接）不改变“是否在会话中”，
 *   只有 DestroySession 成功完成才回到 Idle（区分地图切换与真正离房）。
 * - 显式状态机：所有状态迁移集中在 IsTransitionAllowed 表中，非法迁移被拒绝并记日志。
 * - 每个异步操作绑定代数 OpId，过期回调（代数不匹配）与会话名不匹配的回调一律忽略，
 *   委托句柄在完成或 Deinitialize 时成对释放，防止重复开房、过期回调和失败后残留会话。
 * - 身份未就绪（IOnlineIdentity::GetLoginStatus != LoggedIn）时拒绝开房，给出可操作错误码。
 * - Null 的发现与连接只在本机/可达局域网内有效，不把 Null 会话标识当作可跨互联网解析的全局标识。
 *
 * 调试入口（控制台命令，Development 构建可用，UI 未完成时验证全流程）：
 *   NetCreateRoom          房主开房（4 人 LAN Listen 会话并 ServerTravel 进大厅）
 *   NetFindJoinRoom        客户端搜索并加入，解析地址后连接
 *   NetTravelMatch         房主 ServerTravel 进对局（会话跨地图保持）
 *   NetLeaveRoom           双方退出（房主 End + Destroy，客户端 Destroy）
 *   NetSessionState        打印当前状态/错误/会话信息
 *   NetSessionSelfTest     纯逻辑自动化自检（状态转换表、重复请求拒绝、过期回调防护）
 *   NetSessionConnect <ip:port>  调试直连（无效连接将给出明确失败与重试入口）
 *   NetSessionAuto <Host|Client|Stop>  无人值守双进程脚本
 * 命令行方式：-game NetAuto=Host / -game NetAuto=Client
 */
UCLASS()
class TRY_GLB_PUT_INTO_API UNetSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** 会话总人数上限（房主自身占 1 席）。 */
	static constexpr int32 SessionMaxPlayers = 4;

	// -- USubsystem --
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// -- 操作入口（控制台命令与未来 UI 共用） --

	/** 房主：创建 4 人局域网 Listen 会话并 ServerTravel 进入大厅。 */
	UFUNCTION(BlueprintCallable, Category = "NetSession")
	bool CreateRoom();

	/** 客户端：搜索局域网会话并加入最优结果，随后解析地址并连接。 */
	UFUNCTION(BlueprintCallable, Category = "NetSession")
	bool FindAndJoinRoom();

	/** 房主：ServerTravel 进入对局地图（会话跨地图保持）。 */
	UFUNCTION(BlueprintCallable, Category = "NetSession")
	bool TravelToMatch();

	/** 双方：退出并销毁会话（房主先 EndSession 再 DestroySession）。 */
	UFUNCTION(BlueprintCallable, Category = "NetSession")
	bool LeaveRoom();

	/** 失败恢复入口：使全部进行中回调过期，尽力清理残留会话并回到 Idle。 */
	UFUNCTION(BlueprintCallable, Category = "NetSession")
	bool ResetSessionState();

	/** 纯逻辑自动化自检：状态转换表、重复请求拒绝、过期回调与会话名不匹配防护。 */
	UFUNCTION(BlueprintCallable, Category = "NetSession")
	bool RunSelfTest();

	// -- 状态查询 --
	UFUNCTION(BlueprintPure, Category = "NetSession")
	ENetSessionState GetSessionState() const { return State; }

	UFUNCTION(BlueprintPure, Category = "NetSession")
	ENetSessionError GetLastSessionError() const { return LastError; }

	UFUNCTION(BlueprintPure, Category = "NetSession")
	FString GetLastErrorMessage() const;

	UFUNCTION(BlueprintPure, Category = "NetSession")
	bool IsRoomHost() const { return bIsHostFlag; }

	UFUNCTION(BlueprintPure, Category = "NetSession")
	FString GetStateDebugString() const;

	void SetDesiredRoomCode(const FString& InRoomCode) { DesiredRoomCode = InRoomCode; }
	const FString& GetDesiredRoomCode() const { return DesiredRoomCode; }

	/** 状态变更广播（供未来 UI 订阅）。 */
	UPROPERTY(BlueprintAssignable, Category = "NetSession")
	FOnNetSessionStateChanged OnNetSessionStateChanged;

	// -- 调试 / 自动化钩子 --
	void DebugDumpState(const FString& Context);
	bool DebugConnectToAddress(const FString& Address);
	void StartAutomation(FName Role);
	void StopAutomation();

	/** P0-1 证据：打印实际 GameSession 容量上限与当前有效玩家数。 */
	void DumpCapacityInfo() const;

	/** P1-6 受控测试钩子：让下一次 DestroySession 以失败回调完成（默认关闭）。 */
	void SetInjectDestroyFailure(bool bEnable) { bInjectDestroyFailure = bEnable; }

	/** 状态转换表（单一真值来源，RunSelfTest 逐项验证）。 */
	static bool IsTransitionAllowed(ENetSessionState From, ENetSessionState To);

	/** 身份就绪判定：接口有效且本地用户 0 已登录。 */
	static bool IsIdentityLoginReady(IOnlineIdentity* Identity);

	// -- 在线委托处理（含 OpId 过期防护；RunSelfTest 亦会调用） --
	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful, uint32 OpId);
	void HandleFindSessionsComplete(bool bWasSuccessful, uint32 OpId);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result, uint32 OpId);
	void HandleEndSessionComplete(FName SessionName, bool bWasSuccessful, uint32 OpId);
	void HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful, uint32 OpId);

private:
	/** 状态机主入口：非法迁移拒绝并记日志；进入新操作时清除旧错误。 */
	bool TrySetState(ENetSessionState NewState, ENetSessionError ErrorToRecord = ENetSessionError::None, const FString& Reason = FString());

	/** 进入 Error 终态（仅在忙/失败语境调用）。 */
	void FailCurrentOperation(ENetSessionError Error, const FString& Reason);

	/** 重复点击/状态不允许时的拒绝记录（不改变当前状态）。 */
	void RejectInCurrentState(const FString& RequestName, const FString& Reason);

	IOnlineSessionPtr GetSessionInterface() const;
	bool EnsureIdentityReady();
	bool EnsureSessionInterfaceAvailable();
	bool RequestCreateSession();
	bool RequestFindSessions();
	bool BeginDestroySession();
	void FinishLeaving();
	/** 回归 1 修复：客户端退出走双条件检查，确保 named session Remove 且 standalone 世界到达后才回到 Idle。 */
	void TryFinishLeaving();
	void ClearAllOperationHandles();

	/** P1-8：取消进行中的 LAN 搜索，并使迟到回调失效。 */
	bool CancelActiveSearch();
	/** P0-4：joined 客户端断开当前网络世界并返回本地地图（幂等）。 */
	bool DisconnectClientWorldToLocal();
	/** P1-5：调试直连仅在 Idle/Error 允许。 */
	bool IsManualConnectStateAllowed() const;

	/** World->ServerTravel 到大厅/对局，bListen=true 追加 ?listen 成为 Listen Server（创建房间），bListen=false 不追加（房主回到本地 standalone）。 */
	bool ServerTravelToMap(const FString& MapPath, const FString& ExtraOptions, bool bListen = true);
	/** 在世界上下文中清理所有 NetDriver/PendingNetGame 并切到本地地图；host 走 Listen 关闭路径，client 走 disconnect 路径。不依赖 PlayerController。 */
	bool CleanupNetWorldToLocal(UWorld* World);
	void HandlePostWorldInitialization(UWorld* World, const UWorld::InitializationValues IVS);
	/** 打包实测修复：阶段判定与 standalone 退出完成检测延后到 LoadMap+BeginPlay 后的 next-tick。 */
	void HandleWorldPostBeginPlay(UWorld* World);
	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);

	/** P0-1：GameSession 就绪即强制连接层容量 MaxPlayers=4（定时重试，成功后自停）。
	 *  必须传入当前世界（ServerTravel 后不得再取 GameInstance 的旧世界）。 */
	void EnforceGameSessionCapacity(UWorld* World);

	// -- 自动化脚本 --
	struct FAutoStep
	{
		float DelaySeconds = 0.f;
		FName Action;
		FString Arg;
	};
	void TickAutomationScheduling();
	void ExecuteCurrentAutomationStep();
	void ExecuteAutomationAction(const FAutoStep& Step);

	// -- 状态 --
	UPROPERTY(VisibleAnywhere, Category = "NetSession|Debug")
	ENetSessionState State = ENetSessionState::Idle;

	UPROPERTY(VisibleAnywhere, Category = "NetSession|Debug")
	ENetSessionError LastError = ENetSessionError::None;

	FString LastErrorDetail;

	/** 操作代数：每个异步请求自增；回调携带请求时代数，不匹配即过期丢弃。 */
	uint32 OperationIdCounter = 0;
	uint32 ActiveCreateOpId = 0;
	uint32 ActiveFindOpId = 0;
	uint32 ActiveJoinOpId = 0;
	uint32 ActiveEndOpId = 0;
	uint32 ActiveDestroyOpId = 0;

	bool bIsHostFlag = false;

	// 会话接口委托句柄（注册/释放成对）
	FDelegateHandle OnCreateSessionCompleteHandle;
	FDelegateHandle OnFindSessionsCompleteHandle;
	FDelegateHandle OnJoinSessionCompleteHandle;
	FDelegateHandle OnEndSessionCompleteHandle;
	FDelegateHandle OnDestroySessionCompleteHandle;
	FDelegateHandle OnCancelFindSessionsCompleteHandle;

	/** P1-8：取消尚未完成时禁止启动冲突搜索。 */
	bool bSearchCancelPending = false;

	/** 回归 1：客户端 named-session 退出时 ClientTravel 已发出，等待 standalone 世界到达。 */
	bool bClientLeavingWorldPending = false;
	/** 回归 1：客户端已回到 standalone 本地世界（OnPostWorldInitialization 检测到 NM_Standalone）。 */
	bool bClientWorldReturnedToLocal = false;

	/** P1-6 注入失败回调使用的定时器句柄（避免向 SetTimer 传右值）。 */
	FTimerHandle InjectDestroyFailureTimerHandle;

	/** 最近一次搜索结果（LAN）。 */
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	/** 销毁残留会话后的连锁动作（再次开房/再次搜索加入）。 */
	enum class EPostDestroyAction : uint8
	{
		None,
		ThenCreate,
		ThenFindJoin
	};
	EPostDestroyAction PendingPostDestroyAction = EPostDestroyAction::None;

	/** Lobby stays inside the SuoMao frontend map (WBP_CharSel); match travels to CityZZZ. */
	FString LobbyMapPath = TEXT("/Game/SuoMaoGame/Lvl_Front");
	FString MatchMapPath = TEXT("/Game/CityZZZ/Lvl_ZZZ");
	FString DesiredRoomCode;
	static constexpr const TCHAR* NetPhaseOption = TEXT("NetPhase=");
	static constexpr const TCHAR* NetPhaseMatchValue = TEXT("Match");

	/** 自检模式：抑制真实网络/世界操作。 */
	bool bSelfTestActive = false;

	/** 自检期间抑制状态广播，避免向外部 UI 发送瞬态迁移。 */
	bool bSuppressStateBroadcast = false;

	/** P1-6 测试钩子：下一次 DestroySession 以失败回调结束（默认关闭）。 */
	bool bInjectDestroyFailure = false;

	/** 世界/网络失败委托绑定标记。 */
	bool bWorldDelegatesBound = false;

	/** P0-1：GameSession 容量强制定时器（世界切换后由 HandlePostWorldInitialization 重设）。 */
	FTimerHandle CapacityEnforceTimerHandle;

	// -- 自动化 --
	bool bAutomationActive = false;
	bool bAutomationDoneLogged = false;
	FName AutomationRole;
	int32 AutomationStepIndex = 0;
	bool bAutomationStepScheduled = false;
	/** ConnectBad 等待 OnNetworkFailure 时暂停后续步骤（30 秒看门狗兜底）。 */
	bool bAutomationWaitingNetworkFailure = false;
	/** WaitForPlayers / WaitState 轮询计数（1 秒一轮，上限 90）。 */
	int32 AutomationWaitTicks = 0;
	FTimerHandle AutomationTimerHandle;
	FTimerHandle AutomationWatchdogHandle;
	/** 无人值守兜底挂在 core ticker 上，不随 World 销毁；ServerTravel 之后仍然有效。 */
	FTSTicker::FDelegateHandle AutomationTickerHandle;
	/** 脚本完成的时刻（FPlatformTime::Seconds）；大于 0 表示 SCRIPT_DONE 已记录。 */
	double AutomationDoneAt = 0.0;
	/** 脚本总截止时刻（FPlatformTime::Seconds）。 */
	double AutomationDeadline = 0.0;
	/** JoinHold/Probe 需要保持在线被外部结束，完成后不自动退出。 */
	bool bAutomationKeepAliveAfterDone = false;
	static constexpr float AutomationAutoExitDelaySeconds = 15.f;
	static constexpr float AutomationTimeoutSeconds = 240.f;
	TArray<FAutoStep> AutomationSteps;
};
