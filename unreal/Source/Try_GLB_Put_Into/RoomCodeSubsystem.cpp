#include "RoomCodeSubsystem.h"
#include "CityMapSubsystem.h"
#include "CombatMatchActor.h"
#include "NetSessionSubsystem.h"
#include "PlayerCombatComponent.h"
#include "UnrealClient.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "IPAddress.h"
#include "Json.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "SocketSubsystem.h"
#include "Sound/SoundBase.h"
#include "Styling/CoreStyle.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	class FRoomCodeKeyProcessor final : public IInputProcessor
	{
	public:
		TFunction<bool(const FKeyEvent&)> Handler;
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
		{
			return Handler && Handler(Event);
		}
	};

	URoomCodeSubsystem* GetRoomSubsystem(UWorld* World)
	{
		if (!World || !World->GetGameInstance()) return nullptr;
		return World->GetGameInstance()->GetSubsystem<URoomCodeSubsystem>();
	}

	FAutoConsoleCommandWithWorld GCmdRoomHost(
		TEXT("RoomHost"),
		TEXT("Host a 6-digit LAN room and enter character select."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (URoomCodeSubsystem* Sub = GetRoomSubsystem(World)) Sub->HostRoom();
		}));

	FAutoConsoleCommandWithWorldAndArgs GCmdRoomJoin(
		TEXT("RoomJoin"),
		TEXT("Join a 6-digit LAN room (or IP). Usage: RoomJoin 123456"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (URoomCodeSubsystem* Sub = GetRoomSubsystem(World)) Sub->JoinRoom(Args.Num() > 0 ? Args[0] : TEXT(""));
		}));

	FAutoConsoleCommandWithWorld GCmdRoomStart(
		TEXT("RoomStart"),
		TEXT("Host starts the 8-minute Beacon Rush match once >= 2 players are in room."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (URoomCodeSubsystem* Sub = GetRoomSubsystem(World)) Sub->StartMatch();
		}));

	FAutoConsoleCommandWithWorld GCmdRoomLeave(
		TEXT("RoomLeave"),
		TEXT("Leave the current multiplayer room."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (URoomCodeSubsystem* Sub = GetRoomSubsystem(World)) Sub->LeaveCurrentRoom();
		}));
}

void URoomCodeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UNetSessionSubsystem>();

	// Strongly root SuoMao UFont assets on the GameInstanceSubsystem so LoadMap/ServerTravel GC never destroys them while Slate renders.
	DisplayFontAsset = LoadObject<UFont>(nullptr, TEXT("/Game/SuoMao/Fonts/SM_Display_Font.SM_Display_Font"));
	MediumFontAsset = LoadObject<UFont>(nullptr, TEXT("/Game/SuoMao/Fonts/SM_Medium_Font.SM_Medium_Font"));

	bAutoLAN = FParse::Param(FCommandLine::Get(), TEXT("AutoLAN"));
	FParse::Value(FCommandLine::Get(), TEXT("LocalServiceToken="), LocalServiceToken);
	FParse::Value(FCommandLine::Get(), TEXT("BackendUrl="), BackendUrl);
#if !UE_BUILD_SHIPPING
	FParse::Value(FCommandLine::Get(), TEXT("E2ERoomTest="), E2ERole);
	FParse::Value(FCommandLine::Get(), TEXT("E2EShotDir="), E2EShotDir);
	if (!E2ERole.IsEmpty() && E2EShotDir.IsEmpty())
	{
		E2EShotDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("E2EShots"));
	}
	if (!E2ERole.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*E2EShotDir, true);
		E2EStep = 0;
		E2EStepSince = 0.0;
	}
#endif

	if (UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr)
	{
		NetSub->OnNetSessionStateChanged.AddUniqueDynamic(this, &URoomCodeSubsystem::HandleNetSessionStateChanged);
	}

	bOverlayMounted = false;
	TWeakObjectPtr<URoomCodeSubsystem> WeakThis(this);
	if (FSlateApplication::IsInitialized())
	{
		auto Processor = MakeShared<FRoomCodeKeyProcessor>();
		Processor->Handler = [WeakThis](const FKeyEvent& Event)
		{
			URoomCodeSubsystem* Self = WeakThis.Get();
			return Self && IsValid(Self) && Self->HandleRoomKey(Event);
		};
		RoomKeyProcessor = Processor;
		FSlateApplication::Get().RegisterInputPreProcessor(RoomKeyProcessor, 0);
	}

	TickDelegateHandle = FWorldDelegates::OnWorldPostActorTick.AddLambda([WeakThis](UWorld* World, ELevelTick, float DeltaTime)
	{
		URoomCodeSubsystem* Self = WeakThis.Get();
		if (!Self || !IsValid(Self)) return;
		if (World && World->IsGameWorld() && World->HasBegunPlay() && World->GetGameInstance() == Self->GetGameInstance())
		{
			if (GEngine)
			{
				GEngine->bEnableOnScreenDebugMessages = false;
				GEngine->bEnableOnScreenDebugMessagesDisplay = false;
				GEngine->ClearOnScreenDebugMessages();
			}
			Self->EnsureOverlayMounted(World);
			Self->UpdateMouseAndWorldState(World);
			Self->SyncFrontendWidgets(World);
			if (Self->bPublishWhenReady && World->GetNetMode() == NM_ListenServer)
			{
				Self->PublishReadyRoom();
			}
#if !UE_BUILD_SHIPPING
			if (!Self->E2ERole.IsEmpty())
			{
				Self->TickE2ERoomTest(World, DeltaTime);
			}
#endif
		}
	});
}

void URoomCodeSubsystem::Deinitialize()
{
	if (UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr)
	{
		NetSub->OnNetSessionStateChanged.RemoveDynamic(this, &URoomCodeSubsystem::HandleNetSessionStateChanged);
	}
	if (RoomKeyProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(RoomKeyProcessor);
	}
	RoomKeyProcessor.Reset();
	if (LocalPreparation)
	{
		LocalPreparation->OnProcessRequestComplete().Unbind();
		LocalPreparation->CancelRequest();
		LocalPreparation.Reset();
	}
	if (TickDelegateHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(TickDelegateHandle);
		TickDelegateHandle.Reset();
	}
	if (OverlayWidget.IsValid())
	{
		if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
		{
			if (UGameViewportClient* Viewport = World->GetGameViewport())
			{
				Viewport->RemoveViewportWidgetContent(OverlayWidget.ToSharedRef());
			}
		}
	}
	OverlayWidget.Reset();
	CodeInputBox.Reset();
	bOverlayMounted = false;
	Super::Deinitialize();
}

void URoomCodeSubsystem::PlaySuoMaoSfx(const TCHAR* AssetPath) const
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (USoundBase* Sound = LoadObject<USoundBase>(nullptr, AssetPath))
	{
		UGameplayStatics::PlaySound2D(World, Sound, 0.85f);
	}
}

FSlateFontInfo URoomCodeSubsystem::GetSuoMaoFont(bool bDisplay, int32 Size) const
{
	UFont* FontObj = bDisplay ? DisplayFontAsset.Get() : MediumFontAsset.Get();
	if (IsValid(FontObj))
	{
		return FSlateFontInfo(FontObj, Size);
	}
	return FCoreStyle::GetDefaultFontStyle(bDisplay ? "Bold" : "Regular", Size);
}

FString URoomCodeSubsystem::DetectLocalAddress(UWorld* World) const
{
	FString MultiHomeIp;
	FParse::Value(FCommandLine::Get(), TEXT("LANHostAddress="), MultiHomeIp);
	if (MultiHomeIp.IsEmpty() && (!FParse::Value(FCommandLine::Get(), TEXT("MULTIHOME="), MultiHomeIp) || MultiHomeIp.IsEmpty()))
	{
		bool bCanBindAll = false;
		if (ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			TSharedRef<FInternetAddr> LocalAddr = SocketSub->GetLocalHostAddr(*GLog, bCanBindAll);
			if (LocalAddr->IsValid())
			{
				MultiHomeIp = LocalAddr->ToString(false);
			}
		}
	}
	if (MultiHomeIp.IsEmpty() || MultiHomeIp == TEXT("0.0.0.0"))
	{
		MultiHomeIp = TEXT("127.0.0.1");
	}
	int32 Port = 7777;
	if (World && World->URL.Port > 0) Port = World->URL.Port;
	return FString::Printf(TEXT("%s:%d"), *MultiHomeIp, Port);
}

FString URoomCodeSubsystem::GetLocalLanIpOnly() const
{
	const FString Addr = LocalConnectionAddress.IsEmpty() ? DetectLocalAddress(GetWorld()) : LocalConnectionAddress;
	FString Ip, Port;
	if (Addr.Split(TEXT(":"), &Ip, &Port)) return Ip;
	return Addr;
}

int32 URoomCodeSubsystem::GetConnectedPlayerCount(UWorld* World) const
{
	if (!World) return 1;
	if (const AGameStateBase* GS = World->GetGameState())
	{
		return FMath::Max(1, GS->PlayerArray.Num());
	}
	return 1;
}

bool URoomCodeSubsystem::IsInNetworkRoom(UWorld* World) const
{
	if (!World) return false;
	const ENetMode NetMode = World->GetNetMode();
	return NetMode == NM_ListenServer || NetMode == NM_Client;
}

void URoomCodeSubsystem::SwitchSuoMaoScreen(UUserWidget* RootWidget, const TCHAR* ScreenName) const
{
	if (!RootWidget) return;
	bool bInvoked = false;
	if (UFunction* SwitchFn = RootWidget->FindFunction(TEXT("SwitchTo")))
	{
		FStructOnScope Params(SwitchFn);
		for (TFieldIterator<FProperty> PropIt(SwitchFn); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if ((Prop->PropertyFlags & CPF_Parm) && !(Prop->PropertyFlags & CPF_ReturnParm))
			{
				if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				{
					NameProp->SetPropertyValue_InContainer(Params.GetStructMemory(), FName(ScreenName));
					RootWidget->ProcessEvent(SwitchFn, Params.GetStructMemory());
					bInvoked = true;
					break;
				}
				if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				{
					StrProp->SetPropertyValue_InContainer(Params.GetStructMemory(), FString(ScreenName));
					RootWidget->ProcessEvent(SwitchFn, Params.GetStructMemory());
					bInvoked = true;
					break;
				}
			}
		}
	}
	if (!bInvoked)
	{
		const FString TargetWidgetName = FString::Printf(TEXT("S_%s"), ScreenName);
		for (const TCHAR* Screen : {TEXT("S_Login"), TEXT("S_Home"), TEXT("S_Mode"), TEXT("S_CharSel"), TEXT("S_Load"), TEXT("S_Bag"), TEXT("S_Codex")})
		{
			if (UWidget* Child = RootWidget->GetWidgetFromName(Screen))
			{
				const bool bTarget = TargetWidgetName.Equals(Screen, ESearchCase::IgnoreCase);
				Child->SetVisibility(bTarget ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
				if (bTarget)
				{
					Child->SetRenderOpacity(1.0f);
					if (UUserWidget* ChildUW = Cast<UUserWidget>(Child))
					{
						if (UFunction* OnShowFn = ChildUW->FindFunction(TEXT("OnShow")))
						{
							if (OnShowFn->ParmsSize == 0) ChildUW->ProcessEvent(OnShowFn, nullptr);
						}
					}
				}
			}
		}
	}
}

void URoomCodeSubsystem::UpdateMouseAndWorldState(UWorld* World)
{
	if (!World) return;
	const FString MapName = World->GetMapName();
	const bool bIsFront = MapName.EndsWith(TEXT("Lvl_Front"));
	const bool bIsMatch = MapName.EndsWith(TEXT("Lvl_ZZZ"));
	APlayerController* PC = World->GetFirstPlayerController();

	if (LastInitializedWorld.Get() != World)
	{
		LastInitializedWorld = World;
		HookedModeWidget.Reset();
		HookedCharSelWidget.Reset();
		bWorldAutoNavigated = false;
		bMatchRoomExpanded = false;
		if (bIsMatch)
		{
			bRoomEntryModalOpen = false;
			if (PC)
			{
				PC->bShowMouseCursor = false;
				PC->SetInputMode(FInputModeGameOnly());
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().SetAllUserFocusToGameViewport();
				}
			}
		}
		else if (bIsFront)
		{
			LocalConnectionAddress = DetectLocalAddress(World);
			if (IsInNetworkRoom(World))
			{
				bRoomEntryModalOpen = false;
			}
		}
	}

	if (bIsFront && PC && !PC->bShowMouseCursor)
	{
		PC->bShowMouseCursor = true;
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		InputMode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(InputMode);
	}
}

void URoomCodeSubsystem::SyncFrontendWidgets(UWorld* World)
{
	if (!World || !World->GetMapName().EndsWith(TEXT("Lvl_Front"))) return;

	const bool bInRoom = IsInNetworkRoom(World);
	const bool bIsHost = (World->GetNetMode() == NM_ListenServer);
	const int32 PlayerCount = GetConnectedPlayerCount(World);

	UUserWidget* RootWidget = nullptr;
	UUserWidget* ModeWidget = nullptr;
	UUserWidget* CharSelWidget = nullptr;

	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* Widget = *It;
		if (!Widget || Widget->HasAnyFlags(RF_ClassDefaultObject) || Widget->GetWorld() != World) continue;
		const FString ClassName = Widget->GetClass()->GetName();
		if (ClassName.StartsWith(TEXT("WBP_SM_Root"))) RootWidget = Widget;
		else if (ClassName.StartsWith(TEXT("WBP_Mode"))) ModeWidget = Widget;
		else if (ClassName.StartsWith(TEXT("WBP_CharSel"))) CharSelWidget = Widget;
	}

	// Auto-navigate WBP_SM_Root once after ServerTravel/ClientTravel into Lvl_Front:
	// - Inside a network room -> straight to native WBP_CharSel (选人阶段)
	// - Returning from LeaveCurrentRoom -> straight back to WBP_Mode (01 信标争夺)
	if (RootWidget && !bWorldAutoNavigated)
	{
		if (bInRoom)
		{
			bWorldAutoNavigated = true;
			SwitchSuoMaoScreen(RootWidget, TEXT("CharSel"));
			UE_LOG(LogTemp, Display, TEXT("[RoomCode] Auto-switched Lvl_Front -> CharSel (room=%s, host=%d)"), *CurrentRoomCode, bIsHost ? 1 : 0);
		}
		else if (bReturnToModeOnFront)
		{
			bWorldAutoNavigated = true;
			bReturnToModeOnFront = false;
			SwitchSuoMaoScreen(RootWidget, TEXT("Mode"));
			UE_LOG(LogTemp, Display, TEXT("[RoomCode] Auto-switched Lvl_Front -> Mode after leaving room"));
		}
	}

	// 1. Sync WBP_Mode (01 信标争夺: 10分钟->8分钟 pill only, disable 02/03, rewire BtnGo -> Room Entry Modal)
	if (ModeWidget)
	{
		if (UWidget* Btn1 = ModeWidget->GetWidgetFromName(TEXT("BtnM_1")))
		{
			if (Btn1->GetIsEnabled()) { Btn1->SetIsEnabled(false); Btn1->SetRenderOpacity(0.42f); }
		}
		if (UWidget* Btn2 = ModeWidget->GetWidgetFromName(TEXT("BtnM_2")))
		{
			if (Btn2->GetIsEnabled()) { Btn2->SetIsEnabled(false); Btn2->SetRenderOpacity(0.42f); }
		}
		if (ModeWidget->WidgetTree)
		{
			ModeWidget->WidgetTree->ForEachWidget([](UWidget* W)
			{
				if (UTextBlock* TB = Cast<UTextBlock>(W))
				{
					if (TB->GetText().ToString().TrimStartAndEnd() == TEXT("10 分钟"))
					{
						TB->SetText(FText::FromString(TEXT("8 分钟")));
					}
				}
			});
		}
		if (UTextBlock* OnlineTx = Cast<UTextBlock>(ModeWidget->GetWidgetFromName(TEXT("GoOnline"))))
		{
			OnlineTx->SetText(FText::FromString(TEXT("局域网 2-4 人")));
		}
		if (HookedModeWidget.Get() != ModeWidget)
		{
			if (UButton* BtnGo = Cast<UButton>(ModeWidget->GetWidgetFromName(TEXT("BtnGo"))))
			{
				BtnGo->OnClicked.Clear();
				BtnGo->OnClicked.AddUniqueDynamic(this, &URoomCodeSubsystem::OnModeGoClicked);
				HookedModeWidget = ModeWidget;
				UE_LOG(LogTemp, Display, TEXT("[RoomCode] Rewired WBP_Mode::BtnGo to SuoMao Room Matchmaking Modal"));
			}
		}
	}

	// 2. Sync WBP_CharSel (Disable Koba BtnC_3, read Sel 0..2, update native room header & BtnLock)
	if (CharSelWidget && bInRoom)
	{
		if (UWidget* BtnKoba = CharSelWidget->GetWidgetFromName(TEXT("BtnC_3")))
		{
			if (BtnKoba->GetIsEnabled()) { BtnKoba->SetIsEnabled(false); BtnKoba->SetRenderOpacity(0.35f); }
		}
		if (FIntProperty* SelProp = FindFProperty<FIntProperty>(CharSelWidget->GetClass(), TEXT("Sel")))
		{
			const int32 Val = SelProp->GetPropertyValue_InContainer(CharSelWidget);
			if (Val >= 0 && Val <= 2)
			{
				SelectedCharacterIndex = Val;
			}
			else if (Val == 3)
			{
				SelProp->SetPropertyValue_InContainer(CharSelWidget, SelectedCharacterIndex);
			}
		}

		if (HookedCharSelWidget.Get() != CharSelWidget)
		{
			if (UButton* BtnLock = Cast<UButton>(CharSelWidget->GetWidgetFromName(TEXT("BtnLock"))))
			{
				BtnLock->OnClicked.Clear();
				BtnLock->OnClicked.AddUniqueDynamic(this, &URoomCodeSubsystem::OnCharSelLockClicked);
			}
			if (UButton* BtnBack = Cast<UButton>(CharSelWidget->GetWidgetFromName(TEXT("BtnBack"))))
			{
				BtnBack->OnClicked.Clear();
				BtnBack->OnClicked.AddUniqueDynamic(this, &URoomCodeSubsystem::OnCharSelBackClicked);
			}
			HookedCharSelWidget = CharSelWidget;
			UE_LOG(LogTemp, Display, TEXT("[RoomCode] Rewired WBP_CharSel::BtnLock & BtnBack for LAN room lobby"));
		}

		const FString DisplayCode = CurrentRoomCode.IsEmpty() ? TEXT("LAN") : CurrentRoomCode;
		if (UTextBlock* TmLabel = Cast<UTextBlock>(CharSelWidget->GetWidgetFromName(TEXT("TmLabel"))))
		{
			TmLabel->SetText(FText::FromString(TEXT("房间人数")));
		}
		if (UTextBlock* TmNum = Cast<UTextBlock>(CharSelWidget->GetWidgetFromName(TEXT("TmNum"))))
		{
			TmNum->SetText(FText::FromString(FString::Printf(TEXT("%d/4"), PlayerCount)));
		}

		if (UTextBlock* LockHint = Cast<UTextBlock>(CharSelWidget->GetWidgetFromName(TEXT("LockHint"))))
		{
			if (bIsHost)
			{
				LockHint->SetText(FText::FromString(PlayerCount >= 2 ? TEXT("人数已齐 · 可开局") : TEXT("需满 2 人开局")));
			}
			else
			{
				LockHint->SetText(FText::FromString(TEXT("等待房主开局")));
			}
		}

		UButton* BtnLock = Cast<UButton>(CharSelWidget->GetWidgetFromName(TEXT("BtnLock")));
		UTextBlock* LockCN = Cast<UTextBlock>(CharSelWidget->GetWidgetFromName(TEXT("LockCN")));
		UTextBlock* LockEN = Cast<UTextBlock>(CharSelWidget->GetWidgetFromName(TEXT("LockEN")));
		if (bIsHost)
		{
			const bool bCanStart = (PlayerCount >= 2);
			if (BtnLock)
			{
				BtnLock->SetIsEnabled(bCanStart);
				BtnLock->SetRenderOpacity(bCanStart ? 1.0f : 0.72f);
			}
			if (LockCN) LockCN->SetText(FText::FromString(bCanStart ? TEXT("开始对局") : FString::Printf(TEXT("等待队友 %d/2"), PlayerCount)));
			if (LockEN) LockEN->SetText(FText::FromString(bCanStart ? TEXT("— START 8M —") : TEXT("— NEED 2P —")));
		}
		else
		{
			if (BtnLock)
			{
				BtnLock->SetIsEnabled(true);
				BtnLock->SetRenderOpacity(1.0f);
			}
			if (LockCN) LockCN->SetText(FText::FromString(bClientReadyLocked ? TEXT("已锁定角色") : TEXT("锁定出战")));
			if (LockEN) LockEN->SetText(FText::FromString(bClientReadyLocked ? TEXT("— READY —") : TEXT("— LOCK IN —")));
		}
	}
}

void URoomCodeSubsystem::OnModeGoClicked()
{
	PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Confirm.SFX_Confirm"));
	bRoomEntryModalOpen = true;
	StatusMessage = TEXT("创建房间生成 6 位房号，或输入队友的 6 位房号局域网加入");
	if (CodeInputBox.IsValid()) CodeInputBox->SetText(FText::GetEmpty());
}

void URoomCodeSubsystem::CloseRoomEntryModal()
{
	PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Back.SFX_Back"));
	bRoomEntryModalOpen = false;
}

void URoomCodeSubsystem::OnCharSelLockClicked()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (World->GetNetMode() == NM_ListenServer)
	{
		if (GetConnectedPlayerCount(World) < 2)
		{
			PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Back.SFX_Back"));
			StatusMessage = TEXT("至少需要 2 名玩家进房才能开始对局！");
			return;
		}
		PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Confirm.SFX_Confirm"));
		StartMatch();
	}
	else
	{
		bClientReadyLocked = !bClientReadyLocked;
		PlaySuoMaoSfx(bClientReadyLocked ? TEXT("/Game/SuoMao/Audio/SFX_Lock.SFX_Lock") : TEXT("/Game/SuoMao/Audio/SFX_Click.SFX_Click"));
	}
}

void URoomCodeSubsystem::OnCharSelBackClicked()
{
	PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Back.SFX_Back"));
	LeaveCurrentRoom();
}

void URoomCodeSubsystem::CopyRoomCodeToClipboard()
{
	if (CurrentRoomCode.IsEmpty()) return;
	FPlatformApplicationMisc::ClipboardCopy(*CurrentRoomCode);
	CopiedFeedbackUntil = FPlatformTime::Seconds() + 2.0;
	PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Click.SFX_Click"));
}

void URoomCodeSubsystem::HandleNetSessionStateChanged(ENetSessionState NewState, ENetSessionError LastError)
{
	if (NewState == ENetSessionState::Traveling && !PendingJoinCode.IsEmpty())
	{
		CurrentRoomCode = PendingJoinCode;
		PendingJoinCode.Empty();
		bRoomEntryModalOpen = false;
		StatusMessage = FString::Printf(TEXT("已连接房间【%s】，正在进入选人阶段…"), *CurrentRoomCode);
	}
	else if (NewState == ENetSessionState::Error)
	{
		const FString Tried = PendingJoinCode;
		PendingJoinCode.Empty();
		if (LastError == ENetSessionError::NoSessionsFound)
		{
			StatusMessage = Tried.IsEmpty()
				? TEXT("局域网未搜索到房间，请确认房主已开房且在同一 Wi-Fi")
				: FString::Printf(TEXT("局域网未找到房号【%s】；请确认在同一局域网，或直接输入房主 IP 加入"), *Tried);
		}
		else
		{
			StatusMessage = TEXT("连接房主失败，请确认房号正确且防火墙未拦截，然后重试");
		}
	}
}

void URoomCodeSubsystem::ToggleMatchRoomMenu()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || !World->GetMapName().EndsWith(TEXT("Lvl_ZZZ"))) return;
	bMatchRoomExpanded = !bMatchRoomExpanded;
	PlaySuoMaoSfx(bMatchRoomExpanded ? TEXT("/Game/SuoMao/Audio/SFX_Click.SFX_Click") : TEXT("/Game/SuoMao/Audio/SFX_Back.SFX_Back"));
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (bMatchRoomExpanded)
		{
			bSavedMatchCursor = PC->bShowMouseCursor;
			PC->bShowMouseCursor = true;
			FInputModeGameAndUI InputMode;
			InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			InputMode.SetHideCursorDuringCapture(false);
			PC->SetInputMode(InputMode);
		}
		else
		{
			PC->bShowMouseCursor = bSavedMatchCursor;
			if (!bSavedMatchCursor)
			{
				PC->SetInputMode(FInputModeGameOnly());
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().SetAllUserFocusToGameViewport();
				}
			}
		}
	}
}

bool URoomCodeSubsystem::HandleRoomKey(const FKeyEvent& Event)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World || !World->GetGameViewport()) return false;
	const TSharedPtr<SViewport> Widget = World->GetGameViewport()->GetGameViewportWidget();
	if (!Widget || FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) != FSlateApplication::Get().GetActiveTopLevelWindow())
	{
		return false;
	}
	const FKey Key = Event.GetKey();
	if (Key == EKeys::Escape && !Event.IsRepeat())
	{
		if (bRoomEntryModalOpen)
		{
			CloseRoomEntryModal();
			return true;
		}
		if (bMatchRoomExpanded)
		{
			ToggleMatchRoomMenu();
			return true;
		}
	}
	if (Key == EKeys::F1 && !Event.IsRepeat() && World->GetMapName().EndsWith(TEXT("Lvl_ZZZ")))
	{
		ToggleMatchRoomMenu();
		return true;
	}
	return false;
}

void URoomCodeSubsystem::EnsureOverlayMounted(UWorld* World)
{
	if (!World) return;
	UGameViewportClient* Viewport = World->GetGameViewport();
	if (!Viewport || (bOverlayMounted && OverlayWidget.IsValid())) return;

	TWeakObjectPtr<URoomCodeSubsystem> WeakThis(this);
	const FLinearColor VeilColor(0.02f, 0.028f, 0.036f, 0.80f);
	const FLinearColor CardDark(0.055f, 0.068f, 0.080f, 0.96f);
	const FLinearColor SubCardDark(0.082f, 0.098f, 0.114f, 0.96f);
	const FLinearColor VoltYellow = FLinearColor::FromSRGBColor(FColor(250, 238, 0));
	const FLinearColor LimeAccent = FLinearColor::FromSRGBColor(FColor(214, 250, 106));
	const FLinearColor PaperWhite(0.95f, 0.97f, 0.96f, 1.0f);
	const FLinearColor MutedText(0.68f, 0.74f, 0.73f, 1.0f);
	const FLinearColor DarkInk(0.04f, 0.05f, 0.06f, 1.0f);
	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");

	SAssignNew(CodeInputBox, SEditableTextBox)
		.MinDesiredWidth(230.f)
		.HintText(FText::FromString(TEXT("输入 6 位房号（或房主 IP）")))
		.Font(GetSuoMaoFont(true, 16))
		.BackgroundColor(FLinearColor(0.03f, 0.04f, 0.05f, 1.0f))
		.ForegroundColor(PaperWhite)
		.Padding(FMargin(12.f, 10.f));

	auto MakeSolidButton = [WhiteBrush, this](FLinearColor BgColor, FLinearColor TextColor, const TCHAR* Label, int32 FontSize, bool bDisplayFont, FOnClicked OnClicked)
	{
		return SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.ContentPadding(FMargin(0.f))
			.Cursor(EMouseCursor::Hand)
			.OnClicked(OnClicked)
			[
				SNew(SBorder)
				.BorderImage(WhiteBrush)
				.BorderBackgroundColor(BgColor)
				.Padding(FMargin(18.f, 13.f))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.Font(GetSuoMaoFont(bDisplayFont, FontSize))
					.ColorAndOpacity(TextColor)
				]
			];
	};

	OverlayWidget = SNew(SOverlay)
		.Visibility(EVisibility::SelfHitTestInvisible)

		// LAYER 1: SuoMao-styled Room Entry Modal (ONLY visible after clicking WBP_Mode::BtnGo)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SNew(SBorder)
			.BorderImage(WhiteBrush)
			.BorderBackgroundColor(VeilColor)
			.Visibility_Lambda([WeakThis]()
			{
				URoomCodeSubsystem* Self = WeakThis.Get();
				UWorld* W = Self ? Self->GetWorld() : nullptr;
				return (Self && W && W->GetMapName().EndsWith(TEXT("Lvl_Front")) && Self->bRoomEntryModalOpen && !Self->IsInNetworkRoom(W))
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(WhiteBrush)
				.BorderBackgroundColor(VoltYellow)
				.Padding(FMargin(2.f))
				[
					SNew(SBorder)
					.BorderImage(WhiteBrush)
					.BorderBackgroundColor(CardDark)
					.Padding(FMargin(32.f, 26.f))
					[
						SNew(SBox)
						.WidthOverride(760.f)
						[
							SNew(SVerticalBox)
							// Modal Header
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.f, 0.f, 0.f, 10.f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.FillWidth(1.f)
								.VAlign(VAlign_Center)
								[
									SNew(SVerticalBox)
									+ SVerticalBox::Slot().AutoHeight()
									[
										SNew(STextBlock)
										.Text(FText::FromString(TEXT("OPERATION // 01 BEACON RUSH")))
										.Font(GetSuoMaoFont(true, 11))
										.ColorAndOpacity(VoltYellow)
									]
									+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
									[
										SNew(STextBlock)
										.Text(FText::FromString(TEXT("信标争夺 · 战术房间匹配")))
										.Font(GetSuoMaoFont(true, 26))
										.ColorAndOpacity(PaperWhite)
									]
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Top)
								[
									SNew(SBorder)
									.BorderImage(WhiteBrush)
									.BorderBackgroundColor(FLinearColor(0.28f, 0.34f, 0.38f, 0.9f))
									.Padding(FMargin(1.f))
									[
										MakeSolidButton(SubCardDark, PaperWhite, TEXT("← 返回 [Esc]"), 13, false, FOnClicked::CreateLambda([WeakThis]()
										{
											if (URoomCodeSubsystem* Self = WeakThis.Get()) Self->CloseRoomEntryModal();
											return FReply::Handled();
										}))
									]
								]
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.f, 0.f, 0.f, 20.f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("4 人乱斗  ·  8 分钟限时信标争夺  ·  至少 2 名旅人进房选角后由房主发起行动")))
								.Font(GetSuoMaoFont(false, 13))
								.ColorAndOpacity(MutedText)
							]
							// Two Side-by-Side SuoMao Cards: Host / Join
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.f, 0.f, 0.f, 18.f)
							[
								SNew(SHorizontalBox)
								// Left Card: Host
								+ SHorizontalBox::Slot()
								.FillWidth(1.f)
								.Padding(0.f, 0.f, 12.f, 0.f)
								[
									SNew(SBorder)
									.BorderImage(WhiteBrush)
									.BorderBackgroundColor(SubCardDark)
									.Padding(FMargin(22.f, 20.f))
									[
										SNew(SVerticalBox)
										+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
										[
											SNew(STextBlock)
											.Text(FText::FromString(TEXT("01  /  CREATE ROOM")))
											.Font(GetSuoMaoFont(true, 11))
											.ColorAndOpacity(LimeAccent)
										]
										+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
										[
											SNew(STextBlock)
											.Text(FText::FromString(TEXT("创建战术房间")))
											.Font(GetSuoMaoFont(true, 20))
											.ColorAndOpacity(PaperWhite)
										]
										+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 20.f)
										[
											SNew(STextBlock)
											.WrapTextAt(300.f)
											.Text(FText::FromString(TEXT("自动生成 6 位数字房号并进入选人阶段，队友输入房号即可同局域网接入。")))
											.Font(GetSuoMaoFont(false, 12))
											.ColorAndOpacity(MutedText)
										]
										+ SVerticalBox::Slot().AutoHeight()
										[
											MakeSolidButton(VoltYellow, DarkInk, TEXT("创建房间 · 生成 6 位房号"), 15, true, FOnClicked::CreateLambda([WeakThis]()
											{
												if (URoomCodeSubsystem* Self = WeakThis.Get())
												{
													Self->PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Confirm.SFX_Confirm"));
													Self->HostRoom();
												}
												return FReply::Handled();
											}))
										]
									]
								]
								// Right Card: Join
								+ SHorizontalBox::Slot()
								.FillWidth(1.f)
								.Padding(12.f, 0.f, 0.f, 0.f)
								[
									SNew(SBorder)
									.BorderImage(WhiteBrush)
									.BorderBackgroundColor(SubCardDark)
									.Padding(FMargin(22.f, 20.f))
									[
										SNew(SVerticalBox)
										+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
										[
											SNew(STextBlock)
											.Text(FText::FromString(TEXT("02  /  JOIN BY CODE")))
											.Font(GetSuoMaoFont(true, 11))
											.ColorAndOpacity(LimeAccent)
										]
										+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
										[
											SNew(STextBlock)
											.Text(FText::FromString(TEXT("输入 6 位房号加入")))
											.Font(GetSuoMaoFont(true, 20))
											.ColorAndOpacity(PaperWhite)
										]
										+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 14.f)
										[
											CodeInputBox.ToSharedRef()
										]
										+ SVerticalBox::Slot().AutoHeight()
										[
											MakeSolidButton(PaperWhite, DarkInk, TEXT("立即加入房间"), 15, true, FOnClicked::CreateLambda([WeakThis]()
											{
												if (URoomCodeSubsystem* Self = WeakThis.Get())
												{
													Self->PlaySuoMaoSfx(TEXT("/Game/SuoMao/Audio/SFX_Confirm.SFX_Confirm"));
													const FString InputCode = Self->CodeInputBox.IsValid()
														? Self->CodeInputBox->GetText().ToString().TrimStartAndEnd() : TEXT("");
													Self->JoinRoom(InputCode);
												}
												return FReply::Handled();
											}))
										]
									]
								]
							]
							// Status Footer Bar
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(SBorder)
								.BorderImage(WhiteBrush)
								.BorderBackgroundColor(FLinearColor(0.03f, 0.04f, 0.05f, 0.92f))
								.Padding(FMargin(14.f, 10.f))
								[
									SNew(STextBlock)
									.WrapTextAt(680.f)
									.Text_Lambda([WeakThis]()
									{
										return WeakThis.IsValid() ? FText::FromString(WeakThis->StatusMessage) : FText::GetEmpty();
									})
									.Font(GetSuoMaoFont(false, 13))
									.ColorAndOpacity(VoltYellow)
								]
							]
						]
					]
				]
			]
		]

		// LAYER 2: Sleek SuoMao Top-Right Copy Pill inside WBP_CharSel (ONLY visible when inside a room on Lvl_Front)
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, 24.f, 32.f, 0.f))
		[
			SNew(SBorder)
			.Visibility_Lambda([WeakThis]()
			{
				URoomCodeSubsystem* Self = WeakThis.Get();
				UWorld* W = Self ? Self->GetWorld() : nullptr;
				return (Self && W && W->GetMapName().EndsWith(TEXT("Lvl_Front")) && Self->IsInNetworkRoom(W) && !Self->CurrentRoomCode.IsEmpty())
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.BorderImage(WhiteBrush)
			.BorderBackgroundColor(VoltYellow)
			.Padding(FMargin(1.5f))
			[
				SNew(SButton)
				.ButtonStyle(FCoreStyle::Get(), "NoBorder")
				.ContentPadding(FMargin(0.f))
				.Cursor(EMouseCursor::Hand)
				.OnClicked_Lambda([WeakThis]()
				{
					if (URoomCodeSubsystem* Self = WeakThis.Get()) Self->CopyRoomCodeToClipboard();
					return FReply::Handled();
				})
				[
					SNew(SBorder)
					.BorderImage(WhiteBrush)
					.BorderBackgroundColor(CardDark)
					.Padding(FMargin(16.f, 9.f))
					[
						SNew(STextBlock)
						.Text_Lambda([WeakThis]()
						{
							URoomCodeSubsystem* Self = WeakThis.Get();
							if (!Self) return FText::GetEmpty();
							if (FPlatformTime::Seconds() < Self->CopiedFeedbackUntil)
							{
								return FText::FromString(FString::Printf(TEXT("已复制房号【%s】"), *Self->CurrentRoomCode));
							}
							const FString LanIp = Self->GetLocalLanIpOnly();
							const bool bShowIp = !LanIp.IsEmpty() && LanIp != TEXT("127.0.0.1") && !LanIp.StartsWith(TEXT("198.18."));
							if (bShowIp && Self->GetWorld() && Self->GetWorld()->GetNetMode() == NM_ListenServer)
							{
								return FText::FromString(FString::Printf(TEXT("房号【%s】 · 本机IP %s · 点击复制房号"), *Self->CurrentRoomCode, *LanIp));
							}
							return FText::FromString(FString::Printf(TEXT("房号【%s】 · 点击复制"), *Self->CurrentRoomCode));
						})
						.Font(GetSuoMaoFont(true, 13))
						.ColorAndOpacity(VoltYellow)
					]
				]
			]
		]

		// LAYER 3: Clean SuoMao In-Match F1 Pause/Leave Modal (100% hidden during normal play in Lvl_ZZZ!)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SNew(SBorder)
			.BorderImage(WhiteBrush)
			.BorderBackgroundColor(VeilColor)
			.Visibility_Lambda([WeakThis]()
			{
				URoomCodeSubsystem* Self = WeakThis.Get();
				UWorld* W = Self ? Self->GetWorld() : nullptr;
				return (Self && W && W->GetMapName().EndsWith(TEXT("Lvl_ZZZ")) && Self->bMatchRoomExpanded)
					? EVisibility::Visible : EVisibility::Collapsed;
			})
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(WhiteBrush)
				.BorderBackgroundColor(VoltYellow)
				.Padding(FMargin(2.f))
				[
					SNew(SBorder)
					.BorderImage(WhiteBrush)
					.BorderBackgroundColor(CardDark)
					.Padding(FMargin(28.f, 24.f))
					[
						SNew(SBox)
						.WidthOverride(420.f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(TEXT("OPERATION // TACTICAL MENU")))
								.Font(GetSuoMaoFont(true, 11))
								.ColorAndOpacity(VoltYellow)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 16.f)
							[
								SNew(STextBlock)
								.Text_Lambda([WeakThis]()
								{
									URoomCodeSubsystem* Self = WeakThis.Get();
									const FString Code = (Self && !Self->CurrentRoomCode.IsEmpty()) ? Self->CurrentRoomCode : TEXT("局域网对局");
									return FText::FromString(FString::Printf(TEXT("《异界：溯源信标》 · 房号 %s"), *Code));
								})
								.Font(GetSuoMaoFont(true, 18))
								.ColorAndOpacity(PaperWhite)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 12.f)
							[
								MakeSolidButton(VoltYellow, DarkInk, TEXT("继续行动  [F1 / Esc]"), 14, true, FOnClicked::CreateLambda([WeakThis]()
								{
									if (URoomCodeSubsystem* Self = WeakThis.Get()) Self->ToggleMatchRoomMenu();
									return FReply::Handled();
								}))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								MakeSolidButton(SubCardDark, PaperWhite, TEXT("退出当前房间并返回模式选择"), 13, false, FOnClicked::CreateLambda([WeakThis]()
								{
									if (URoomCodeSubsystem* Self = WeakThis.Get())
									{
										Self->bMatchRoomExpanded = false;
										Self->LeaveCurrentRoom();
									}
									return FReply::Handled();
								}))
							]
						]
					]
				]
			]
		];

	Viewport->AddViewportWidgetContent(OverlayWidget.ToSharedRef(), 100);
	bOverlayMounted = true;
}

void URoomCodeSubsystem::HostRoom()
{
	if (bLocalPreparing) return;
	if (bAutoLAN)
	{
		PrepareAutoRoom(true, TEXT(""));
		return;
	}

	UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr;
	if (!NetSub)
	{
		StatusMessage = TEXT("错误：联机会话组件不可用");
		return;
	}

	const FString GeneratedCode = FString::Printf(TEXT("%06d"), FMath::RandRange(100000, 999999));
	CurrentRoomCode = GeneratedCode;
	PendingJoinCode.Empty();
	bClientReadyLocked = false;
	LocalConnectionAddress = DetectLocalAddress(GetWorld());
	NetSub->SetDesiredRoomCode(CurrentRoomCode);

	StatusMessage = FString::Printf(TEXT("正在创建房间【%s】并进入选人阶段…"), *CurrentRoomCode);
	if (!NetSub->CreateRoom() && NetSub->GetSessionState() != ENetSessionState::InLobby && NetSub->GetSessionState() != ENetSessionState::Traveling)
	{
		CurrentRoomCode.Empty();
		StatusMessage = FString::Printf(TEXT("创建房间失败：%s"), *NetSub->GetLastErrorMessage());
		return;
	}

	bRoomEntryModalOpen = false;
	UE_LOG(LogTemp, Display, TEXT("[RoomCode] LAN_HOST_CREATED code=%s addr=%s"), *CurrentRoomCode, *LocalConnectionAddress);

	// Optional HTTP backend registration only when explicitly launched with -BackendUrl=
	if (!BackendUrl.IsEmpty())
	{
		CreatePreparedRoom();
	}
}

void URoomCodeSubsystem::JoinRoom(const FString& RoomCode)
{
	const FString Trimmed = RoomCode.TrimStartAndEnd();
	if (bLocalPreparing) return;
	if (Trimmed.IsEmpty())
	{
		StatusMessage = TEXT("请输入 6 位数字房号（或房主局域网 IP）！");
		return;
	}

	UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr;
	if (!NetSub)
	{
		StatusMessage = TEXT("错误：联机会话组件不可用");
		return;
	}

	// Direct IP fallback (e.g., 192.168.1.105 or 127.0.0.1:7777) in case hackathon Wi-Fi blocks LAN UDP broadcast
	if (Trimmed.Contains(TEXT(".")) || Trimmed.Contains(TEXT(":")))
	{
		FString HostIpOnly = Trimmed;
		FString PortPart;
		if (Trimmed.Split(TEXT(":"), &HostIpOnly, &PortPart))
		{
			HostIpOnly = HostIpOnly.TrimStartAndEnd();
		}
		const FString TargetAddr = Trimmed.Contains(TEXT(":")) ? Trimmed : (Trimmed + TEXT(":7777"));
		PendingJoinCode = Trimmed;
		bClientReadyLocked = false;
		if (bAutoLAN && !HostIpOnly.IsEmpty() && HostIpOnly != TEXT("127.0.0.1"))
		{
			SelectedForgeUrl = FString::Printf(TEXT("http://%s:8020"), *HostIpOnly);
		}
		StatusMessage = FString::Printf(TEXT("正在直连房主 %s …"), *TargetAddr);
		if (NetSub->DebugConnectToAddress(TargetAddr))
		{
			CurrentRoomCode = Trimmed;
			bRoomEntryModalOpen = false;
		}
		else
		{
			PendingJoinCode.Empty();
			StatusMessage = FString::Printf(TEXT("无法直连 %s，请检查地址后重试"), *TargetAddr);
		}
		return;
	}

	if (Trimmed.Len() != 6 || !Trimmed.IsNumeric())
	{
		StatusMessage = TEXT("请输入 6 位纯数字房间号（如 482917）或房主 IP！");
		return;
	}

	if (bAutoLAN)
	{
		PrepareAutoRoom(false, Trimmed);
		return;
	}
	if (!BackendUrl.IsEmpty())
	{
		QueryPreparedRoom(Trimmed);
		return;
	}

	// Zero-config 2-PC LAN discovery via OnlineSubsystemNull ROOMCODE advertisement
	PendingJoinCode = Trimmed;
	bClientReadyLocked = false;
	NetSub->SetDesiredRoomCode(Trimmed);
	StatusMessage = FString::Printf(TEXT("正在局域网搜索房间【%s】…"), *Trimmed);
	if (!NetSub->FindAndJoinRoom())
	{
		PendingJoinCode.Empty();
		StatusMessage = FString::Printf(TEXT("搜房请求繁忙，请稍候重试（%s）"), *NetSub->GetLastErrorMessage());
	}
}

void URoomCodeSubsystem::StartMatch()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;
	if (World->GetMapName().EndsWith(TEXT("Lvl_ZZZ")))
	{
		StatusMessage = TEXT("已在对局中。");
		return;
	}

	UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr;
	if (!NetSub || !NetSub->IsRoomHost())
	{
		StatusMessage = TEXT("只有房主可以发起【开始对局】！");
		return;
	}

	const int32 PlayerCount = GetConnectedPlayerCount(World);
	if (PlayerCount < 2)
	{
		StatusMessage = TEXT("至少需要 2 名玩家进入房间，才可开始信标争夺！");
		return;
	}

	if (!BackendUrl.IsEmpty() && !CurrentRoomCode.IsEmpty() && !CurrentManageToken.IsEmpty())
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> StartReq = FHttpModule::Get().CreateRequest();
		StartReq->OnProcessRequestComplete().BindUObject(this, &URoomCodeSubsystem::OnStartMatchResponse);
		StartReq->SetURL(FString::Printf(TEXT("%s/rooms/%s/start"), *BackendUrl, *CurrentRoomCode));
		StartReq->SetVerb(TEXT("POST"));
		StartReq->SetHeader(TEXT("X-Manage-Token"), CurrentManageToken);
		StartReq->ProcessRequest();
	}

	if (NetSub->TravelToMatch())
	{
		StatusMessage = FString::Printf(TEXT("行动开始！全员正在进入 8 分钟信标争夺战场…"));
	}
}

void URoomCodeSubsystem::LeaveCurrentRoom()
{
	if (LocalPreparation)
	{
		LocalPreparation->OnProcessRequestComplete().Unbind();
		LocalPreparation->CancelRequest();
		LocalPreparation.Reset();
	}
	bLocalPreparing = false;
	bPublishWhenReady = false;
	bClientReadyLocked = false;
	bRoomEntryModalOpen = false;
	bMatchRoomExpanded = false;
	bReturnToModeOnFront = true;
	LocalAction(TEXT("leave"));
	SelectedForgeUrl.Empty();
	DiscoveredConnection.Empty();

	if (!BackendUrl.IsEmpty() && !CurrentRoomCode.IsEmpty() && !CurrentManageToken.IsEmpty())
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> RevokeReq = FHttpModule::Get().CreateRequest();
		RevokeReq->SetURL(FString::Printf(TEXT("%s/rooms/%s"), *BackendUrl, *CurrentRoomCode));
		RevokeReq->SetVerb(TEXT("DELETE"));
		RevokeReq->SetHeader(TEXT("X-Manage-Token"), CurrentManageToken);
		RevokeReq->ProcessRequest();
	}
	else if (!BackendUrl.IsEmpty() && !CurrentReservationId.IsEmpty())
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> ReleaseReq = FHttpModule::Get().CreateRequest();
		ReleaseReq->SetURL(FString::Printf(TEXT("%s/reservations/%s/release"), *BackendUrl, *CurrentReservationId));
		ReleaseReq->SetVerb(TEXT("POST"));
		ReleaseReq->ProcessRequest();
	}

	CurrentRoomCode.Empty();
	PendingJoinCode.Empty();
	CurrentManageToken.Empty();
	CurrentReservationId.Empty();

	if (UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr)
	{
		NetSub->SetDesiredRoomCode(FString());
		if (!NetSub->LeaveRoom())
		{
			NetSub->ResetSessionState();
		}
	}

	if (UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr)
	{
		if (World->GetNetMode() == NM_Client)
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				PC->ClientTravel(TEXT("/Game/SuoMaoGame/Lvl_Front"), TRAVEL_Absolute);
			}
		}
	}
	StatusMessage = TEXT("已离开房间。");
}

// ---------- Legacy AutoLAN / optional HTTP backend compatibility ----------

void URoomCodeSubsystem::LocalAction(const FString& Action, const FString& Body)
{
	if (!bAutoLAN) return;
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(TEXT("http://127.0.0.1:18902/local/") + Action);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("X-Lux-Local"), LocalServiceToken);
	Request->SetContentAsString(Body);
	Request->SetTimeout(90.f);
	Request->ProcessRequest();
}

void URoomCodeSubsystem::PrepareAutoRoom(bool bHost, const FString& Code)
{
	bLocalPreparing = true;
	StatusMessage = bHost ? TEXT("正在准备房间，请稍候…") : TEXT("正在同一网络查找房号…");
	LocalPreparation = FHttpModule::Get().CreateRequest();
	LocalPreparation->SetURL(bHost ? TEXT("http://127.0.0.1:18902/local/host") : TEXT("http://127.0.0.1:18902/local/find"));
	LocalPreparation->SetVerb(TEXT("POST"));
	LocalPreparation->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	LocalPreparation->SetHeader(TEXT("X-Lux-Local"), LocalServiceToken);
	LocalPreparation->SetTimeout(bHost ? 95.f : 8.f);
	LocalPreparation->SetContentAsString(FString::Printf(TEXT("{\"code\":\"%s\"}"), *Code));
	LocalPreparation->OnProcessRequestComplete().BindWeakLambda(this, [this, bHost, Code](FHttpRequestPtr, FHttpResponsePtr Response, bool bOK)
	{
		bLocalPreparing = false;
		LocalPreparation.Reset();
		TSharedPtr<FJsonObject> Json;
		if (!bOK || !Response || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) || !Json)
		{
			StatusMessage = TEXT("本机试玩后台未就绪，请检查启动入口。");
			return;
		}
		if (Response->GetResponseCode() != 200)
		{
			if (!Json->TryGetStringField(TEXT("error"), StatusMessage))
			{
				StatusMessage = TEXT("未找到该房号；若路由器开启了局域网广播隔离，可直接输入房主右上角显示的本机IP加入！");
			}
			else if (!bHost)
			{
				StatusMessage += TEXT("（若开启梯子或路由器隔离，也可直接输入房主右上角本机IP加入）");
			}
			return;
		}
		BackendUrl = Json->GetStringField(TEXT("backend_url"));
		SelectedForgeUrl = Json->GetStringField(TEXT("forge_url"));
		DiscoveredConnection = bHost ? FString() : Json->GetStringField(TEXT("host_ip")) + TEXT(":7777");
		if (bHost) CreatePreparedRoom(); else QueryPreparedRoom(Code);
	});
	LocalPreparation->ProcessRequest();
}

void URoomCodeSubsystem::PublishReadyRoom()
{
	bPublishWhenReady = false;
	if (!bAutoLAN) return;
	bLocalPreparing = true;
	LocalPreparation = FHttpModule::Get().CreateRequest();
	LocalPreparation->SetURL(TEXT("http://127.0.0.1:18902/local/publish"));
	LocalPreparation->SetVerb(TEXT("POST"));
	LocalPreparation->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	LocalPreparation->SetHeader(TEXT("X-Lux-Local"), LocalServiceToken);
	LocalPreparation->SetContentAsString(FString::Printf(TEXT("{\"code\":\"%s\"}"), *CurrentRoomCode));
	LocalPreparation->SetTimeout(5.f);
	LocalPreparation->OnProcessRequestComplete().BindWeakLambda(this, [this](FHttpRequestPtr, FHttpResponsePtr Response, bool bOK)
	{
		bLocalPreparing = false;
		LocalPreparation.Reset();
		StatusMessage = bOK && Response && Response->GetResponseCode() == 200
			? FString::Printf(TEXT("房间已就绪！队友输入房号【%s】即可加入。"), *CurrentRoomCode)
			: TEXT("房间发现发布失败，请重试。");
	});
	LocalPreparation->ProcessRequest();
}

void URoomCodeSubsystem::CreatePreparedRoom()
{
	UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr;
	if (bAutoLAN && NetSub && NetSub->GetSessionState() == ENetSessionState::Idle)
	{
		NetSub->CreateRoom();
	}

	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	LocalConnectionAddress = DetectLocalAddress(World);
	const FString SessionId = FString::Printf(TEXT("sess-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString IdempotencyKey = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("provider"), TEXT("ue-session"));
	Body->SetStringField(TEXT("session_id"), SessionId);
	Body->SetStringField(TEXT("connection"), LocalConnectionAddress);
	Body->SetStringField(TEXT("protocol_version"), ProtocolVersion);
	Body->SetStringField(TEXT("idempotency_key"), IdempotencyKey);
	Body->SetStringField(TEXT("display_version"), TEXT("hackathon-demo"));
	Body->SetNumberField(TEXT("ttl_seconds"), 3600);

	FString Payload;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->OnProcessRequestComplete().BindUObject(this, &URoomCodeSubsystem::OnHostRoomResponse);
	Request->SetURL(BackendUrl + TEXT("/rooms"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Payload);
	Request->ProcessRequest();
}

void URoomCodeSubsystem::OnHostRoomResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	if (!bConnectedSuccessfully || !Response.IsValid()) return;
	TSharedPtr<FJsonObject> JsonObject;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), JsonObject) || !JsonObject.IsValid()) return;
	const int32 Code = Response->GetResponseCode();
	if (Code == 200 || Code == 201)
	{
		CurrentRoomCode = JsonObject->GetStringField(TEXT("short_code"));
		CurrentManageToken = JsonObject->GetStringField(TEXT("manage_token"));
		bPublishWhenReady = bAutoLAN;
		bRoomEntryModalOpen = false;
	}
}

void URoomCodeSubsystem::QueryPreparedRoom(const FString& Trimmed)
{
	StatusMessage = FString::Printf(TEXT("正在查询房间号 %s …"), *Trimmed);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->OnProcessRequestComplete().BindUObject(this, &URoomCodeSubsystem::OnJoinRoomResponse, Trimmed);
	Request->SetURL(FString::Printf(TEXT("%s/rooms/%s?protocol_version=%s"), *BackendUrl, *Trimmed, *ProtocolVersion));
	Request->SetVerb(TEXT("GET"));
	Request->ProcessRequest();
}

void URoomCodeSubsystem::OnJoinRoomResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, FString RequestedCode)
{
	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		StatusMessage = TEXT("无法连接房间后端，请确认网络后重试。");
		return;
	}
	TSharedPtr<FJsonObject> JsonObject;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), JsonObject);
	if (Response->GetResponseCode() != 200 || !JsonObject.IsValid())
	{
		StatusMessage = FString::Printf(TEXT("无法加入房间 %s，请核对房号。"), *RequestedCode);
		return;
	}
	const FString TargetConnection = DiscoveredConnection.IsEmpty() ? JsonObject->GetStringField(TEXT("connection")) : DiscoveredConnection;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("player_name"), TEXT("Player"));
	Body->SetStringField(TEXT("protocol_version"), ProtocolVersion);
	Body->SetNumberField(TEXT("ttl_seconds"), 60);
	FString Payload;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Payload);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> ReserveReq = FHttpModule::Get().CreateRequest();
	ReserveReq->OnProcessRequestComplete().BindUObject(this, &URoomCodeSubsystem::OnReserveSeatResponse, TargetConnection, RequestedCode);
	ReserveReq->SetURL(FString::Printf(TEXT("%s/rooms/%s/reservations"), *BackendUrl, *RequestedCode));
	ReserveReq->SetVerb(TEXT("POST"));
	ReserveReq->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	ReserveReq->SetContentAsString(Payload);
	ReserveReq->ProcessRequest();
}

void URoomCodeSubsystem::OnReserveSeatResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, FString TargetConnection, FString RoomCode)
{
	if (!bConnectedSuccessfully || !Response.IsValid()) { StatusMessage = TEXT("预留房间席位失败"); return; }
	TSharedPtr<FJsonObject> JsonObject;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), JsonObject);
	if (Response->GetResponseCode() != 201 || !JsonObject.IsValid()) { StatusMessage = TEXT("房间已满或已开始对局"); return; }
	CurrentRoomCode = RoomCode;
	CurrentReservationId = JsonObject->GetStringField(TEXT("reservation_id"));
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> ConfirmReq = FHttpModule::Get().CreateRequest();
	ConfirmReq->SetURL(FString::Printf(TEXT("%s/reservations/%s/confirm"), *BackendUrl, *CurrentReservationId));
	ConfirmReq->SetVerb(TEXT("POST"));
	ConfirmReq->ProcessRequest();
	UNetSessionSubsystem* NetSub = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNetSessionSubsystem>() : nullptr;
	if (NetSub && NetSub->DebugConnectToAddress(TargetConnection))
	{
		bRoomEntryModalOpen = false;
		StatusMessage = FString::Printf(TEXT("已进入房间【%s】，正在跳转选人阶段…"), *CurrentRoomCode);
	}
	else
	{
		StatusMessage = TEXT("连接房主失败，请重试。");
	}
}

void URoomCodeSubsystem::OnStartMatchResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	if (bConnectedSuccessfully && Response.IsValid() && Response->GetResponseCode() == 200)
	{
		UE_LOG(LogTemp, Log, TEXT("[RoomCode] MATCH_STARTED locked room=%s"), *CurrentRoomCode);
	}
}

#if !UE_BUILD_SHIPPING
void URoomCodeSubsystem::TickE2ERoomTest(UWorld* World, float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	if (E2EStepSince <= 0.0) E2EStepSince = Now;
	const double Elapsed = Now - E2EStepSince;
	const FString MapName = World->GetMapName();
	const auto Advance = [&](int32 NextStep) { E2EStep = NextStep; E2EStepSince = Now; };
	const auto Shot = [&](const TCHAR* Name)
	{
		const FString FullPath = E2EShotDir / Name;
		FScreenshotRequest::RequestScreenshot(FullPath, true, false);
		UE_LOG(LogTemp, Display, TEXT("[E2E] SHOT %s"), *FullPath);
	};
	auto FindRoot = [&]() -> UUserWidget*
	{
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			if (*It && !(*It)->HasAnyFlags(RF_ClassDefaultObject) && (*It)->GetWorld() == World
				&& (*It)->GetClass()->GetName().StartsWith(TEXT("WBP_SM_Root")))
				return *It;
		}
		return nullptr;
	};

	const FString CodeFile = E2EShotDir / TEXT("e2e_room_code.txt");
	if (E2ERole.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
	{
		if (E2EStep == 0 && MapName.EndsWith(TEXT("Lvl_Front")) && Elapsed >= 2.0)
		{
			Shot(TEXT("01_front_login.png"));
			Advance(10);
		}
		else if (E2EStep == 10 && Elapsed >= 0.35)
		{
			if (UUserWidget* Root = FindRoot()) SwitchSuoMaoScreen(Root, TEXT("Home"));
			Advance(1);
		}
		else if (E2EStep == 1 && Elapsed >= 2.0)
		{
			Shot(TEXT("02_front_home.png"));
			Advance(11);
		}
		else if (E2EStep == 11 && Elapsed >= 0.35)
		{
			if (UUserWidget* Root = FindRoot()) SwitchSuoMaoScreen(Root, TEXT("Mode"));
			Advance(2);
		}
		else if (E2EStep == 2 && Elapsed >= 2.0)
		{
			Shot(TEXT("03_front_mode.png"));
			Advance(12);
		}
		else if (E2EStep == 12 && Elapsed >= 0.35)
		{
			OnModeGoClicked();
			Advance(3);
		}
		else if (E2EStep == 3 && Elapsed >= 1.2)
		{
			Shot(TEXT("04_room_modal.png"));
			Advance(13);
		}
		else if (E2EStep == 13 && Elapsed >= 0.35)
		{
			HostRoom();
			FFileHelper::SaveStringToFile(CurrentRoomCode, *CodeFile);
			UE_LOG(LogTemp, Display, TEXT("[E2E] HOST_WROTE_CODE %s"), *CurrentRoomCode);
			Advance(4);
		}
		else if (E2EStep == 4 && MapName.EndsWith(TEXT("Lvl_Front")) && World->GetNetMode() == NM_ListenServer && Elapsed >= 2.5)
		{
			Shot(TEXT("05_host_charsel_1of4.png"));
			Advance(5);
		}
		else if (E2EStep == 5 && MapName.EndsWith(TEXT("Lvl_Front")) && World->GetNetMode() == NM_ListenServer && GetConnectedPlayerCount(World) >= 2)
		{
			if (FPaths::FileExists(E2EShotDir / TEXT("client_charsel_done.txt")))
			{
				Shot(TEXT("06_host_charsel_2of4.png"));
				Advance(15);
			}
		}
		else if (E2EStep == 15 && Elapsed >= 0.35)
		{
			OnCharSelLockClicked();
			Advance(6);
		}
		else if (E2EStep == 6 && MapName.EndsWith(TEXT("Lvl_ZZZ")))
		{
			const auto* Match = ACombatMatchActor::Find(World);
			if (Match && Match->GetPhase() == ECombatRoundPhase::Playing && Elapsed >= 2.5)
			{
				if (FParse::Param(FCommandLine::Get(), TEXT("E2EAnimTest")))
				{
					// Bring Client right in front of Host facing Host so both cameras clearly frame each other!
					ACharacter* HostChar = Cast<ACharacter>(World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr);
					ACharacter* RemoteChar = nullptr;
					for (TActorIterator<ACharacter> It(World); It; ++It)
					{
						if (*It && *It != HostChar && It->IsPlayerControlled()) { RemoteChar = *It; break; }
					}
					if (HostChar && RemoteChar)
					{
						const FVector Fwd = HostChar->GetActorForwardVector().GetSafeNormal2D();
						const FVector TargetLoc = HostChar->GetActorLocation() + Fwd * 230.f;
						RemoteChar->SetActorLocationAndRotation(TargetLoc, (-Fwd).Rotation(), false, nullptr, ETeleportType::TeleportPhysics);
						if (auto* RC = Cast<APlayerController>(RemoteChar->GetController()))
						{
							RC->SetControlRotation((-Fwd).Rotation());
							RC->ClientSetRotation((-Fwd).Rotation(), true);
						}
						if (auto* RCombat = RemoteChar->FindComponentByClass<UPlayerCombatComponent>())
						{
							// Make sure Client snaps visually too
							RemoteChar->ForceNetUpdate();
						}
						// Start Host moving + Miyabi blade attack!
						if (auto* FwdP = FindFProperty<FDoubleProperty>(HostChar->GetClass(), TEXT("ForwardInput")))
							FwdP->SetPropertyValue_InContainer(HostChar, 1.0);
						else if (auto* FwdF = FindFProperty<FFloatProperty>(HostChar->GetClass(), TEXT("ForwardInput")))
							FwdF->SetPropertyValue_InContainer(HostChar, 1.0f);
						if (UFunction* BladeFn = HostChar->FindFunction(TEXT("TrioBotBladeAttack")))
							HostChar->ProcessEvent(BladeFn, nullptr);
						UE_LOG(LogTemp, Display, TEXT("[E2EAnim] HOST_TRIGGERED_MOVE_AND_BLADE host=%s remote=%s"),
							*HostChar->GetClass()->GetName(), *RemoteChar->GetClass()->GetName());
						Advance(70);
					}
				}
				else
				{
					Shot(TEXT("08_host_match_hud.png"));
					Advance(7);
				}
			}
		}
		else if (E2EStep == 70 && MapName.EndsWith(TEXT("Lvl_ZZZ")))
		{
			// Keep Host running & re-triggering combo while logging what Host sees on Client's remote pawn!
			ACharacter* HostChar = Cast<ACharacter>(World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr);
			if (HostChar)
			{
				if (auto* FwdP = FindFProperty<FDoubleProperty>(HostChar->GetClass(), TEXT("ForwardInput")))
					FwdP->SetPropertyValue_InContainer(HostChar, 1.0);
				else if (auto* FwdF = FindFProperty<FFloatProperty>(HostChar->GetClass(), TEXT("ForwardInput")))
					FwdF->SetPropertyValue_InContainer(HostChar, 1.0f);
			}
			if (Elapsed >= 1.2)
			{
				for (TActorIterator<ACharacter> It(World); It; ++It)
				{
					ACharacter* Ch = *It;
					if (!Ch || Ch->IsLocallyControlled()) continue;
					UAnimInstance* Anim = Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
					UAnimMontage* ActiveM = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
					bool bHasInput = false;
					if (Anim)
						if (const auto* P = FindFProperty<FBoolProperty>(Anim->GetClass(), TEXT("HasInput")))
							bHasInput = P->GetPropertyValue_InContainer(Anim);
					double AnimSpeed = 0.0;
					if (Anim)
					{
						if (const auto* P = FindFProperty<FDoubleProperty>(Anim->GetClass(), TEXT("Speed")))
							AnimSpeed = P->GetPropertyValue_InContainer(Anim);
						else if (const auto* PF = FindFProperty<FFloatProperty>(Anim->GetClass(), TEXT("Speed")))
							AnimSpeed = PF->GetPropertyValue_InContainer(Anim);
					}
					UE_LOG(LogTemp, Display, TEXT("[E2EAnim] HOST_SEES_REMOTE class=%s vel=%.1f animSpeed=%.1f hasInput=%d montage=%s"),
						*Ch->GetClass()->GetName(), Ch->GetVelocity().Size2D(), AnimSpeed, bHasInput ? 1 : 0,
						ActiveM ? *ActiveM->GetName() : TEXT("NONE"));
				}
				Shot(TEXT("anim_host_sees_client.png"));
				Advance(71);
			}
		}
		else if (E2EStep == 71 && (FPaths::FileExists(E2EShotDir / TEXT("client_match_done.txt")) || Elapsed >= 8.0))
		{
			UE_LOG(LogTemp, Display, TEXT("[E2E] HOST_DONE"));
			FPlatformMisc::RequestExit(false);
		}
		else if (E2EStep == 7 && Elapsed >= 1.5)
		{
			ToggleMatchRoomMenu();
			Advance(8);
		}
		else if (E2EStep == 8 && Elapsed >= 1.0)
		{
			Shot(TEXT("10_host_match_f1_menu.png"));
			Advance(9);
		}
		else if (E2EStep == 9 && (FPaths::FileExists(E2EShotDir / TEXT("client_match_done.txt")) || Elapsed >= 12.0))
		{
			UE_LOG(LogTemp, Display, TEXT("[E2E] HOST_DONE"));
			FPlatformMisc::RequestExit(false);
		}
	}
	else if (E2ERole.Equals(TEXT("Client"), ESearchCase::IgnoreCase))
	{
		if (E2EStep == 0 && MapName.EndsWith(TEXT("Lvl_Front")) && Elapsed >= 1.5)
		{
			FString Code;
			if (FFileHelper::LoadFileToString(Code, *CodeFile))
			{
				Code = Code.TrimStartAndEnd();
				if (Code.Len() == 6)
				{
					UE_LOG(LogTemp, Display, TEXT("[E2E] CLIENT_JOIN_CODE %s"), *Code);
					JoinRoom(Code);
					Advance(1);
				}
			}
		}
		else if (E2EStep == 1 && MapName.EndsWith(TEXT("Lvl_Front")) && World->GetNetMode() == NM_Client && Elapsed >= 2.5)
		{
			// Invoke WBP_CharSel::SelectItem(1) so Client genuinely selects 莱卡 (BP_Yousun)!
			if (UUserWidget* CS = HookedCharSelWidget.Get())
			{
				if (UFunction* SelectFn = CS->FindFunction(TEXT("SelectItem")))
				{
					FStructOnScope Params(SelectFn);
					for (TFieldIterator<FProperty> PropIt(SelectFn); PropIt; ++PropIt)
					{
						if (FIntProperty* IntP = CastField<FIntProperty>(*PropIt))
						{
							IntP->SetPropertyValue_InContainer(Params.GetStructMemory(), 1);
							CS->ProcessEvent(SelectFn, Params.GetStructMemory());
							break;
						}
					}
				}
				if (FIntProperty* SelProp = FindFProperty<FIntProperty>(CS->GetClass(), TEXT("Sel")))
				{
					SelProp->SetPropertyValue_InContainer(CS, 1);
				}
			}
			SelectedCharacterIndex = 1;
			OnCharSelLockClicked();
			Advance(2);
		}
		else if (E2EStep == 2 && Elapsed >= 2.0)
		{
			Shot(TEXT("07_client_charsel_2of4.png"));
			FFileHelper::SaveStringToFile(TEXT("ok"), *(E2EShotDir / TEXT("client_charsel_done.txt")));
			Advance(3);
		}
		else if (E2EStep == 3 && MapName.EndsWith(TEXT("Lvl_ZZZ")))
		{
			const auto* Match = ACombatMatchActor::Find(World);
			if (Match && Match->GetPhase() == ECombatRoundPhase::Playing && Elapsed >= 2.0)
			{
				if (FParse::Param(FCommandLine::Get(), TEXT("E2EAnimTest")))
				{
					// Client (Yousun) starts moving AND plays Body_Fire + Weapon_Fire dynamic slot montage!
					if (ACharacter* ClientChar = Cast<ACharacter>(World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr))
					{
						ClientChar->AddMovementInput(ClientChar->GetActorForwardVector(), 1.0f);
						if (UAnimInstance* Anim = ClientChar->GetMesh() ? ClientChar->GetMesh()->GetAnimInstance() : nullptr)
						{
							if (UAnimSequenceBase* Seq = LoadObject<UAnimSequenceBase>(nullptr, TEXT("/Game/character/yousun/Combat/Animations/Body_Aim.Body_Aim")))
							{
								Anim->PlaySlotAnimationAsDynamicMontage(Seq, TEXT("YousunCombat"), 0.08f, 0.2f);
							}
						}
						if (const auto* WeaponProp = FindFProperty<FObjectPropertyBase>(ClientChar->GetClass(), TEXT("YousunWeapon")))
						{
							if (auto* WMesh = Cast<USkeletalMeshComponent>(WeaponProp->GetObjectPropertyValue_InContainer(ClientChar)))
								if (UAnimInstance* WAnim = WMesh->GetAnimInstance())
									if (UAnimSequenceBase* WSeq = LoadObject<UAnimSequenceBase>(nullptr, TEXT("/Game/character/yousun/Combat/Animations/Weapon_Aim.Weapon_Aim")))
										WAnim->PlaySlotAnimationAsDynamicMontage(WSeq, TEXT("YousunCombat"), 0.08f, 0.2f);
						}
						UE_LOG(LogTemp, Display, TEXT("[E2EAnim] CLIENT_TRIGGERED_MOVE_AND_AIM class=%s"), *ClientChar->GetClass()->GetName());
					}
					Advance(30);
				}
				else
				{
					Shot(TEXT("09_client_match_hud.png"));
					FFileHelper::SaveStringToFile(TEXT("ok"), *(E2EShotDir / TEXT("client_match_done.txt")));
					UE_LOG(LogTemp, Display, TEXT("[E2E] CLIENT_DONE"));
					Advance(4);
				}
			}
		}
		else if (E2EStep == 30 && MapName.EndsWith(TEXT("Lvl_ZZZ")))
		{
			if (ACharacter* ClientChar = Cast<ACharacter>(World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr))
			{
				ClientChar->AddMovementInput(ClientChar->GetActorForwardVector(), 1.0f);
			}
			if (Elapsed >= 1.5)
			{
				for (TActorIterator<ACharacter> It(World); It; ++It)
				{
					ACharacter* Ch = *It;
					if (!Ch || Ch->IsLocallyControlled()) continue;
					UAnimInstance* Anim = Ch->GetMesh() ? Ch->GetMesh()->GetAnimInstance() : nullptr;
					UAnimMontage* ActiveM = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
					double FwdVal = 0.0;
					if (const auto* P = FindFProperty<FDoubleProperty>(Ch->GetClass(), TEXT("ForwardInput")))
						FwdVal = P->GetPropertyValue_InContainer(Ch);
					else if (const auto* PF = FindFProperty<FFloatProperty>(Ch->GetClass(), TEXT("ForwardInput")))
						FwdVal = PF->GetPropertyValue_InContainer(Ch);
					int32 MoveStateVal = -1;
					if (Anim)
					{
						if (const auto* BP = FindFProperty<FByteProperty>(Anim->GetClass(), TEXT("CurrentMoveState")))
							MoveStateVal = BP->GetPropertyValue_InContainer(Anim);
						else if (const auto* EP = FindFProperty<FEnumProperty>(Anim->GetClass(), TEXT("CurrentMoveState")))
							MoveStateVal = static_cast<int32>(EP->GetUnderlyingProperty()->GetUnsignedIntPropertyValue(EP->ContainerPtrToValuePtr<void>(Anim)));
					}
					UE_LOG(LogTemp, Display, TEXT("[E2EAnim] CLIENT_SEES_REMOTE class=%s vel=%.1f fwd=%.2f moveState=%d montage=%s"),
						*Ch->GetClass()->GetName(), Ch->GetVelocity().Size2D(), FwdVal, MoveStateVal,
						ActiveM ? *ActiveM->GetName() : TEXT("NONE"));
				}
				Shot(TEXT("anim_client_sees_host.png"));
				FFileHelper::SaveStringToFile(TEXT("ok"), *(E2EShotDir / TEXT("client_match_done.txt")));
				UE_LOG(LogTemp, Display, TEXT("[E2E] CLIENT_DONE"));
				Advance(4);
			}
		}
		else if (E2EStep == 4 && Elapsed >= 1.5)
		{
			FPlatformMisc::RequestExit(false);
		}
	}
}
#endif
