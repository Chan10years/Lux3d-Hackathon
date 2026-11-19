#include "NetSessionSubsystem.h"

#include "Engine/Engine.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogNetSession);

namespace
{
	const TCHAR* ToString(ENetSessionState State)
	{
		switch (State)
		{
		case ENetSessionState::Idle: return TEXT("Idle");
		case ENetSessionState::Creating: return TEXT("Creating");
		case ENetSessionState::Finding: return TEXT("Finding");
		case ENetSessionState::Joining: return TEXT("Joining");
		case ENetSessionState::InLobby: return TEXT("InLobby");
		case ENetSessionState::Traveling: return TEXT("Traveling");
		case ENetSessionState::InMatch: return TEXT("InMatch");
		case ENetSessionState::Leaving: return TEXT("Leaving");
		case ENetSessionState::Error: return TEXT("Error");
		default: return TEXT("Unknown");
		}
	}

	const TCHAR* ToString(ENetSessionError Error)
	{
		switch (Error)
		{
		case ENetSessionError::None: return TEXT("None");
		case ENetSessionError::IdentityNotReady: return TEXT("IdentityNotReady");
		case ENetSessionError::SubsystemUnavailable: return TEXT("SubsystemUnavailable");
		case ENetSessionError::BusyState: return TEXT("BusyState");
		case ENetSessionError::CreateFailed: return TEXT("CreateFailed");
		case ENetSessionError::FindFailed: return TEXT("FindFailed");
		case ENetSessionError::NoSessionsFound: return TEXT("NoSessionsFound");
		case ENetSessionError::JoinFailed: return TEXT("JoinFailed");
		case ENetSessionError::DestroyFailed: return TEXT("DestroyFailed");
		case ENetSessionError::ResolveConnectFailed: return TEXT("ResolveConnectFailed");
		case ENetSessionError::TravelFailed: return TEXT("TravelFailed");
		case ENetSessionError::Disconnected: return TEXT("Disconnected");
		case ENetSessionError::InternalError: return TEXT("InternalError");
		default: return TEXT("Unknown");
		}
	}

	struct FTransitionKey
	{
		ENetSessionState From;
		ENetSessionState To;
	};

	/**
	 * 合法状态迁移表（单一真值来源）。
	 * 注：X->Leaving 的 Creating/Finding/Joining/Idle/Traveling 迁移仅供
	 * ResetSessionState 恢复路径与残留清理链使用，LeaveRoom 在这些状态下会先拒绝。
	 */
	const TArray<FTransitionKey>& GetAllowedTransitions()
	{
		static const TArray<FTransitionKey> Allowed =
		{
			{ ENetSessionState::Idle,      ENetSessionState::Creating },
			{ ENetSessionState::Idle,      ENetSessionState::Finding },
			{ ENetSessionState::Idle,      ENetSessionState::Leaving },
			{ ENetSessionState::Idle,      ENetSessionState::Traveling },  // 调试直连入口
			{ ENetSessionState::Creating,  ENetSessionState::InLobby },
			{ ENetSessionState::Creating,  ENetSessionState::Error },
			{ ENetSessionState::Creating,  ENetSessionState::Leaving },
			{ ENetSessionState::Finding,   ENetSessionState::Joining },
			{ ENetSessionState::Finding,   ENetSessionState::Error },
			{ ENetSessionState::Finding,   ENetSessionState::Leaving },
			{ ENetSessionState::Joining,   ENetSessionState::Traveling },
			{ ENetSessionState::Joining,   ENetSessionState::Error },
			{ ENetSessionState::Joining,   ENetSessionState::Leaving },
			{ ENetSessionState::InLobby,   ENetSessionState::Traveling },
			{ ENetSessionState::InLobby,   ENetSessionState::InMatch },    // 跟随房主旅行
			{ ENetSessionState::InLobby,   ENetSessionState::Leaving },
			{ ENetSessionState::Traveling, ENetSessionState::InLobby },
			{ ENetSessionState::Traveling, ENetSessionState::InMatch },
			{ ENetSessionState::Traveling, ENetSessionState::Leaving },
			{ ENetSessionState::Traveling, ENetSessionState::Error },
			{ ENetSessionState::InMatch,   ENetSessionState::InLobby },    // 跟随房主旅行
			{ ENetSessionState::InMatch,   ENetSessionState::Traveling },
			{ ENetSessionState::InMatch,   ENetSessionState::Leaving },
			{ ENetSessionState::Leaving,   ENetSessionState::Idle },
			{ ENetSessionState::Leaving,   ENetSessionState::Error },
			{ ENetSessionState::Error,     ENetSessionState::Creating },
			{ ENetSessionState::Error,     ENetSessionState::Finding },
			{ ENetSessionState::Error,     ENetSessionState::Leaving },
			{ ENetSessionState::Error,     ENetSessionState::Idle },
			{ ENetSessionState::Error,     ENetSessionState::Traveling }   // 直连重试入口
		};
		return Allowed;
	}

	UNetSessionSubsystem* GetNetSessionSubsystemFromWorld(UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		UGameInstance* GameInstance = World->GetGameInstance();
		return GameInstance ? GameInstance->GetSubsystem<UNetSessionSubsystem>() : nullptr;
	}

	// -- 控制台命令处理（Development 构建始终可用，供 UI 完成前验证全流程） --
	void ConsoleNetCreateRoom(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->CreateRoom();
		}
	}

	void ConsoleNetFindJoinRoom(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->FindAndJoinRoom();
		}
	}

	void ConsoleNetTravelMatch(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->TravelToMatch();
		}
	}

	void ConsoleNetLeaveRoom(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->LeaveRoom();
		}
	}

	void ConsoleNetSessionState(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->DebugDumpState(TEXT("NetSessionState"));
		}
	}

	void ConsoleNetSessionSelfTest(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->RunSelfTest();
		}
	}

	void ConsoleNetSessionConnect(const TArray<FString>& Args, UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->DebugConnectToAddress(Args.Num() > 0 ? Args[0] : FString());
		}
	}

	void ConsoleNetSessionAuto(const TArray<FString>& Args, UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->StartAutomation(Args.Num() > 0 ? FName(*Args[0]) : FName(TEXT("Stop")));
		}
	}

	void ConsoleNetSessionCapacity(UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			Subsystem->DumpCapacityInfo();
		}
	}

	void ConsoleNetSessionInjectDestroyFailure(const TArray<FString>& Args, UWorld* World)
	{
		if (UNetSessionSubsystem* Subsystem = GetNetSessionSubsystemFromWorld(World))
		{
			const bool bEnable = Args.Num() == 0 || Args[0] != TEXT("0");
			Subsystem->SetInjectDestroyFailure(bEnable);
			UE_LOG(LogNetSession, Log, TEXT("[NetSession] INJECT_DESTROY_FAILURE armed=%d"), bEnable ? 1 : 0);
		}
	}

	FAutoConsoleCommandWithWorld GCmdNetCreateRoom(
		TEXT("NetCreateRoom"),
		TEXT("NetSession: create a 4-player LAN listen session and ServerTravel to the lobby (host)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetCreateRoom));

	FAutoConsoleCommandWithWorld GCmdNetFindJoinRoom(
		TEXT("NetFindJoinRoom"),
		TEXT("NetSession: search LAN sessions and join the best result, then resolve and connect (client)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetFindJoinRoom));

	FAutoConsoleCommandWithWorld GCmdNetTravelMatch(
		TEXT("NetTravelMatch"),
		TEXT("NetSession: host ServerTravel to the match map; the session persists across the map switch."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetTravelMatch));

	FAutoConsoleCommandWithWorld GCmdNetLeaveRoom(
		TEXT("NetLeaveRoom"),
		TEXT("NetSession: leave and destroy the session (host: End + Destroy; client: Destroy)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetLeaveRoom));

	FAutoConsoleCommandWithWorld GCmdNetSessionState(
		TEXT("NetSessionState"),
		TEXT("NetSession: dump the current session state machine, error code and session info."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetSessionState));

	FAutoConsoleCommandWithWorld GCmdNetSessionSelfTest(
		TEXT("NetSessionSelfTest"),
		TEXT("NetSession: run pure-logic self tests (transition table, duplicate request rejection, stale callback guards)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetSessionSelfTest));

	FAutoConsoleCommandWithWorldAndArgs GCmdNetSessionConnect(
		TEXT("NetSessionConnect"),
		TEXT("NetSession: connect directly to <ip:port> (debug; invalid addresses surface a clear failure and a retry entry)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ConsoleNetSessionConnect));

	FAutoConsoleCommandWithWorldAndArgs GCmdNetSessionAuto(
		TEXT("NetSessionAuto"),
		TEXT("NetSession: run the unattended two-process script: NetSessionAuto Host | Client | Stop."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ConsoleNetSessionAuto));

	FAutoConsoleCommandWithWorld GCmdNetSessionCapacity(
		TEXT("NetSessionCapacity"),
		TEXT("NetSession: dump the actual GameSession MaxPlayers and current player count (P0-1 evidence)."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ConsoleNetSessionCapacity));

	FAutoConsoleCommandWithWorldAndArgs GCmdNetSessionInjectDestroyFailure(
		TEXT("NetSessionInjectDestroyFailure"),
		TEXT("NetSession: arm the next DestroySession to complete with a failure callback (P1-6 test hook; 0 disarms)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ConsoleNetSessionInjectDestroyFailure));
}

// ---------------------------------------------------------------- lifecycle --

void UNetSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	FWorldDelegates::OnPostWorldInitialization.AddUObject(this, &UNetSessionSubsystem::HandlePostWorldInitialization);
	// 打包实测修复（11-05 基线包）：阶段判定与 standalone 退出完成检测不能在 InitWorld 阶段做——
	// 此时 World URL/netmode 仍是旧值、PendingNetGame 未清理、GameMode/GameSession 未 spawn。
	// 改为在 Init 时为每个到达的 World 注册下一个 tick 的回调（SetTimerForNextTick），
	// LoadMap → InitWorld → BeginPlay → 首次世界 tick 时触发，此时 URL/netmode/PendingNetGame 全部就绪。
	// UE5.8：网络失败事件位于 UEngine（原 FWorldDelegates::OnNetworkFailure 已移除）
	if (GEngine)
	{
		GEngine->OnNetworkFailure().AddUObject(this, &UNetSessionSubsystem::HandleNetworkFailure);
	}
	bWorldDelegatesBound = true;

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] INIT subsystem ready (session max players=%d, listen server, LAN-only discovery; GameSession capacity enforced via [/Script/Engine.GameSession] MaxPlayers)"), SessionMaxPlayers);

	// 命令行自动化：-game -NetAuto=<Role>（Host/Client/HostUat/ClientStay/... 统一解析，
	// 避免新增角色时漏加分支导致脚本静默不启动——本轮 UAT 实测即由此造成日志无 START）。
	FString AutomationRoleValue;
	if (FParse::Value(FCommandLine::Get(), TEXT("NetAuto="), AutomationRoleValue) && !AutomationRoleValue.IsEmpty())
	{
		StartAutomation(FName(*AutomationRoleValue));
	}
}

void UNetSessionSubsystem::Deinitialize()
{
	if (bWorldDelegatesBound)
	{
		FWorldDelegates::OnPostWorldInitialization.RemoveAll(this);
		if (GEngine)
		{
			GEngine->OnNetworkFailure().RemoveAll(this);
		}
		bWorldDelegatesBound = false;
	}
	// 子系统销毁时释放全部会话接口委托句柄，防止过期回调进入已销毁对象
	ClearAllOperationHandles();
	if (IOnlineSessionPtr Sessions = GetSessionInterface())
	{
		Sessions->ClearOnCancelFindSessionsCompleteDelegate_Handle(OnCancelFindSessionsCompleteHandle);
	}
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] DEINIT subsystem released"));
	Super::Deinitialize();
}

// ------------------------------------------------------------ state machine --

bool UNetSessionSubsystem::IsTransitionAllowed(ENetSessionState From, ENetSessionState To)
{
	for (const FTransitionKey& Key : GetAllowedTransitions())
	{
		if (Key.From == From && Key.To == To)
		{
			return true;
		}
	}
	return false;
}

bool UNetSessionSubsystem::TrySetState(ENetSessionState NewState, ENetSessionError ErrorToRecord, const FString& Reason)
{
	if (NewState == State)
	{
		if (ErrorToRecord != ENetSessionError::None)
		{
			LastError = ErrorToRecord;
			LastErrorDetail = Reason;
		}
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] STATE keep %s (error=%s %s)"), ToString(State), ToString(LastError), *LastErrorDetail);
		return true;
	}

	if (!IsTransitionAllowed(State, NewState))
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] TRANSITION_REJECTED %s -> %s (%s)"), ToString(State), ToString(NewState), *Reason);
		return false;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] STATE %s -> %s (error=%s %s)"), ToString(State), ToString(NewState), ToString(ErrorToRecord), *Reason);
	State = NewState;

	if (ErrorToRecord != ENetSessionError::None)
	{
		LastError = ErrorToRecord;
		LastErrorDetail = Reason;
	}
	else if (NewState == ENetSessionState::Idle ||
		NewState == ENetSessionState::Creating ||
		NewState == ENetSessionState::Finding ||
		NewState == ENetSessionState::Joining ||
		NewState == ENetSessionState::Traveling)
	{
		// 进入新操作或回到入口时清除旧错误；Error 态保留错误供展示重试入口
		LastError = ENetSessionError::None;
		LastErrorDetail.Reset();
	}

	if (!bSuppressStateBroadcast)
	{
		OnNetSessionStateChanged.Broadcast(State, LastError);
	}
	return true;
}

void UNetSessionSubsystem::FailCurrentOperation(ENetSessionError Error, const FString& Reason)
{
	UE_LOG(LogNetSession, Error, TEXT("[NetSession] FAIL error=%s detail='%s' (from %s; retry via CreateRoom/FindAndJoinRoom/LeaveRoom or ResetSessionState)"), ToString(Error), *Reason, ToString(State));
	if (!TrySetState(ENetSessionState::Error, Error, Reason))
	{
		// 兜底：即使迁移被拒也记录错误，保证日志可见
		LastError = Error;
		LastErrorDetail = Reason;
	}
}

void UNetSessionSubsystem::RejectInCurrentState(const FString& RequestName, const FString& Reason)
{
	LastError = ENetSessionError::BusyState;
	LastErrorDetail = Reason;
	UE_LOG(LogNetSession, Warning, TEXT("[NetSession] BUSY_REJECT %s while %s: %s"), *RequestName, ToString(State), *Reason);
}

bool UNetSessionSubsystem::IsIdentityLoginReady(IOnlineIdentity* Identity)
{
	return Identity != nullptr && Identity->GetLoginStatus(0) == ELoginStatus::LoggedIn;
}

IOnlineSessionPtr UNetSessionSubsystem::GetSessionInterface() const
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	return OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : IOnlineSessionPtr();
}

bool UNetSessionSubsystem::EnsureIdentityReady()
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	if (!OnlineSubsystem)
	{
		FailCurrentOperation(ENetSessionError::SubsystemUnavailable, TEXT("IOnlineSubsystem::Get() returned null; is OnlineSubsystemNull enabled?"));
		return false;
	}
	if (!IsIdentityLoginReady(OnlineSubsystem->GetIdentityInterface().Get()))
	{
		// 身份未就绪必须停止开房
		FailCurrentOperation(ENetSessionError::IdentityNotReady, TEXT("local identity not logged in; refusing to open/join a room"));
		return false;
	}
	return true;
}

bool UNetSessionSubsystem::EnsureSessionInterfaceAvailable()
{
	if (!GetSessionInterface().IsValid())
	{
		FailCurrentOperation(ENetSessionError::SubsystemUnavailable, TEXT("session interface unavailable"));
		return false;
	}
	return true;
}

void UNetSessionSubsystem::ClearAllOperationHandles()
{
	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		return;
	}
	Sessions->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteHandle);
	Sessions->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteHandle);
	Sessions->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteHandle);
	Sessions->ClearOnEndSessionCompleteDelegate_Handle(OnEndSessionCompleteHandle);
	Sessions->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteHandle);
}

// ----------------------------------------------------------------- requests --

bool UNetSessionSubsystem::CreateRoom()
{
	if (State != ENetSessionState::Idle && State != ENetSessionState::Error)
	{
		// 防重复开房：状态机直接拒绝重复点击/已有会话
		RejectInCurrentState(TEXT("CreateRoom"), TEXT("a room already exists or another operation is running"));
		return false;
	}
	if (bSelfTestActive)
	{
		// P0-3：自检期间不触碰在线接口/世界
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS CreateRoom"));
		return false;
	}
	if (!EnsureIdentityReady())
	{
		return false;
	}
	if (!EnsureSessionInterfaceAvailable())
	{
		return false;
	}

	// 清理上一次失败可能残留的会话，销毁完成后自动链式重开
	if (GetSessionInterface()->GetNamedSession(NAME_GameSession))
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CREATE_BLOCKED_BY_RESIDUAL destroying leftover session first, create will follow"));
		PendingPostDestroyAction = EPostDestroyAction::ThenCreate;
		if (!TrySetState(ENetSessionState::Leaving))
		{
			PendingPostDestroyAction = EPostDestroyAction::None;
			FailCurrentOperation(ENetSessionError::InternalError, TEXT("cannot enter Leaving to clean residual session"));
			return false;
		}
		return BeginDestroySession();
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CREATE_REQUEST state=%s"), ToString(State));
	return RequestCreateSession();
}

bool UNetSessionSubsystem::RequestCreateSession()
{
	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		FailCurrentOperation(ENetSessionError::SubsystemUnavailable, TEXT("session interface unavailable"));
		return false;
	}

	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS RequestCreateSession"));
		return false;
	}

	// 先进入 Creating 再注册回调：Null 的 CreateSession 可能同步完成，
	// 若在回调触发后才迁移状态，Creating -> InLobby 会因状态未就绪被拒。
	if (!TrySetState(ENetSessionState::Creating))
	{
		FailCurrentOperation(ENetSessionError::InternalError, TEXT("cannot enter Creating"));
		return false;
	}

	// 防御：注册前清掉旧句柄，保证注册/释放成对
	Sessions->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteHandle);

	const uint32 OpId = ++OperationIdCounter;
	ActiveCreateOpId = OpId;
	OnCreateSessionCompleteHandle = Sessions->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateLambda([this, OpId](FName InSessionName, bool bWasSuccessful)
		{
			HandleCreateSessionComplete(InSessionName, bWasSuccessful, OpId);
		}));

	// Null 局域网 Listen Server：4 个公共席位，房主注册自身占 1 席
	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections = SessionMaxPlayers;
	Settings.NumPrivateConnections = 0;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	Settings.bAllowJoinViaPresence = false;
	Settings.bUsesPresence = false;
	Settings.bIsLANMatch = true;          // UE5.8 字段名（旧名 bIsLAN）
	Settings.bIsDedicated = false;        // Listen Server（房主权威）
	Settings.bAntiCheatProtected = false;
	Settings.bAllowInvites = false;
	Settings.bUsesStats = false;
	if (!DesiredRoomCode.IsEmpty())
	{
		Settings.Set(FName(TEXT("ROOMCODE")), DesiredRoomCode, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	}
	// P0-2 根因修复：FOnlineSessionSettings 构造时已按 GetBuildUniqueId() 填充 BuildUniqueId，
	// 这里覆盖为固定值会让客户端在 OnValidResponsePacketReceived 的 BuildUniqueId 校验中
	// 静默丢弃全部 LAN 应答（results=0）。必须保留引擎默认值。
	const int32 EffectiveBuildId = Settings.BuildUniqueId;

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CREATE_SENT op=%u max=%d lan=1 dedicated=0 listen=1 buildId=%d"),
		OpId, SessionMaxPlayers, EffectiveBuildId);

	const bool bRequested = Sessions->CreateSession(0, NAME_GameSession, Settings);
	if (!bRequested && ActiveCreateOpId == OpId)
	{
		// Null 通常同步触发完成回调；仅当回调未处理时在此清理，避免二次状态迁移
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteHandle);
		ActiveCreateOpId = 0;
		FailCurrentOperation(ENetSessionError::CreateFailed, TEXT("CreateSession returned false before completion"));
		return false;
	}
	return true;
}

bool UNetSessionSubsystem::FindAndJoinRoom()
{
	if (State != ENetSessionState::Idle && State != ENetSessionState::Error)
	{
		RejectInCurrentState(TEXT("FindAndJoinRoom"), TEXT("a room already exists or another operation is running"));
		return false;
	}
	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS FindAndJoinRoom"));
		return false;
	}
	if (!EnsureIdentityReady())
	{
		return false;
	}
	if (!EnsureSessionInterfaceAvailable())
	{
		return false;
	}

	if (GetSessionInterface()->GetNamedSession(NAME_GameSession))
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_BLOCKED_BY_RESIDUAL destroying leftover session first, find+join will follow"));
		PendingPostDestroyAction = EPostDestroyAction::ThenFindJoin;
		if (!TrySetState(ENetSessionState::Leaving))
		{
			PendingPostDestroyAction = EPostDestroyAction::None;
			FailCurrentOperation(ENetSessionError::InternalError, TEXT("cannot enter Leaving to clean residual session"));
			return false;
		}
		return BeginDestroySession();
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_REQUEST state=%s"), ToString(State));
	return RequestFindSessions();
}

bool UNetSessionSubsystem::RequestFindSessions()
{
	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		FailCurrentOperation(ENetSessionError::SubsystemUnavailable, TEXT("session interface unavailable"));
		return false;
	}

	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS RequestFindSessions"));
		return false;
	}

	// P1-8：上一次 CancelFindSessions 尚未完成时会与这次搜索冲突，明确返回可重试状态
	if (bSearchCancelPending)
	{
		RejectInCurrentState(TEXT("RequestFindSessions"), TEXT("previous search cancellation is still pending; retry shortly"));
		return false;
	}

	// 先进入 Finding 再注册回调：与 CreateSession 同理，避免同步完成时状态未就绪
	if (!TrySetState(ENetSessionState::Finding))
	{
		FailCurrentOperation(ENetSessionError::InternalError, TEXT("cannot enter Finding"));
		return false;
	}

	Sessions->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteHandle);

	const uint32 OpId = ++OperationIdCounter;
	ActiveFindOpId = OpId;
	OnFindSessionsCompleteHandle = Sessions->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateLambda([this, OpId](bool bWasSuccessful)
		{
			HandleFindSessionsComplete(bWasSuccessful, OpId);
		}));

	SessionSearch = MakeShareable(new FOnlineSessionSearch());
	SessionSearch->bIsLanQuery = true;       // Null 的发现仅限本机/可达局域网
	SessionSearch->MaxSearchResults = 64;

	const bool bRequested = Sessions->FindSessions(0, SessionSearch.ToSharedRef());
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_SENT op=%u lan=1"), OpId);
	if (!bRequested && ActiveFindOpId == OpId)
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteHandle);
		ActiveFindOpId = 0;
		FailCurrentOperation(ENetSessionError::FindFailed, TEXT("FindSessions returned false before completion"));
		return false;
	}
	return true;
}

bool UNetSessionSubsystem::TravelToMatch()
{
	if (!bIsHostFlag)
	{
		RejectInCurrentState(TEXT("TravelToMatch"), TEXT("only the room host may ServerTravel"));
		return false;
	}
	if (State != ENetSessionState::InLobby && State != ENetSessionState::InMatch)
	{
		RejectInCurrentState(TEXT("TravelToMatch"), TEXT("host is not in a stable in-room state"));
		return false;
	}
	if (bSelfTestActive)
	{
		// P0-3：自检禁止真实 ServerTravel
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS TravelToMatch (target '%s')"), *MatchMapPath);
		return false;
	}
	// 房主权威：携带 ?listen、InitOptions ?MaxPlayers=4 与 NetPhase=Match 选项 ServerTravel，会话跨地图保持
	return ServerTravelToMap(MatchMapPath, FString::Printf(TEXT("?MaxPlayers=%d?%s%s"), SessionMaxPlayers, NetPhaseOption, NetPhaseMatchValue));
}

bool UNetSessionSubsystem::LeaveRoom()
{
	// P0-3：自检隔离必须在函数入口生效，任何分支（含无会话/接口不可用）都不得产生真实断连/销毁副作用。
	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS LeaveRoom"));
		return false;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();

	if (State == ENetSessionState::Idle)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] LEAVE_SKIP already Idle (nothing to leave)"));
		return true;
	}
	if (State == ENetSessionState::Leaving)
	{
		RejectInCurrentState(TEXT("LeaveRoom"), TEXT("leaving already in progress"));
		return false;
	}
	if (State == ENetSessionState::Creating || State == ENetSessionState::Finding || State == ENetSessionState::Joining)
	{
		RejectInCurrentState(TEXT("LeaveRoom"), TEXT("async operation in progress; wait for completion or use ResetSessionState"));
		return false;
	}

	if (!Sessions.IsValid())
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] LEAVE_FORCED session interface unavailable; returning to entry"));
		const bool bDisconnectRequested = DisconnectClientWorldToLocal();
		if (bDisconnectRequested)
		{
			bClientLeavingWorldPending = true;
			bClientWorldReturnedToLocal = false;
		}
		else
		{
			// 没有旅行请求：当前世界已是本地世界，允许 Idle 门禁通过 localReady
			bClientLeavingWorldPending = false;
			bClientWorldReturnedToLocal = true;
		}
		ClearAllOperationHandles();
		PendingPostDestroyAction = EPostDestroyAction::None;
		TrySetState(ENetSessionState::Leaving);
		FinishLeaving();
		return true;
	}

	const FNamedOnlineSession* NamedSession = Sessions->GetNamedSession(NAME_GameSession);
	if (!NamedSession)
	{
		// 例如搜索失败后的 Error 态：没有可销毁的会话，直接回到入口
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] LEAVE_NO_SESSION returning to Idle directly"));
		// 直连模式下没有本地 named session：若仍在客户端网络世界，主动断开连接回本地
		const bool bDisconnectRequested = DisconnectClientWorldToLocal();
		if (bDisconnectRequested)
		{
			bClientLeavingWorldPending = true;
			bClientWorldReturnedToLocal = false;
		}
		else
		{
			bClientLeavingWorldPending = false;
			bClientWorldReturnedToLocal = true;
		}
		ClearAllOperationHandles();
		PendingPostDestroyAction = EPostDestroyAction::None;
		TrySetState(ENetSessionState::Leaving);
		FinishLeaving();
		return true;
	}

	const bool bHosting = NamedSession->bHosting;
	const EOnlineSessionState::Type NamedSessionState = NamedSession->SessionState;

	if (!TrySetState(ENetSessionState::Leaving))
	{
		FailCurrentOperation(ENetSessionError::InternalError, TEXT("cannot enter Leaving"));
		return false;
	}

	if (bHosting)
	{
		// P1-7 会话 End 语义：本实现从不调用 StartSession，Null 创建的会话停留在 Pending，
		// 对 Pending 调用 EndSession 必然失败并打印 "Can't end session in state Pending"。
		// 未开始（非 InProgress）的会话直接 Destroy；只有真正 InProgress 才 End -> Destroy。
		if (NamedSessionState != EOnlineSessionState::InProgress)
		{
			UE_LOG(LogNetSession, Log, TEXT("[NetSession] LEAVE_SKIP_END host sessionState=%d (not InProgress; destroying directly)"),
				static_cast<int32>(NamedSessionState));
			return BeginDestroySession();
		}

		// 房主：先 EndSession（结束会话），完成后再 DestroySession（销毁）
		Sessions->ClearOnEndSessionCompleteDelegate_Handle(OnEndSessionCompleteHandle);
		const uint32 OpId = ++OperationIdCounter;
		ActiveEndOpId = OpId;
		OnEndSessionCompleteHandle = Sessions->AddOnEndSessionCompleteDelegate_Handle(
			FOnEndSessionCompleteDelegate::CreateLambda([this, OpId](FName InSessionName, bool bWasSuccessful)
			{
				HandleEndSessionComplete(InSessionName, bWasSuccessful, OpId);
			}));
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] LEAVE_REQUEST host EndSession op=%u"), OpId);
		const bool bRequested = Sessions->EndSession(NAME_GameSession);
		if (!bRequested && ActiveEndOpId == OpId)
		{
			UE_LOG(LogNetSession, Warning, TEXT("[NetSession] EndSession returned false; falling back to DestroySession"));
			Sessions->ClearOnEndSessionCompleteDelegate_Handle(OnEndSessionCompleteHandle);
			ActiveEndOpId = 0;
			return BeginDestroySession();
		}
		return true;
	}

	// P0-4：joined 客户端（named session 非 hosting）——必须先断开网络世界再销毁会话记录，
	// 否则会出现“会话记录已销毁但仍停留在 NM_Client”的悬挂状态。
	// 回归 1 修复：DisconnectClientWorldToLocal 返回 true 表示 ClientTravel 已发出，
	// 后续 TryFinishLeaving 必须同时等待 DestroySession 回调 + standalone 世界到达才回到 Idle。
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLIENT_NAMED_EXIT named session present (hosting=0); disconnecting then destroying"));
	const bool bDisconnectRequested = DisconnectClientWorldToLocal();
	if (bDisconnectRequested)
	{
		bClientLeavingWorldPending = true;
		bClientWorldReturnedToLocal = false;
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLIENT_LEAVE_WORLD_PENDING waiting for standalone arrival after DestroySession"));
	}
	return BeginDestroySession();
}

bool UNetSessionSubsystem::BeginDestroySession()
{
	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS BeginDestroySession"));
		return false;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] DESTROY_FORCED session interface unavailable; returning to entry"));
		PendingPostDestroyAction = EPostDestroyAction::None;
		TrySetState(ENetSessionState::Leaving);
		FinishLeaving();
		return true;
	}

	Sessions->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteHandle);

	const uint32 OpId = ++OperationIdCounter;
	ActiveDestroyOpId = OpId;
	OnDestroySessionCompleteHandle = Sessions->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateLambda([this, OpId](FName InSessionName, bool bWasSuccessful)
		{
			HandleDestroySessionComplete(InSessionName, bWasSuccessful, OpId);
		}));

	// P1-6 受控测试钩子：不调用真实 DestroySession，直接以失败回调走失败清理路径，
	// 用于证明失败后 PendingPostDestroyAction 被清除、后续重试不会执行旧动作。默认关闭。
	if (bInjectDestroyFailure)
	{
		bInjectDestroyFailure = false;
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] INJECT_DESTROY_FAILURE op=%u (bypassing real DestroySession; delivering failure callback)"), OpId);
		UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		if (World)
		{
			World->GetTimerManager().SetTimer(
				InjectDestroyFailureTimerHandle,
				FTimerDelegate::CreateWeakLambda(this, [this, OpId]()
				{
					HandleDestroySessionComplete(NAME_GameSession, false, OpId);
				}),
				0.01f,
				false);
		}
		else
		{
			HandleDestroySessionComplete(NAME_GameSession, false, OpId);
		}
		return true;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] DESTROY_SENT op=%u session='%s'"), OpId, *FName(NAME_GameSession).ToString());
	const bool bRequested = Sessions->DestroySession(NAME_GameSession);
	if (!bRequested && ActiveDestroyOpId == OpId)
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteHandle);
		ActiveDestroyOpId = 0;
		PendingPostDestroyAction = EPostDestroyAction::None;
		FailCurrentOperation(ENetSessionError::DestroyFailed, TEXT("DestroySession returned false before completion"));
		return false;
	}
	return true;
}

void UNetSessionSubsystem::FinishLeaving()
{
	// 回归 1 修复：不再无条件回 Idle，改为双条件检查。
	TryFinishLeaving();
}

void UNetSessionSubsystem::TryFinishLeaving()
{
	// 自检是纯逻辑测试：不检查真实世界/会话，不产生副作用。调用方已开启 bSelfTestActive。
	if (bSelfTestActive)
	{
		bIsHostFlag = false;
		PendingPostDestroyAction = EPostDestroyAction::None;
		bClientLeavingWorldPending = false;
		bClientWorldReturnedToLocal = false;
		TrySetState(ENetSessionState::Idle);
		return;
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	IOnlineSessionPtr Sessions = GetSessionInterface();

	// 实测修复（0703b6b 包内）：Idle 门禁必须确认「本端世界已就绪」，不能把
	// NetDriver 被 HandleDisconnect 销毁后 GetNetMode() 回退的 Standalone 当作到达。
	// A. 本端本地世界已到达：bClientWorldReturnedToLocal 由新世界 post-BeginPlay 置位
	// B. 当前世界为 Standalone 且无 NetDriver
	// C. 本地无待旅行（WorldContext.TravelURL 为空），防止 LoadMap 前提前放行
	// D. named session 已移除
	// E. 无 PendingNetGame
	// F. 房主 ListenServer 远端连接已清空
	const bool bStandaloneWorld = World && World->GetNetMode() == NM_Standalone && World->GetNetDriver() == nullptr;

	bool bNoQueuedTravel = true;
	if (GEngine && World)
	{
		if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
		{
			bNoQueuedTravel = Context->TravelURL.IsEmpty() && Context->PendingNetGame == nullptr;
		}
	}

	const FNamedOnlineSession* NamedSession = Sessions.IsValid() ? Sessions->GetNamedSession(NAME_GameSession) : nullptr;
	const bool bNamedSessionGone = NamedSession == nullptr;

	int32 PendingCount = 0;
	if (GEngine)
	{
		for (const FWorldContext& WC : GEngine->GetWorldContexts())
		{
			if (WC.PendingNetGame) { ++PendingCount; }
		}
	}
	const bool bNoPendingConnections = PendingCount == 0;

	int32 NetConns = 0;
	if (World && World->GetNetDriver())
	{
		for (UNetConnection* C : World->GetNetDriver()->ClientConnections) { ++NetConns; }
	}
	const bool bHostNetClean = !World || World->GetNetMode() != NM_ListenServer || NetConns == 0;

	const bool bLocalWorldReady = bClientWorldReturnedToLocal;
	const bool bAllConditionsMet = bLocalWorldReady && bStandaloneWorld && bNoQueuedTravel
		&& bNamedSessionGone && bNoPendingConnections && bHostNetClean;

	if (!bAllConditionsMet)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] LEAVE_PARTIAL waiting localReady=%d standalone=%d noQueuedTravel=%d namedGone=%d noPending=%d hostNetClean=%d"),
			bLocalWorldReady ? 1 : 0, bStandaloneWorld ? 1 : 0, bNoQueuedTravel ? 1 : 0,
			bNamedSessionGone ? 1 : 0, bNoPendingConnections ? 1 : 0, bHostNetClean ? 1 : 0);
		return;
	}

	bIsHostFlag = false;
	PendingPostDestroyAction = EPostDestroyAction::None;
	bClientLeavingWorldPending = false;
	bClientWorldReturnedToLocal = false;

	if (TrySetState(ENetSessionState::Idle))
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] LEAVE_OK all-conditions met (localWorld+standalone+noQueuedTravel+namedGone+noPending+hostNetClean) → Idle; a new room can be created now"));
	}
}

bool UNetSessionSubsystem::ResetSessionState()
{
	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS ResetSessionState"));
		return false;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] RESET_REQUEST from state=%s"), ToString(State));

	// P1-8：先取消进行中的搜索（Null 实现会同步 finalize，其他实现可能异步），
	// 并使所有在途回调代数过期，迟到回调一律丢弃，不会混入新搜索。
	CancelActiveSearch();

	++OperationIdCounter;
	ActiveCreateOpId = 0;
	ActiveFindOpId = 0;
	ActiveJoinOpId = 0;
	ActiveEndOpId = 0;
	ActiveDestroyOpId = 0;
	PendingPostDestroyAction = EPostDestroyAction::None;
	// Blind review P2：ResetSessionState 在已加入的客户端世界中应先 DisconnectClientWorldToLocal。
	// 原实现直接清掉客户端离开标记 + DestroySession，销毁完进 Idle 但客户端仍连旧房主；
	// 随后 CreateRoom 因 World 是客户端没有 AuthGameMode 无法 ServerTravel。
	// 修复：在清除标记前先判断 netmode 并触发 Disconnect；Disconnect 成功后置 bClientLeavingWorldPending。
	UWorld* CurrentWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const bool bConnectedClientWorld = CurrentWorld && CurrentWorld->GetNetMode() == NM_Client;
	if (bConnectedClientWorld)
	{
		const bool bDisconnectRequested = DisconnectClientWorldToLocal();
		if (bDisconnectRequested)
		{
			bClientLeavingWorldPending = true;
			bClientWorldReturnedToLocal = false;
			UE_LOG(LogNetSession, Log, TEXT("[NetSession] RESET_CLIENT_DISCONNECT initiated; waiting for standalone arrival after DestroySession"));
		}
		else
		{
			UE_LOG(LogNetSession, Warning, TEXT("[NetSession] RESET_CLIENT_DISCONNECT ClientTravel rejected; destroy will proceed but world may stay net-connected"));
			// 即使 ClientTravel 被拒，也保留 pending 让 TryFinishLeaving 双条件检查能检测到异常；
			// 若没设 pending，TryFinishLeaving 会无条件放行进 Idle（回归 1 的反向）。
			bClientLeavingWorldPending = true;
			bClientWorldReturnedToLocal = false;
		}
	}
	else
	{
		// 非客户端世界（房主 / standalone）：Reset 直接清掉客户端离开追踪标记
		bClientLeavingWorldPending = false;
		bClientWorldReturnedToLocal = false;
	}
	ClearAllOperationHandles();

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && Sessions->GetNamedSession(NAME_GameSession))
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] RESET destroying residual session"));
		if (!TrySetState(ENetSessionState::Leaving))
		{
			FailCurrentOperation(ENetSessionError::InternalError, TEXT("cannot enter Leaving during reset"));
			return false;
		}
		return BeginDestroySession();
	}

	bIsHostFlag = false;
	TrySetState(ENetSessionState::Leaving);
	FinishLeaving();
	return true;
}

bool UNetSessionSubsystem::CancelActiveSearch()
{
	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		return false;
	}

	const bool bSearching = (State == ENetSessionState::Finding) || (ActiveFindOpId != 0) || bSearchCancelPending;
	if (!bSearching)
	{
		return true;
	}

	// 先使当前搜索的回调过期，保证任何迟到结果都不会被当成新搜索完成
	++OperationIdCounter;
	ActiveFindOpId = 0;
	bSearchCancelPending = true;

	// 取消完成委托只注册一次，独立于各操作句柄的清理，确保异步引擎实现也能解除等待
	if (!OnCancelFindSessionsCompleteHandle.IsValid())
	{
		OnCancelFindSessionsCompleteHandle = Sessions->AddOnCancelFindSessionsCompleteDelegate_Handle(
			FOnCancelFindSessionsCompleteDelegate::CreateWeakLambda(this, [this](bool bWasSuccessful)
			{
				UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_CANCEL_DONE success=%d (conflicting searches re-enabled)"), bWasSuccessful ? 1 : 0);
				bSearchCancelPending = false;
			}));
	}

	const bool bCancelRequested = Sessions->CancelFindSessions();
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_CANCEL_REQUEST accepted=%d pending=%d"), bCancelRequested ? 1 : 0, bSearchCancelPending ? 1 : 0);
	// 回归 2 修复：取消失败时**不要**清 bSearchCancelPending。
	// 底层搜索仍在进行，旧 FindSessionsComplete 回调可能迟到；
	// 必须等到旧回调到达（含 StaleCallback 路径）或 Cancel 成功后才释放所有权，
	// 避免允许新搜索进来重置 ActiveFindOpId，导致旧回调被静默丢弃。
	if (!bCancelRequested)
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] FIND_CANCEL_REJECTED by backend; ownership retained until old FindSessionsComplete arrives"));
	}
	return bCancelRequested;
}

bool UNetSessionSubsystem::DisconnectClientWorldToLocal()
{
	// 执行单末节：改用 CleanupNetWorldToLocal 统一 world-context 断连路径（不依赖 PlayerController）。
	// 旧实现直接 PlayerController->ClientTravel；新实现走 UEngine::HandleDisconnect + LocalPlayer->SetClientTravel。
	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (!World || World->GetNetMode() != NM_Client)
	{
		// 已经在本地世界：幂等，无需断连
		return false;
	}
	return CleanupNetWorldToLocal(World);
}

bool UNetSessionSubsystem::IsManualConnectStateAllowed() const
{
	// P1-5：只有干净的入口状态允许调试直连；Creating/Finding/Joining/Leaving/Traveling/InLobby/InMatch 一律拒绝
	return State == ENetSessionState::Idle || State == ENetSessionState::Error;
}

// ----------------------------------------------------------------- handlers --

void UNetSessionSubsystem::HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful, uint32 OpId)
{
	if (OpId != ActiveCreateOpId)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] STALE_CALLBACK create op=%u (active=%u) ignored"), OpId, ActiveCreateOpId);
		return;
	}
	if (SessionName != NAME_GameSession)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] create callback for foreign session '%s' ignored"), *SessionName.ToString());
		return;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && !bSelfTestActive)
	{
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(OnCreateSessionCompleteHandle);
	}
	ActiveCreateOpId = 0;

	if (!bWasSuccessful)
	{
		FailCurrentOperation(ENetSessionError::CreateFailed, TEXT("OnCreateSessionComplete reported failure"));
		return;
	}

	bIsHostFlag = true;
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CREATE_OK session='%s' max=%d host"), *SessionName.ToString(), SessionMaxPlayers);

	if (bSelfTestActive)
	{
		// 自检模式：不触碰世界，仅验证状态迁移
		TrySetState(ENetSessionState::InLobby);
		return;
	}

	if (!TrySetState(ENetSessionState::InLobby))
	{
		FailCurrentOperation(ENetSessionError::InternalError, TEXT("Creating -> InLobby rejected"));
		return;
	}

	// 受支持的 ServerTravel 流程：携带 ?listen 与 InitOptions ?MaxPlayers=4 进入大厅（会话保持）
	// 执行单末节：默认容量在 Game 配置与 Listen URL InitOptions 双处显式设置。
	if (!ServerTravelToMap(LobbyMapPath, FString::Printf(TEXT("?MaxPlayers=%d"), SessionMaxPlayers)))
	{
		FailCurrentOperation(ENetSessionError::TravelFailed, TEXT("ServerTravel to lobby failed after create"));
	}
}

void UNetSessionSubsystem::HandleFindSessionsComplete(bool bWasSuccessful, uint32 OpId)
{
	if (OpId != ActiveFindOpId)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] STALE_CALLBACK find op=%u (active=%u) ignored"), OpId, ActiveFindOpId);
		// 回归 2 修复：取消等待中的旧搜索回调到达——这是旧搜索真正结束的信号。
		// CancelActiveSearch 已通过 ++OperationIdCounter 使旧回调过期，
		// StaleCallback 路径保证迟到结果不会污染新搜索；这里清 bSearchCancelPending
		// 释放所有权，允许 RequestFindSessions 放行新搜索。
		if (bSearchCancelPending)
		{
			bSearchCancelPending = false;
			UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_OWNERSHIP_RELEASED stale callback arrived; conflicting searches re-enabled"));
		}
		return;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid())
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(OnFindSessionsCompleteHandle);
	}
	ActiveFindOpId = 0;

	if (!bWasSuccessful || !SessionSearch.IsValid())
	{
		FailCurrentOperation(ENetSessionError::FindFailed, TEXT("OnFindSessionsComplete reported failure"));
		return;
	}

	const int32 NumResults = SessionSearch->SearchResults.Num();
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] FIND_DONE results=%d"), NumResults);
	if (NumResults == 0)
	{
		// 可操作失败：确认房主已开房后重试 NetFindJoinRoom
		FailCurrentOperation(ENetSessionError::NoSessionsFound, TEXT("no LAN session found; start the host first, then retry NetFindJoinRoom"));
		return;
	}

	// 若指定了 6 位房号，精确匹配局域网广播的 ROOMCODE；否则按原逻辑选延迟最低
	const FOnlineSessionSearchResult* Best = nullptr;
	if (!DesiredRoomCode.IsEmpty())
	{
		for (const FOnlineSessionSearchResult& Result : SessionSearch->SearchResults)
		{
			FString AdvertisedCode;
			if (Result.Session.SessionSettings.Get(FName(TEXT("ROOMCODE")), AdvertisedCode)
				&& AdvertisedCode.Equals(DesiredRoomCode, ESearchCase::IgnoreCase))
			{
				Best = &Result;
				break;
			}
		}
		if (!Best)
		{
			FailCurrentOperation(ENetSessionError::NoSessionsFound,
				FString::Printf(TEXT("no LAN room matching code %s"), *DesiredRoomCode));
			return;
		}
	}
	else
	{
		Best = &SessionSearch->SearchResults[0];
		for (const FOnlineSessionSearchResult& Result : SessionSearch->SearchResults)
		{
			if (Result.PingInMs < Best->PingInMs)
			{
				Best = &Result;
			}
		}
	}
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] JOIN_REQUEST best ping=%d of %d targetSessionId='%s' openPublic=%d"),
		Best->PingInMs, NumResults,
		Best->Session.SessionInfo.IsValid() ? *Best->Session.SessionInfo->GetSessionId().ToString() : TEXT("<unknown>"),
		Best->Session.NumOpenPublicConnections);

	if (!TrySetState(ENetSessionState::Joining))
	{
		FailCurrentOperation(ENetSessionError::InternalError, TEXT("Finding -> Joining rejected"));
		return;
	}

	Sessions->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteHandle);
	const uint32 JoinOpId = ++OperationIdCounter;
	ActiveJoinOpId = JoinOpId;
	OnJoinSessionCompleteHandle = Sessions->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateLambda([this, JoinOpId](FName InSessionName, EOnJoinSessionCompleteResult::Type Result)
		{
			HandleJoinSessionComplete(InSessionName, Result, JoinOpId);
		}));

	const bool bRequested = Sessions->JoinSession(0, NAME_GameSession, *Best);
	if (!bRequested && ActiveJoinOpId == JoinOpId)
	{
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteHandle);
		ActiveJoinOpId = 0;
		FailCurrentOperation(ENetSessionError::JoinFailed, TEXT("JoinSession returned false before completion"));
	}
}

void UNetSessionSubsystem::HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result, uint32 OpId)
{
	if (OpId != ActiveJoinOpId)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] STALE_CALLBACK join op=%u (active=%u) ignored"), OpId, ActiveJoinOpId);
		return;
	}
	if (SessionName != NAME_GameSession)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] join callback for foreign session '%s' ignored"), *SessionName.ToString());
		return;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid())
	{
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(OnJoinSessionCompleteHandle);
	}
	ActiveJoinOpId = 0;

	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		FailCurrentOperation(ENetSessionError::JoinFailed, FString::Printf(TEXT("OnJoinSessionComplete result=%d"), static_cast<int32>(Result)));
		return;
	}

	bIsHostFlag = false;
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] JOIN_OK session='%s'"), *SessionName.ToString());

	if (bSelfTestActive)
	{
		// 自检模式：仅验证 Joining -> Traveling 迁移
		TrySetState(ENetSessionState::Traveling);
		return;
	}

	// 客户端解析会话连接地址并连接
	FString ConnectString;
	if (!Sessions.IsValid() ||
		!Sessions->GetResolvedConnectString(NAME_GameSession, ConnectString, NAME_GamePort) ||
		ConnectString.IsEmpty())
	{
		FailCurrentOperation(ENetSessionError::ResolveConnectFailed, TEXT("could not resolve connect string after join"));
		return;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] RESOLVE_OK session='%s' connectString='%s'"), *SessionName.ToString(), *ConnectString);

	APlayerController* PlayerController = GetGameInstance() ? GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
	if (!PlayerController)
	{
		FailCurrentOperation(ENetSessionError::InternalError, TEXT("no local player controller for ClientTravel"));
		return;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CONNECT_REQUEST addr='%s'"), *ConnectString);
	PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLIENTTRAVEL_SENT addr='%s'"), *ConnectString);
	TrySetState(ENetSessionState::Traveling);
}

void UNetSessionSubsystem::HandleEndSessionComplete(FName SessionName, bool bWasSuccessful, uint32 OpId)
{
	if (OpId != ActiveEndOpId)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] STALE_CALLBACK end op=%u (active=%u) ignored"), OpId, ActiveEndOpId);
		return;
	}
	if (SessionName != NAME_GameSession)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] end callback for foreign session '%s' ignored"), *SessionName.ToString());
		return;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid())
	{
		Sessions->ClearOnEndSessionCompleteDelegate_Handle(OnEndSessionCompleteHandle);
	}
	ActiveEndOpId = 0;

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] END_DONE success=%d"), bWasSuccessful ? 1 : 0);
	if (bSelfTestActive)
	{
		// P0-3：自检期间不发起真实 DestroySession
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS End->Destroy"));
		return;
	}
	// 无论 End 结果如何都继续销毁，避免残留会话
	BeginDestroySession();
}

void UNetSessionSubsystem::HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful, uint32 OpId)
{
	if (OpId != ActiveDestroyOpId)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] STALE_CALLBACK destroy op=%u (active=%u) ignored"), OpId, ActiveDestroyOpId);
		return;
	}
	if (SessionName != NAME_GameSession)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] destroy callback for foreign session '%s' ignored"), *SessionName.ToString());
		return;
	}

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && !bSelfTestActive)
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteHandle);
	}
	ActiveDestroyOpId = 0;

	if (!bWasSuccessful)
	{
		// P1-6：销毁失败必须收敛全部关联状态，尤其是待执行动作，
		// 保证旧动作不会在下一次销毁或重试中被延迟执行。
		const EPostDestroyAction DroppedAction = PendingPostDestroyAction;
		PendingPostDestroyAction = EPostDestroyAction::None;
		ActiveCreateOpId = 0;
		ActiveFindOpId = 0;
		ActiveJoinOpId = 0;
		ActiveEndOpId = 0;
		bSearchCancelPending = false;
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] DESTROY_FAILED droppedPendingAction=%d (cleared; retry starts from a clean state)"),
			static_cast<int32>(DroppedAction));
		FailCurrentOperation(ENetSessionError::DestroyFailed, TEXT("OnDestroySessionComplete reported failure"));
		return;
	}

	const EPostDestroyAction PendingAction = PendingPostDestroyAction;
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] DESTROY_DONE pendingAction=%d"), static_cast<int32>(PendingAction));

	// 执行单末节：房主退出必须真正关闭 Listen Driver。不能用 ServerTravelToMap（无条件追加 ?listen）。
	// 改用 CleanupNetWorldToLocal：UEngine::HandleDisconnect 关 Listen Socket + world 上下文 ServerTravel
	// 到不带 ?listen 的本地大厅地图（UEngine 在 LoadMap 前检测到不再 Listen → 自动关闭 Listen Driver）。
	// 条件：房主（bIsHostFlag 必须在 FinishLeaving 之前捕获，因为 TryFinishLeaving 会清它）+
	// 当前 World 是 Listen 或 Server + 没有 ThenCreate/ThenFindJoin 待执行动作。
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const bool bIsHostLeaveExit = !bSelfTestActive
		&& (PendingAction == EPostDestroyAction::None)
		&& bIsHostFlag && World && (World->GetNetMode() == NM_ListenServer || World->GetNetMode() == NM_DedicatedServer);
	if (bIsHostLeaveExit)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] HOST_EXIT_CLEANUP listen driver closing -> local lobby via CleanupNetWorldToLocal"));
		// TrySetState(Leaving) → FinishLeaving → Traveling 是因为 ServerTravel 需要稳定非 Idle 状态
		if (TrySetState(ENetSessionState::Leaving))
		{
			FinishLeaving(); // 把旧的 bIsHostFlag / pending 清掉，让 Cleanup 从干净状态出发
			if (CleanupNetWorldToLocal(World))
			{
				UE_LOG(LogNetSession, Log, TEXT("[NetSession] HOST_EXIT_CLEANUP_INITIATED listen driver close + local lobby travel requested"));
			}
		}
	}
	else
	{
		FinishLeaving();
	}

	if (bSelfTestActive)
	{
		// P0-3：自检期间不重新发起真实创建/搜索
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS post-destroy action=%d"), static_cast<int32>(PendingAction));
		return;
	}

	switch (PendingAction)
	{
	case EPostDestroyAction::ThenCreate:
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] RESUME create after residual cleanup"));
		RequestCreateSession();
		break;
	case EPostDestroyAction::ThenFindJoin:
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] RESUME find+join after residual cleanup"));
		RequestFindSessions();
		break;
	default:
		break;
	}
}

// -------------------------------------------------------------- world hooks --

bool UNetSessionSubsystem::ServerTravelToMap(const FString& MapPath, const FString& ExtraOptions, bool bListen)
{
	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS ServerTravelToMap '%s'"), *MapPath);
		return false;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	if (!World)
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] TRAVEL_FAIL no world available"));
		return false;
	}
	if (!World->GetAuthGameMode())
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] TRAVEL_FAIL not on a server world; clients cannot ServerTravel"));
		return false;
	}

	// 核心 bug 修复：不再无条件追加 ?listen。创建房间时 bListen=true 走 Listen Server；
	// 房主退出回到 standalone 时 bListen=false，URL 不带 ?listen，UEngine 会自动关闭 Listen Driver。
	const FString ListenPart = bListen ? TEXT("?listen") : TEXT("");
	const FString TravelURL = FString::Printf(TEXT("%s%s%s"), *MapPath, *ListenPart, *ExtraOptions);
	// bAbsolute=true 让每次 ServerTravel 从干净 URL 开始，不继承旧世界残留的阶段选项（NetPhase=Match 等）。
	const bool bRequested = World->ServerTravel(TravelURL, /*bAbsolute=*/true);
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] TRAVEL_REQUEST url='%s' listen=%d accepted=%d"), *TravelURL, bListen ? 1 : 0, bRequested ? 1 : 0);
	if (bRequested)
	{
		TrySetState(ENetSessionState::Traveling);
	}
	return bRequested;
}

bool UNetSessionSubsystem::CleanupNetWorldToLocal(UWorld* World)
{
	if (!World)
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] CLEANUP_FAIL null world"));
		return false;
	}
	UWorld* CurrentWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!CurrentWorld || CurrentWorld != World)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLEANUP_SKIP world no longer authoritative; won't force-disconnect"));
		// World 已过期（可能已在 LoadMap 前被标记），保持幂等让后续 HandleWorldPostBeginPlay 收尾
		return false;
	}

	const ENetMode NetMode = World->GetNetMode();
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLEANUP_WORLD mode=%d map='%s' -> local lobby '%s'"),
		static_cast<int32>(NetMode), *World->GetMapName(), *LobbyMapPath);

	// world-context 断连路径（不依赖 PlayerController）：
	// 1. UEngine::HandleDisconnect 清理当前世界的 NetDriver / PendingNetGame / 远端连接
	//    - Listen Server：关闭 Listen Driver 端口
	//    - DedicatedServer：同上
	//    - Client：断开到房主的连接 + 清 PendingNetGame
	// 2. NetMode==NM_Client 额外：CancelPending 清掉仍在进行的连接请求
	// 3. 切到本地大厅地图不带 ?listen，UEngine 自动关闭 Listen Socket（若原来在 Listen）
	UEngine* Engine = GEngine;
	if (!Engine)
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] CLEANUP_FAIL no GEngine available; fallback to PlayerController ClientTravel only"));
		if (NetMode == NM_Client)
		{
			APlayerController* PC = GetGameInstance() ? GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
			if (PC) { PC->ClientTravel(LobbyMapPath, TRAVEL_Absolute); return true; }
		}
		return false;
	}

	if (NetMode == NM_Client)
	{
		// 客户端先 CancelPending 清掉仍在进行的连接请求，再 HandleDisconnect
		Engine->CancelPending(World);
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLEANUP_CLIENT_CANCEL_PENDING done"));
	}

	UNetDriver* NetDriver = World->GetNetDriver();
	if (NetDriver)
	{
		Engine->HandleDisconnect(World, NetDriver);
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLEANUP_HANDLE_DISCONNECT done (netdriver cleaned)"));
	}

	if (NetMode == NM_Client)
	{
		// 客户端没有 AuthGameMode，不能直接 World->ServerTravel。
		// GEngine::SetClientTravel(UWorld*, TCHAR*, ETravelType) —— world-context 纯 Engine 路径，不依赖 PlayerController。
		// 已先 CancelPending + HandleDisconnect，SetClientTravel 发起 LoadMap 到本地大厅。
		Engine->SetClientTravel(World, *LobbyMapPath, TRAVEL_Absolute);
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLEANUP_CLIENT_SETTRAVEL url='%s'"), *LobbyMapPath);
		return true;
	}
	else
	{
		// Host (Listen / Dedicated)：直接世界上下文 ServerTravel 到本地 standalone 大厅（不带 ?listen）
		// UEngine 在 LoadMap 前检测到请求不再 listen → 自动关闭 Listen Driver
		const bool bRequested = World->ServerTravel(LobbyMapPath, /*bAbsolute=*/true);
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CLEANUP_HOST_SERVERTRAVEL url='%s' accepted=%d listen_will_close=%d"), *LobbyMapPath, bRequested ? 1 : 0, 1);
		return bRequested;
	}
}

void UNetSessionSubsystem::EnforceGameSessionCapacity(UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] CAPACITY_SKIP no server world passed in; capacity left unchanged"));
		return;
	}

	AGameModeBase* GameMode = World->GetAuthGameMode();
	AGameSession* GameSession = GameMode ? GameMode->GameSession : nullptr;
	if (!GameSession)
	{
		// GameSession 尚未 spawn：保留定时器继续重试（0.25s 周期）。
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] CAPACITY GameSession not spawned yet world='%s'; retry timer stays active"), *World->GetMapName());
		return;
	}

	if (GameSession->MaxPlayers != SessionMaxPlayers)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CAPACITY_FORCED maxPlayers %d -> %d"), GameSession->MaxPlayers, SessionMaxPlayers);
		GameSession->MaxPlayers = SessionMaxPlayers;
	}
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CAPACITY maxPlayers=%d players=%d"),
		GameSession->MaxPlayers, GameMode->GetNumPlayers());

	World->GetTimerManager().ClearTimer(CapacityEnforceTimerHandle);
}

void UNetSessionSubsystem::HandlePostWorldInitialization(UWorld* World, const UWorld::InitializationValues)
{
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// 世界切换后旧世界的定时器失效：重置调度标记（真正调度延后到 BeginPlay 后触发）
	bAutomationStepScheduled = false;

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] MAP_INIT world='%s' netmode=%d url='%s' state=%s [early init; phase+exit deferred to post-BeginPlay tick]"),
		*World->GetMapName(), static_cast<int32>(World->GetNetMode()), *World->URL.ToString(), ToString(State));

	// Blind review P1-2：容量重试定时器不应依赖 GetAuthGameMode 存在。
	// OnPostWorldInitialization 发生在 GameMode 创建之前时整个 if block 跳过，后续
	// HandleWorldPostBeginPlay 也没补做容量设置。修复：不管 GameMode 是否已 spawn，
	// 只要不是客户端世界，都启动 0.25s 定时重试；EnforceGameSessionCapacity 内部
	// 再检查 GameSession 并决定是强制容量还是保留定时器。
	if (World->GetNetMode() != NM_Client)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CAPACITY starting retry timer (GameMode may not exist yet) world='%s'"), *World->GetMapName());
		World->GetTimerManager().SetTimer(
			CapacityEnforceTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this, World]()
			{
				EnforceGameSessionCapacity(World);
			}),
			0.25f,
			true);
	}

	// 打包实测修复：阶段判定 / standalone 退出完成 / 自动化调度——延后到 LoadMap 完成 + BeginPlay 后。
	// 此时 World URL 已是最终值（不再是 InitWorld 时的旧值）、PendingNetGame 已由 ?closed 清理、
	// ListenDriver / GameSession 已 spawn。SetTimerForNextTick 在下一次 World tick 触发，
	// 时序为：InitWorld → OnPostWorldInit → BeginPlay → Tick → 我们的回调。
	World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateWeakLambda(this, [this, World]()
		{
			if (!IsValid(World) || !World->IsGameWorld())
			{
				return;
			}
			HandleWorldPostBeginPlay(World);
		}));
}

void UNetSessionSubsystem::HandleWorldPostBeginPlay(UWorld* World)
{
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] MAP_READY world='%s' netmode=%d url='%s' state=%s"),
		*World->GetMapName(), static_cast<int32>(World->GetNetMode()), *World->URL.ToString(), ToString(State));

	// 实测修复（0703b6b 包内）：任何 Leaving 角色（客机断开或房主关闭监听）都必须在新本地世界
	// BeginPlay 后重新判定 Idle。之前只在 bClientLeavingWorldPending 时重检，导致房主退房后
	// 永远停在 Leaving、无法再次开房（Host.log STEP6/7 CreateRoom state=Leaving 被拒）。
	if (World->GetNetMode() == NM_Standalone && State == ENetSessionState::Leaving)
	{
		bClientWorldReturnedToLocal = true;
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] LOCAL_WORLD_READY standalone world beginplay (pendingClient=%d)"),
			bClientLeavingWorldPending ? 1 : 0);
		TryFinishLeaving();
	}

	switch (State)
	{
	case ENetSessionState::InLobby:
	case ENetSessionState::InMatch:
	case ENetSessionState::Traveling:
		break;
	default:
		TickAutomationScheduling();
		return;
	}

	// 阶段判定延后到 post-BeginPlay tick（打包实测 Host.log 1237→1250：InitWorld 时 URL 仍为旧值，
	// ServerTravel ?NetPhase=Match 到达后 Init 读出空 URL 回退 InLobby）。
	// 此时 URL 已是最终值，PendingNetGame/ListenDriver/GameSession 全部就绪。
	const FString Phase = World->URL.GetOption(NetPhaseOption, TEXT(""));
	const bool bMatchPhase = Phase.Equals(NetPhaseMatchValue, ESearchCase::IgnoreCase);
	if (State == ENetSessionState::Traveling)
	{
		TrySetState(bMatchPhase ? ENetSessionState::InMatch : ENetSessionState::InLobby);
	}
	else if (bMatchPhase && State != ENetSessionState::InMatch)
	{
		// 跟随房主进入对局
		TrySetState(ENetSessionState::InMatch);
	}
	else if (!bMatchPhase && State != ENetSessionState::InLobby)
	{
		// 跟随房主回到大厅
		TrySetState(ENetSessionState::InLobby);
	}

	// Blind review P1-2 补做：post-BeginPlay 时 GameMode/GameSession 一定已 spawn，
	// 立即强制容量一次保证在接受客户端登录前生效；Init 阶段的定时器作为兜底。
	if (World->GetNetMode() != NM_Client)
	{
		EnforceGameSessionCapacity(World);
	}

	TickAutomationScheduling();
}

void UNetSessionSubsystem::HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	UE_LOG(LogNetSession, Warning, TEXT("[NetSession] NETWORK_FAILURE type=%d error='%s' state=%s"),
		static_cast<int32>(FailureType), *ErrorString, ToString(State));

	if (State == ENetSessionState::Traveling || State == ENetSessionState::Joining)
	{
		// 无效连接/超时：给出明确的失败与重试入口
		FailCurrentOperation(ENetSessionError::TravelFailed,
			FString::Printf(TEXT("connection failed (%s); retry NetFindJoinRoom or connect manually via NetSessionConnect"), *ErrorString));
	}

	// 执行单末节：稳定房内（InLobby/InMatch）的连接丢失分两种角色处理。
	// 房主权威：单个远端连接超时/断开不得解散房间（S3 实测：客户端进程被杀后房主仅记录
	// UNetConnection::Close 并继续 InMatch，named session 保留）。这里若 bIsHostFlag 为真，
	// 只记录不断房；不能在 NetDriver 已销毁导致 GetNetMode 回退 Standalone 时误判为客机。
	// 客机：Fail + 断开 + 销毁本地会话，回到可重试入口。
	if (State == ENetSessionState::InLobby || State == ENetSessionState::InMatch)
	{
		if (bIsHostFlag)
		{
			UE_LOG(LogNetSession, Log, TEXT("[NetSession] NETWORK_FAILURE host-side: keep room, ignore single-connection loss (type=%d)"),
				static_cast<int32>(FailureType));
		}
		else
		{
			const FString Detail = FString::Printf(TEXT("lost connection to host (%s); cleaning up local session for retry"), *ErrorString);
			FailCurrentOperation(ENetSessionError::Disconnected, Detail);
			UE_LOG(LogNetSession, Log, TEXT("[NetSession] NETWORK_FAILURE client: scheduling next-tick disconnect + destroying local session"));
			// Defer disconnect out of the synchronous UNetDriver/Engine failure callback to avoid WorldContext re-entrancy asserts.
			TWeakObjectPtr<UNetSessionSubsystem> WeakSelf(this);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakSelf](float)
			{
				if (UNetSessionSubsystem* Self = WeakSelf.Get())
				{
					const bool bDisconnectRequested = Self->DisconnectClientWorldToLocal();
					if (bDisconnectRequested)
					{
						Self->bClientLeavingWorldPending = true;
						Self->bClientWorldReturnedToLocal = false;
					}
					if (Self->TrySetState(ENetSessionState::Leaving))
					{
						Self->BeginDestroySession();
					}
				}
				return false;
			}), 0.0f);
		}
	}

	if (bAutomationWaitingNetworkFailure)
	{
		bAutomationWaitingNetworkFailure = false;
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] network failure observed; resuming script"));
		TickAutomationScheduling();
	}
}

// ------------------------------------------------------------------- debug --

FString UNetSessionSubsystem::GetLastErrorMessage() const
{
	return FString::Printf(TEXT("%s: %s"), ToString(LastError), *LastErrorDetail);
}

FString UNetSessionSubsystem::GetStateDebugString() const
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	FString Line = FString::Printf(TEXT("state=%s error=%s detail='%s' host=%d netmode=%d pending=%d ops(c/f/j/e/d)=%u/%u/%u/%u/%u selftest=%d"),
		ToString(State), ToString(LastError), *LastErrorDetail,
		bIsHostFlag ? 1 : 0,
		World ? static_cast<int32>(World->GetNetMode()) : -1,
		static_cast<int32>(PendingPostDestroyAction),
		ActiveCreateOpId, ActiveFindOpId, ActiveJoinOpId, ActiveEndOpId, ActiveDestroyOpId,
		bSelfTestActive ? 1 : 0);

	IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid())
	{
		const FNamedOnlineSession* NamedSession = Sessions->GetNamedSession(NAME_GameSession);
		if (NamedSession)
		{
			Line += FString::Printf(TEXT(" session='%s' sessionState=%d openPublic=%d/%d hosting=%d"),
				*NamedSession->SessionName.ToString(),
				static_cast<int32>(NamedSession->SessionState),
				NamedSession->NumOpenPublicConnections,
				NamedSession->SessionSettings.NumPublicConnections,
				NamedSession->bHosting ? 1 : 0);
		}
		else
		{
			Line += TEXT(" session=<none>");
		}
	}
	return Line;
}

void UNetSessionSubsystem::DebugDumpState(const FString& Context)
{
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] DUMP (%s) %s"), *Context, *GetStateDebugString());
}

bool UNetSessionSubsystem::DebugConnectToAddress(const FString& Address)
{
	if (Address.IsEmpty())
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] CONNECT_REJECTED usage: NetSessionConnect <ip:port>"));
		return false;
	}

	// P1-5 状态门禁：Creating/Finding/Joining/Leaving/Traveling/InLobby/InMatch 一律拒绝，
	// 拒绝路径不产生 ClientTravel、不改动世界、也不触碰有效会话。
	if (!IsManualConnectStateAllowed())
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetSession] CONNECT_REJECTED state=%s (manual connect only allowed in Idle/Error; no travel issued)"), ToString(State));
		return false;
	}
	if (bSelfTestActive)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] SELF_TEST_SUPPRESS DebugConnectToAddress '%s'"), *Address);
		return false;
	}

	// 地址门禁：必须是 host:port，端口在 1..65535 范围内
	FString HostPart;
	FString PortPart;
	if (!Address.Split(TEXT(":"), &HostPart, &PortPart) || HostPart.IsEmpty() || PortPart.IsEmpty())
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] CONNECT_REJECTED malformed address '%s' (expected <ip:port>; no travel issued)"), *Address);
		return false;
	}
	const int32 PortNumber = FCString::Atoi(*PortPart);
	if (!PortPart.IsNumeric() || PortNumber <= 0 || PortNumber > 65535)
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] CONNECT_REJECTED invalid port '%s' in '%s' (no travel issued)"), *PortPart, *Address);
		return false;
	}

	APlayerController* PlayerController = GetGameInstance() ? GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
	if (!PlayerController)
	{
		UE_LOG(LogNetSession, Error, TEXT("[NetSession] CONNECT_FAIL no local player controller"));
		return false;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CONNECT_REQUEST manual addr='%s' (an invalid address surfaces OnNetworkFailure -> Error with a retry entry)"), *Address);
	// UE5.8 ClientTravel 无返回值：连接结果通过 MAP_ARRIVED / OnNetworkFailure 观测
	PlayerController->ClientTravel(Address, TRAVEL_Absolute);
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CONNECT_SENT addr='%s'"), *Address);
	TrySetState(ENetSessionState::Traveling);
	return true;
}

void UNetSessionSubsystem::DumpCapacityInfo() const
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	AGameModeBase* GameMode = World ? World->GetAuthGameMode() : nullptr;
	AGameSession* GameSession = GameMode ? GameMode->GameSession : nullptr;
	if (!World || !GameMode || !GameSession)
	{
		UE_LOG(LogNetSession, Log, TEXT("[NetSession] CAPACITY_DUMP no auth server here (netmode=%d); capacity is enforced on the listen server"),
			World ? static_cast<int32>(World->GetNetMode()) : -1);
		return;
	}
	UE_LOG(LogNetSession, Log, TEXT("[NetSession] CAPACITY_DUMP maxPlayers=%d players=%d netmode=%d"),
		GameSession->MaxPlayers, GameMode->GetNumPlayers(), static_cast<int32>(World->GetNetMode()));
}

// --------------------------------------------------------------- automation --

void UNetSessionSubsystem::StartAutomation(FName Role)
{
	if (Role == FName(TEXT("Stop")))
	{
		StopAutomation();
		return;
	}

	AutomationSteps.Reset();
	AutomationStepIndex = 0;
	bAutomationStepScheduled = false;
	bAutomationWaitingNetworkFailure = false;
	bAutomationDoneLogged = false;

	if (Role == FName(TEXT("Host")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// 房主脚本：开房 -> 重复开房(必须拒绝) -> 真实 InLobby 自检(68 项) -> 旅行对局
		//          -> 退房 -> 再次开房 -> 重复开房(必须拒绝) -> 再退房 -> 容量证据
		AutomationSteps =
		{
			{ 5.0f,  FName(TEXT("CreateRoom")) },
			{ 0.8f,  FName(TEXT("CreateRoom")) },
			{ 15.0f, FName(TEXT("SelfTest")) },
			{ 4.0f,  FName(TEXT("TravelMatch")) },
			{ 12.0f, FName(TEXT("LeaveRoom")) },
			{ 6.0f,  FName(TEXT("CreateRoom")) },
			{ 0.8f,  FName(TEXT("CreateRoom")) },
			{ 8.0f,  FName(TEXT("LeaveRoom")) },
			{ 1.0f,  FName(TEXT("DumpState")) },
			{ 0.5f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("HostFail")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// P1-6/P1-7 失败注入脚本：开房 -> 注入销毁失败 -> 退房(失败清理) -> 查看 Pending/状态
		//                   -> Reset 恢复 -> 再次开房 -> 正常退房 -> 状态汇总
		AutomationSteps =
		{
			{ 5.0f,  FName(TEXT("CreateRoom")) },
			{ 8.0f,  FName(TEXT("InjectDestroyFail")) },
			{ 1.0f,  FName(TEXT("LeaveRoom")) },
			{ 3.0f,  FName(TEXT("DumpState")) },
			{ 2.0f,  FName(TEXT("ResetState")) },
			{ 5.0f,  FName(TEXT("CreateRoom")) },
			{ 8.0f,  FName(TEXT("LeaveRoom")) },
			{ 1.0f,  FName(TEXT("DumpState")) },
			{ 0.5f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("HostDiag")))
	{
		// 诊断模式：开房后长驻并周期性 Dump 状态与容量，供 netstat / 手工发包验证 beacon，
		// 以及容量测试期间观测 GetNumPlayers 与 GameSession MaxPlayers。
		bAutomationActive = true;
		AutomationRole = Role;
		AutomationSteps =
		{
			{ 5.0f,  FName(TEXT("CreateRoom")) },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("DumpCapacity")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("DumpCapacity")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("DumpCapacity")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("DumpCapacity")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("DumpCapacity")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("ClientDiag")))
	{
		// 诊断模式：多次搜索（发现层验证）
		bAutomationActive = true;
		AutomationRole = Role;
		AutomationSteps =
		{
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 10.0f, FName(TEXT("DumpState")) }
		};
	}
	else if (Role == FName(TEXT("Client")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// 客户端脚本：正式 LAN 发现加入 -> 重复加入(拒绝) -> InLobby 直连(必须拒绝，P1-5 门禁)
		//          -> named-session 退出(断开+销毁) -> Idle 直连无效地址(允许路径->明确失败)
		//          -> 再次开房(Error 恢复) -> 重复(拒绝) -> 退房 -> 汇总
		AutomationSteps =
		{
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 0.8f,  FName(TEXT("FindJoin")) },
			{ 0.5f,  FName(TEXT("ConnectBad")), TEXT("127.0.0.1:9") },
			{ 8.0f,  FName(TEXT("DumpState")) },
			{ 12.0f, FName(TEXT("LeaveRoom")) },
			{ 1.0f,  FName(TEXT("ConnectBad")), TEXT("127.0.0.1:9") },
			{ 8.0f,  FName(TEXT("DumpState")) },
			{ 4.0f,  FName(TEXT("CreateRoom")) },
			{ 0.8f,  FName(TEXT("CreateRoom")) },
			{ 8.0f,  FName(TEXT("LeaveRoom")) },
			{ 1.0f,  FName(TEXT("DumpState")) },
			{ 0.5f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("HostUat")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// 真机 UAT（仅测试编排，不改变产品行为）：
		// 开房 -> 等待客机正式入房 -> 再确认人数 -> 房内自检(隔离) -> 旅行对局
		// -> 双方 InMatch -> 长驻观测（客机退出/掉线/房主退出测试由外部或后续步骤驱动）
		AutomationSteps =
		{
			{ 5.0f,  FName(TEXT("CreateRoom")) },
			{ 1.0f,  FName(TEXT("WaitForPlayers")), TEXT("2") },
			{ 8.0f,  FName(TEXT("DumpState")) },
			{ 5.0f,  FName(TEXT("TravelMatch")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InMatch") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 20.0f, FName(TEXT("DumpState")) },
			{ 20.0f, FName(TEXT("DumpState")) },
			{ 20.0f, FName(TEXT("DumpState")) },
			{ 15.0f, FName(TEXT("DumpState")) },
			{ 15.0f, FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("ClientStay")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// 客机保持在线：加入 -> 等待双方 InMatch -> 长驻观测（房主退房/房主进程退出时验证客机恢复）
		AutomationSteps =
		{
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InMatch") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) }
		};
	}
	else if (Role == FName(TEXT("ClientLeaveReopen")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// 客机主动退出后立即重开：加入 -> 双方 InMatch -> 退房 -> 等到 Idle（1 秒轮询）
		// -> 立即开房 -> 确认 InLobby -> 长驻观测
		AutomationSteps =
		{
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InMatch") },
			{ 3.0f,  FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("LeaveRoom")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("Idle") },
			{ 0.2f,  FName(TEXT("CreateRoom")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InLobby") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("HostLeaveUat")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// UAT：房主在双方 InMatch 后主动退房，验证 Listen 关闭 + 回到可重开状态
		AutomationSteps =
		{
			{ 5.0f,  FName(TEXT("CreateRoom")) },
			{ 1.0f,  FName(TEXT("WaitForPlayers")), TEXT("2") },
			{ 8.0f,  FName(TEXT("DumpState")) },
			{ 8.0f,  FName(TEXT("TravelMatch")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InMatch") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 15.0f, FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("LeaveRoom")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("Idle") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("DumpCapacity")) },
			{ 10.0f, FName(TEXT("DumpState")) }
		};
	}
	else if (Role == FName(TEXT("ClientRecover")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// UAT：客机在局内等待房主退房，出现断线后回到 Idle/Error，再自行开房恢复
		AutomationSteps =
		{
			{ 10.0f, FName(TEXT("FindJoin")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InMatch") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 20.0f, FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("Idle") },
			{ 0.5f,  FName(TEXT("CreateRoom")) },
			{ 1.0f,  FName(TEXT("WaitState")), TEXT("InLobby") },
			{ 5.0f,  FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 1.0f,  FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("JoinHold")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// P0-1 容量测试：加入后保持在线，直到进程被外部结束
		AutomationSteps =
		{
			{ 6.0f,  FName(TEXT("FindJoin")) },
			{ 8.0f,  FName(TEXT("DumpState")) },
			{ 15.0f, FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) },
			{ 10.0f, FName(TEXT("DumpState")) }
		};
	}
	else if (Role == FName(TEXT("Probe")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// 容量探针（第 5 名参与者）：搜索加入满员会话（JoinSession 成功后连接层拒绝），
		// 第一次 FindJoin 若因多进程 beacon 单播竞态 results=0，Error 态自动重试一次；
		// 最后显式直连（绕过会话层）验证连接层 ApproveLogin 拒绝（"Server full."）。
		// 直连属容量测试场景，需同时传 -NetDirectFallback 与 -NetHostAddr=<hostip>:7777。
		AutomationSteps =
		{
			{ 8.0f, FName(TEXT("FindJoin")) },
			{ 3.0f, FName(TEXT("FindJoin")) },
			{ 8.0f, FName(TEXT("DumpState")) },
			{ 1.0f, FName(TEXT("ConnectHost")) },
			{ 8.0f, FName(TEXT("DumpState")) },
			{ 0.5f, FName(TEXT("DumpCapacity")) }
		};
	}
	else if (Role == FName(TEXT("ResetDuringFind")))
	{
		bAutomationActive = true;
		AutomationRole = Role;
		// P1-8：搜索中 reset -> 立即重搜；无搜索时再次 reset（引擎返回取消失败路径）；
		// 最后确认不卡 Finding、旧回调不误报、状态回到 Idle。
		AutomationSteps =
		{
			{ 6.0f, FName(TEXT("FindJoin")) },
			{ 0.3f, FName(TEXT("ResetState")) },
			{ 1.0f, FName(TEXT("FindJoin")) },
			{ 1.0f, FName(TEXT("ResetState")) },
			{ 5.0f, FName(TEXT("DumpState")) }
		};
	}
	else
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetAuto] unknown role '%s' (use Host|Client|Stop)"), *Role.ToString());
		return;
	}

	UE_LOG(LogNetSession, Log, TEXT("[NetAuto] START role='%s' steps=%d"), *Role.ToString(), AutomationSteps.Num());
	// JoinHold/Probe 是要被其他进程连接的常驻角色，不自动退出（由测试脚本按 PID 结束）。
	bAutomationKeepAliveAfterDone = (Role == FName(TEXT("JoinHold")) || Role == FName(TEXT("Probe"))
		|| Role == FName(TEXT("HostUat")) || Role == FName(TEXT("ClientStay")) || Role == FName(TEXT("ClientLeaveReopen"))
		|| Role == FName(TEXT("HostLeaveUat")) || Role == FName(TEXT("ClientRecover")));
	// 每次启动脚本都重置轮询计数，避免上一角色残留
	AutomationWaitTicks = 0;
	// 无人值守脚本前先跑纯逻辑自检（状态转换表/重复请求拒绝/过期回调防护），证据随进程日志留存
	RunSelfTest();
	TickAutomationScheduling();

	// 无人值守兜底：挂在 core ticker 上（不随 World 销毁），因此 ServerTravel 后依然有效。
	// 同时覆盖两种情况：脚本未完成时按总超时退出；SCRIPT_DONE 之后也保留硬退出兜底。
	AutomationDeadline = FPlatformTime::Seconds() + AutomationTimeoutSeconds;
	AutomationDoneAt = 0.0;
	AutomationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(this, [this](float DeltaTime) -> bool
		{
			if (!bAutomationActive || bAutomationKeepAliveAfterDone)
			{
				// JoinHold/Probe 常驻，由测试脚本按记录的 PID 结束。
				AutomationTickerHandle.Reset();
				return false;
			}

			const double Now = FPlatformTime::Seconds();
			if (bAutomationDoneLogged || AutomationDoneAt > 0.0)
			{
				const double DoneAt = AutomationDoneAt > 0.0 ? AutomationDoneAt : Now;
				if (Now - DoneAt >= AutomationAutoExitDelaySeconds)
				{
					UE_LOG(LogNetSession, Log, TEXT("[NetAuto] AUTO_EXIT after SCRIPT_DONE (hard fallback)"));
					FPlatformMisc::RequestExit(false);
					return false;
				}
				return true;
			}

			if (Now >= AutomationDeadline)
			{
				UE_LOG(LogNetSession, Warning, TEXT("[NetAuto] TIMEOUT after %.0fs role='%s'; requesting exit"),
					AutomationTimeoutSeconds, *AutomationRole.ToString());
				FPlatformMisc::RequestExit(false);
				return false;
			}
			return true;
		}));
}

void UNetSessionSubsystem::StopAutomation()
{
	if (!bAutomationActive)
	{
		return;
	}
	bAutomationActive = false;
	bAutomationStepScheduled = false;
	bAutomationWaitingNetworkFailure = false;
	AutomationStepIndex = 0;
	AutomationSteps.Reset();
	FTSTicker::GetCoreTicker().RemoveTicker(AutomationTickerHandle);
	AutomationTickerHandle.Reset();
	AutomationDoneAt = 0.0;
	AutomationDeadline = 0.0;

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (World)
	{
		World->GetTimerManager().ClearTimer(AutomationTimerHandle);
		World->GetTimerManager().ClearTimer(AutomationWatchdogHandle);
	}
	UE_LOG(LogNetSession, Log, TEXT("[NetAuto] STOP"));
}

void UNetSessionSubsystem::TickAutomationScheduling()
{
	if (!bAutomationActive)
	{
		return;
	}
	if (AutomationStepIndex >= AutomationSteps.Num())
	{
		if (!bAutomationDoneLogged)
		{
			bAutomationDoneLogged = true;
			AutomationDoneAt = FPlatformTime::Seconds();
			UE_LOG(LogNetSession, Log, TEXT("[NetAuto] SCRIPT_DONE role='%s'"), *AutomationRole.ToString());
			// 退出由 StartAutomation 注册的 core ticker 兜底完成（跨 ServerTravel 有效）；
			// JoinHold/Probe 需要持续在线供其他进程连接，由测试脚本按记录的 PID 结束。
		}
		return;
	}
	if (bAutomationStepScheduled || bAutomationWaitingNetworkFailure)
	{
		return;
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	const FAutoStep& NextStep = AutomationSteps[AutomationStepIndex];
	World->GetTimerManager().SetTimer(
		AutomationTimerHandle,
		FTimerDelegate::CreateUObject(this, &UNetSessionSubsystem::ExecuteCurrentAutomationStep),
		FMath::Max(NextStep.DelaySeconds, 0.f),
		false);
	bAutomationStepScheduled = true;
	UE_LOG(LogNetSession, Log, TEXT("[NetAuto] scheduled step %d/%d '%s' in %.1fs (world='%s')"),
		AutomationStepIndex + 1, AutomationSteps.Num(), *NextStep.Action.ToString(), NextStep.DelaySeconds, *World->GetMapName());
}

void UNetSessionSubsystem::ExecuteCurrentAutomationStep()
{
	bAutomationStepScheduled = false;
	if (!bAutomationActive || AutomationStepIndex >= AutomationSteps.Num())
	{
		return;
	}

	const FAutoStep Step = AutomationSteps[AutomationStepIndex];
	++AutomationStepIndex;
	UE_LOG(LogNetSession, Log, TEXT("[NetAuto] STEP %d/%d '%s' arg='%s' state=%s"),
		AutomationStepIndex, AutomationSteps.Num(), *Step.Action.ToString(), *Step.Arg, ToString(State));

	ExecuteAutomationAction(Step);
	TickAutomationScheduling();
}

void UNetSessionSubsystem::ExecuteAutomationAction(const FAutoStep& Step)
{
	if (Step.Action == FName(TEXT("CreateRoom")))
	{
		CreateRoom();
	}
	else if (Step.Action == FName(TEXT("FindJoin")))
	{
		FindAndJoinRoom();
	}
	else if (Step.Action == FName(TEXT("TravelMatch")))
	{
		TravelToMatch();
	}
	else if (Step.Action == FName(TEXT("LeaveRoom")))
	{
		LeaveRoom();
	}
	else if (Step.Action == FName(TEXT("DumpState")))
	{
		DebugDumpState(TEXT("NetAuto"));
	}
	else if (Step.Action == FName(TEXT("DumpCapacity")))
	{
		DumpCapacityInfo();
	}
	else if (Step.Action == FName(TEXT("SelfTest")))
	{
		// P0-3 验收：必须在真实房主 InLobby（客户端在线）状态下执行，不影响世界/连接/会话
		UWorld* SelfTestWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] SelfTest in real state=%s (world map='%s')"),
			ToString(State), SelfTestWorld ? *SelfTestWorld->GetMapName() : TEXT("none"));
		RunSelfTest();
		DebugDumpState(TEXT("PostSelfTest"));
	}
	else if (Step.Action == FName(TEXT("InjectDestroyFail")))
	{
		// P1-6：受控测试钩子，仅自动化链路使用，默认不启用
		SetInjectDestroyFailure(true);
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] InjectDestroyFail armed for next DestroySession"));
	}
	else if (Step.Action == FName(TEXT("ResetState")))
	{
		ResetSessionState();
	}
	else if (Step.Action == FName(TEXT("ConnectHost")))
	{
		// 直连回退默认关闭：正式链路测试禁止用直连冒充 JoinSession 成功
		if (!FParse::Param(FCommandLine::Get(), TEXT("NetDirectFallback")))
		{
			UE_LOG(LogNetSession, Warning, TEXT("[NetAuto] ConnectHost blocked: official LAN chain must not silently fall back to direct connect (pass -NetDirectFallback to enable explicitly)"));
			return;
		}
		FString HostAddr;
		FParse::Value(FCommandLine::Get(), TEXT("NetHostAddr="), HostAddr);
		if (HostAddr.IsEmpty())
		{
			HostAddr = TEXT("127.0.0.1:7777");
			UE_LOG(LogNetSession, Log, TEXT("[NetAuto] ConnectHost: no -NetHostAddr=<ip:port> given; using default %s"), *HostAddr);
		}
		else
		{
			UE_LOG(LogNetSession, Log, TEXT("[NetAuto] ConnectHost: explicit direct connect to %s"), *HostAddr);
		}
		const bool bStarted = DebugConnectToAddress(HostAddr);
		if (!bStarted)
		{
			UE_LOG(LogNetSession, Log, TEXT("[NetAuto] ConnectHost rejected before travel (state=%s); no network failure expected"), ToString(State));
		}
	}
	else if (Step.Action == FName(TEXT("ConnectBad")))
	{
		const bool bStarted = DebugConnectToAddress(Step.Arg);
		if (!bStarted)
		{
			// P1-5：状态/地址门禁拒绝了旅行请求，不应等待网络失败
			UE_LOG(LogNetSession, Log, TEXT("[NetAuto] ConnectBad rejected before travel (state=%s); no network failure expected"), ToString(State));
			return;
		}
		bAutomationWaitingNetworkFailure = true;
		// 看门狗：即使网络失败回调未触发，也要继续脚本
		UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
		if (World)
		{
			World->GetTimerManager().SetTimer(
				AutomationWatchdogHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					if (bAutomationWaitingNetworkFailure)
					{
						bAutomationWaitingNetworkFailure = false;
						UE_LOG(LogNetSession, Warning, TEXT("[NetAuto] watchdog fired: no OnNetworkFailure within 30s; resuming script"));
						TickAutomationScheduling();
					}
				}),
				30.0f,
				false);
		}
	}
	else if (Step.Action == FName(TEXT("WaitForPlayers")))
	{
		// UAT 编排：等到房主权威世界里的在线人数达到指定值（默认 2），
		// 每 1 秒轮询一次，最多 90 秒；未达成也继续（日志保留真实结果，避免脚本卡死）。
		const int32 Want = Step.Arg.IsEmpty() ? 2 : FCString::Atoi(*Step.Arg);
		int32 Num = 0;
		if (UWorld* WaitWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
		{
			if (AGameModeBase* GameMode = WaitWorld->GetAuthGameMode())
			{
				Num = GameMode->GetNumPlayers();
			}
		}
		++AutomationWaitTicks;
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] WAIT_PLAYERS want=%d current=%d tick=%d state=%s"),
			Want, Num, AutomationWaitTicks, ToString(State));
		if (Num < Want && AutomationWaitTicks <= 90)
		{
			--AutomationStepIndex; // 1 秒后重新执行本步骤
			return;
		}
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] WAIT_PLAYERS_RESULT want=%d current=%d ok=%d"),
			Want, Num, Num >= Want ? 1 : 0);
		AutomationWaitTicks = 0;
	}
	else if (Step.Action == FName(TEXT("WaitState")))
	{
		// UAT 编排：等待状态机到达指定状态（大小写不敏感），每 1 秒轮询，最多 90 秒。
		++AutomationWaitTicks;
		const bool bMatched = FCString::Stricmp(ToString(State), *Step.Arg) == 0;
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] WAIT_STATE want='%s' current=%s tick=%d"),
			*Step.Arg, ToString(State), AutomationWaitTicks);
		if (!bMatched && AutomationWaitTicks <= 90)
		{
			--AutomationStepIndex;
			return;
		}
		UE_LOG(LogNetSession, Log, TEXT("[NetAuto] WAIT_STATE_RESULT want='%s' current=%s ok=%d"),
			*Step.Arg, ToString(State), bMatched ? 1 : 0);
		AutomationWaitTicks = 0;
	}
	else
	{
		UE_LOG(LogNetSession, Warning, TEXT("[NetAuto] unknown action '%s'"), *Step.Action.ToString());
	}
}

// ---------------------------------------------------------------- self test --

bool UNetSessionSubsystem::RunSelfTest()
{
	int32 Passed = 0;
	int32 Failed = 0;

	// P0-3：在首个可能产生副作用的操作之前启用隔离，并完整保存被触碰的共享状态。
	UWorld* SelfTestWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const ENetSessionState SavedState = State;
	const ENetSessionError SavedError = LastError;
	const FString SavedDetail = LastErrorDetail;
	const bool SavedIsHostFlag = bIsHostFlag;
	const uint32 SavedOperationIdCounter = OperationIdCounter;
	const uint32 SavedCreateOpId = ActiveCreateOpId;
	const uint32 SavedFindOpId = ActiveFindOpId;
	const uint32 SavedJoinOpId = ActiveJoinOpId;
	const uint32 SavedEndOpId = ActiveEndOpId;
	const uint32 SavedDestroyOpId = ActiveDestroyOpId;
	const EPostDestroyAction SavedPending = PendingPostDestroyAction;
	const bool SavedSearchCancelPending = bSearchCancelPending;
	const bool SavedClientLeavingWorldPending = bClientLeavingWorldPending;
	const bool SavedClientWorldReturnedToLocal = bClientWorldReturnedToLocal;
	const bool SavedAutomationStepScheduled = bAutomationStepScheduled;
	const bool SavedSuppressBroadcast = bSuppressStateBroadcast;
	const bool SavedSelfTestActive = bSelfTestActive;
	const FString SavedWorldName = SelfTestWorld ? SelfTestWorld->GetMapName() : FString(TEXT("none"));
	const int32 SavedNetMode = SelfTestWorld ? static_cast<int32>(SelfTestWorld->GetNetMode()) : -1;
	const int32 SavedPlayerCount = [&]()
	{
		if (!SelfTestWorld) { return -1; }
		AGameModeBase* GameMode = SelfTestWorld->GetAuthGameMode();
		return GameMode ? GameMode->GetNumPlayers() : -1;
	}();

	bSelfTestActive = true;
	bSuppressStateBroadcast = true;

	auto Check = [&Passed, &Failed](bool bCondition, const FString& Name)
	{
		if (bCondition)
		{
			++Passed;
			UE_LOG(LogNetSession, Log, TEXT("[NetSessionSelfTest] PASS: %s"), *Name);
		}
		else
		{
			++Failed;
			UE_LOG(LogNetSession, Error, TEXT("[NetSessionSelfTest] FAIL: %s"), *Name);
		}
	};

	// 1) 状态转换表：合法迁移必须放行
	struct FPair { ENetSessionState From; ENetSessionState To; };
	const FPair AllowedPairs[] =
	{
		{ ENetSessionState::Idle,      ENetSessionState::Creating },
		{ ENetSessionState::Idle,      ENetSessionState::Finding },
		{ ENetSessionState::Idle,      ENetSessionState::Leaving },
		{ ENetSessionState::Idle,      ENetSessionState::Traveling },
		{ ENetSessionState::Creating,  ENetSessionState::InLobby },
		{ ENetSessionState::Creating,  ENetSessionState::Error },
		{ ENetSessionState::Creating,  ENetSessionState::Leaving },
		{ ENetSessionState::Finding,   ENetSessionState::Joining },
		{ ENetSessionState::Finding,   ENetSessionState::Error },
		{ ENetSessionState::Finding,   ENetSessionState::Leaving },
		{ ENetSessionState::Joining,   ENetSessionState::Traveling },
		{ ENetSessionState::Joining,   ENetSessionState::Error },
		{ ENetSessionState::Joining,   ENetSessionState::Leaving },
		{ ENetSessionState::InLobby,   ENetSessionState::Traveling },
		{ ENetSessionState::InLobby,   ENetSessionState::InMatch },
		{ ENetSessionState::InLobby,   ENetSessionState::Leaving },
		{ ENetSessionState::Traveling, ENetSessionState::InLobby },
		{ ENetSessionState::Traveling, ENetSessionState::InMatch },
		{ ENetSessionState::Traveling, ENetSessionState::Leaving },
		{ ENetSessionState::Traveling, ENetSessionState::Error },
		{ ENetSessionState::InMatch,   ENetSessionState::InLobby },
		{ ENetSessionState::InMatch,   ENetSessionState::Traveling },
		{ ENetSessionState::InMatch,   ENetSessionState::Leaving },
		{ ENetSessionState::Leaving,   ENetSessionState::Idle },
		{ ENetSessionState::Leaving,   ENetSessionState::Error },
		{ ENetSessionState::Error,     ENetSessionState::Creating },
		{ ENetSessionState::Error,     ENetSessionState::Finding },
		{ ENetSessionState::Error,     ENetSessionState::Leaving },
		{ ENetSessionState::Error,     ENetSessionState::Idle },
		{ ENetSessionState::Error,     ENetSessionState::Traveling }
	};
	for (const FPair& Pair : AllowedPairs)
	{
		Check(IsTransitionAllowed(Pair.From, Pair.To),
			FString::Printf(TEXT("transition allowed %s -> %s"), ToString(Pair.From), ToString(Pair.To)));
	}

	// 2) 非法迁移抽样：必须全部拒绝
	const FPair DeniedPairs[] =
	{
		{ ENetSessionState::Idle,      ENetSessionState::InLobby },
		{ ENetSessionState::Idle,      ENetSessionState::InMatch },
		{ ENetSessionState::Idle,      ENetSessionState::Joining },
		{ ENetSessionState::Creating,  ENetSessionState::Finding },
		{ ENetSessionState::Creating,  ENetSessionState::Idle },
		{ ENetSessionState::Creating,  ENetSessionState::Joining },
		{ ENetSessionState::Finding,   ENetSessionState::Creating },
		{ ENetSessionState::Finding,   ENetSessionState::Idle },
		{ ENetSessionState::Joining,   ENetSessionState::Finding },
		{ ENetSessionState::Joining,   ENetSessionState::Idle },
		{ ENetSessionState::InLobby,   ENetSessionState::Idle },
		{ ENetSessionState::InLobby,   ENetSessionState::Creating },
		{ ENetSessionState::InLobby,   ENetSessionState::Finding },
		{ ENetSessionState::InLobby,   ENetSessionState::Error },
		{ ENetSessionState::InMatch,   ENetSessionState::Creating },
		{ ENetSessionState::InMatch,   ENetSessionState::Finding },
		{ ENetSessionState::InMatch,   ENetSessionState::Error },
		{ ENetSessionState::Traveling, ENetSessionState::Creating },
		{ ENetSessionState::Traveling, ENetSessionState::Finding },
		{ ENetSessionState::Traveling, ENetSessionState::Idle },
		{ ENetSessionState::Leaving,   ENetSessionState::Creating },
		{ ENetSessionState::Leaving,   ENetSessionState::Finding },
		{ ENetSessionState::Leaving,   ENetSessionState::Joining },
		{ ENetSessionState::Leaving,   ENetSessionState::InLobby },
		{ ENetSessionState::Error,     ENetSessionState::Joining },
		{ ENetSessionState::Error,     ENetSessionState::InLobby },
		{ ENetSessionState::Error,     ENetSessionState::InMatch }
	};
	for (const FPair& Pair : DeniedPairs)
	{
		Check(!IsTransitionAllowed(Pair.From, Pair.To),
			FString::Printf(TEXT("transition denied %s -> %s"), ToString(Pair.From), ToString(Pair.To)));
	}

	// 3) 重复请求拒绝：状态机层面（不触碰在线接口）
	State = ENetSessionState::Creating;
	Check(!CreateRoom(), TEXT("CreateRoom rejected while Creating"));
	Check(LastError == ENetSessionError::BusyState, TEXT("BusyState recorded for duplicate create"));
	// 真实房主在 InLobby 时 bIsHostFlag 为 true：这里显式置 false，使 TravelToMatch 走“非房主拒绝”
	// 的同一分支，既保持断言语义，也避免任何真实旅行。
	bIsHostFlag = false;
	State = ENetSessionState::InLobby;
	Check(!CreateRoom(), TEXT("CreateRoom rejected while InLobby (already in room)"));
	Check(!FindAndJoinRoom(), TEXT("FindAndJoinRoom rejected while InLobby"));
	Check(!TravelToMatch(), TEXT("TravelToMatch rejected for non-host"));
	// 3b) Finding 期间拒绝退房：必须临时关闭自检抑制，否则断言恒真（假阳性）。
	// 关闭抑制后走真实状态机拒绝分支；断言两者（返回值 + 错误码）以区分“抑制”与“真实拒绝”。
	bSelfTestActive = false;
	State = ENetSessionState::Finding;
	const bool bLeaveRejectedByState = !LeaveRoom();
	const ENetSessionError LeaveRejectError = LastError;
	bSelfTestActive = true;
	Check(bLeaveRejectedByState, TEXT("LeaveRoom rejected while Finding (outside self-test suppression)"));
	Check(LeaveRejectError == ENetSessionError::BusyState, TEXT("BusyState recorded for LeaveRoom while Finding"));

	// 3c) 自检抑制路径本身：抑制开启时同一状态必须直接返回（避免任何真实断连副作用）
	bSelfTestActive = true;
	State = ENetSessionState::InLobby;
	Check(!LeaveRoom(), TEXT("LeaveRoom suppressed during self-test in InLobby"));
	Check(State == ENetSessionState::InLobby, TEXT("suppressed LeaveRoom keeps state unchanged"));

	// 4) 身份未就绪必须拒绝
	Check(!IsIdentityLoginReady(nullptr), TEXT("identity gate rejects null identity"));

	// 5) 过期回调防护：旧代数回调必须被忽略
	State = ENetSessionState::Creating;
	ActiveCreateOpId = 999;
	HandleCreateSessionComplete(NAME_GameSession, true, 42);
	Check(State == ENetSessionState::Creating, TEXT("stale create callback ignored (state unchanged)"));

	// 6) 当前代数回调：自检模式进入 InLobby 且不触发 ServerTravel
	HandleCreateSessionComplete(NAME_GameSession, true, 999);
	Check(State == ENetSessionState::InLobby, TEXT("valid create callback reaches InLobby in self-test"));
	ActiveCreateOpId = 0;

	// 7) 会话名不匹配防护
	State = ENetSessionState::Leaving;
	ActiveDestroyOpId = 777;
	HandleDestroySessionComplete(FName(TEXT("OtherSession")), true, 777);
	Check(State == ENetSessionState::Leaving, TEXT("destroy callback with foreign session name ignored"));
	HandleDestroySessionComplete(NAME_GameSession, true, 777);
	Check(State == ENetSessionState::Idle, TEXT("valid destroy callback returns to Idle"));
	ActiveDestroyOpId = 0;

	// 恢复现场：正常结束路径（失败退出由下方统一恢复后返回 false）
	State = SavedState;
	LastError = SavedError;
	LastErrorDetail = SavedDetail;
	bIsHostFlag = SavedIsHostFlag;
	OperationIdCounter = SavedOperationIdCounter;
	ActiveCreateOpId = SavedCreateOpId;
	ActiveFindOpId = SavedFindOpId;
	ActiveJoinOpId = SavedJoinOpId;
	ActiveEndOpId = SavedEndOpId;
	ActiveDestroyOpId = SavedDestroyOpId;
	PendingPostDestroyAction = SavedPending;
	bSearchCancelPending = SavedSearchCancelPending;
	bClientLeavingWorldPending = SavedClientLeavingWorldPending;
	bClientWorldReturnedToLocal = SavedClientWorldReturnedToLocal;
	bAutomationStepScheduled = SavedAutomationStepScheduled;
	bSuppressStateBroadcast = SavedSuppressBroadcast;
	bSelfTestActive = SavedSelfTestActive;

	// 恢复校验（不计入 68 项断言）：地图/网络模式/玩家数/房主身份/会话名与进入时一致
	UWorld* AfterWorld = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	const FString AfterWorldName = AfterWorld ? AfterWorld->GetMapName() : FString(TEXT("none"));
	const int32 AfterNetMode = AfterWorld ? static_cast<int32>(AfterWorld->GetNetMode()) : -1;
	int32 AfterPlayers = -1;
	FString AfterSessionName = TEXT("<none>");
	if (AfterWorld && AfterWorld->GetAuthGameMode())
	{
		AfterPlayers = AfterWorld->GetAuthGameMode()->GetNumPlayers();
	}
	if (IOnlineSessionPtr Sessions = GetSessionInterface())
	{
		if (const FNamedOnlineSession* Named = Sessions->GetNamedSession(NAME_GameSession))
		{
			AfterSessionName = Named->SessionName.ToString();
		}
	}
	UE_LOG(LogNetSession, Log, TEXT("[NetSessionSelfTest] RESTORE_CHECK world '%s'->'%s' netmode %d->%d players %d->%d hostFlag=%d session='%s' state=%s pending=%d"),
		*SavedWorldName, *AfterWorldName, SavedNetMode, AfterNetMode, SavedPlayerCount, AfterPlayers,
		bIsHostFlag ? 1 : 0, *AfterSessionName, ToString(State), static_cast<int32>(PendingPostDestroyAction));

	UE_LOG(LogNetSession, Log, TEXT("[NetSessionSelfTest] RESULT: %d passed, %d failed -> %s"),
		Passed, Failed, Failed == 0 ? TEXT("PASS") : TEXT("FAIL"));
	return Failed == 0;
}
