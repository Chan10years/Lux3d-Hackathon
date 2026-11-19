#include "CombatSubsystem.h"
#include "PlayerCombatComponent.h"
#include "WeaponForgeSubsystem.h"
#include "WeaponSyncComponent.h"
#include "RoomCodeSubsystem.h"
#include "CombatRules.h"
#include "CombatMatchActor.h"
#include "GoldenHammerActor.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/InputComponent.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/UnrealType.h"

namespace
{
    const TCHAR* HeroNameCN(int32 Choice)
    {
        switch (Choice)
        {
        case 0: return TEXT("薇奥");
        case 1: return TEXT("莱卡");
        case 2: return TEXT("凯恩");
        default: return TEXT("旅人");
        }
    }

    int32 InferHeroIndexFromClass(const AActor* Actor)
    {
        if (!Actor) return 0;
        const FString Path = Actor->GetClass()->GetPathName();
        if (Path.Contains(TEXT("BP_Yousun"))) return 1;
        if (Path.Contains(TEXT("BP_MyCharacter"))) return 2;
        return 0;
    }

    void SetWidgetText(UUserWidget* Root, const TCHAR* Name, const FString& Value)
    {
        if (!Root) return;
        if (UTextBlock* TB = Cast<UTextBlock>(Root->GetWidgetFromName(Name)))
        {
            TB->SetText(FText::FromString(Value));
        }
    }

    void SetWidgetVisible(UUserWidget* Root, const TCHAR* Name, bool bVisible)
    {
        if (!Root) return;
        if (UWidget* W = Root->GetWidgetFromName(Name))
        {
            W->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
            if (bVisible && W->GetRenderOpacity() < 0.05f)
            {
                W->SetRenderOpacity(1.0f);
            }
        }
    }

    const TCHAR* DirectionLabel(const ACharacter* Player, const FVector& TargetWorldPos)
    {
        if (!Player) return TEXT("前方");
        const FVector Delta = TargetWorldPos - Player->GetActorLocation();
        const FVector Local = Player->GetActorRotation().UnrotateVector(Delta);
        const float Angle = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X));
        if (Angle >= -22.5f && Angle < 22.5f) return TEXT("正前方");
        if (Angle >= 22.5f && Angle < 67.5f) return TEXT("右前方");
        if (Angle >= 67.5f && Angle < 112.5f) return TEXT("右侧");
        if (Angle >= 112.5f && Angle < 157.5f) return TEXT("右后方");
        if (Angle >= -67.5f && Angle < -22.5f) return TEXT("左前方");
        if (Angle >= -112.5f && Angle < -67.5f) return TEXT("左侧");
        if (Angle >= -157.5f && Angle < -112.5f) return TEXT("左后方");
        return TEXT("正后方");
    }
}

bool UCombatSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const auto* World = Cast<UWorld>(Outer);
    if (!World || !World->IsGameWorld()) return false;
    if (World->GetMapName().EndsWith(TEXT("Lvl_Front"))) return false;
    return Super::ShouldCreateSubsystem(Outer);
}

TStatId UCombatSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UCombatSubsystem, STATGROUP_Tickables);
}

void UCombatSubsystem::Tick(float DeltaTime)
{
    UWorld* World = GetWorld();
    if (!World || !World->HasBegunPlay()) return;

    if (GEngine)
    {
        GEngine->bEnableOnScreenDebugMessages = false;
        GEngine->bEnableOnScreenDebugMessagesDisplay = false;
    }

    if (World->GetNetMode() != NM_Client)
    {
        const bool InMatchMap = FString(World->URL.GetOption(TEXT("NetPhase="), TEXT(""))) == TEXT("Match")
            || World->GetMapName().EndsWith(TEXT("Lvl_ZZZ"));
        const auto* Match = ACombatMatchActor::Find(World);
        const bool Active = InMatchMap && Match && Match->IsMatchActive();
        bool HasPlayer = false;
        for (TActorIterator<ACharacter> It(World); It; ++It)
        {
            ACharacter* Player = *It;
            if (!Player->IsPlayerControlled()) continue;
            HasPlayer = true;
            auto* Combat = Player->FindComponentByClass<UPlayerCombatComponent>();
            if (!Combat)
            {
                Combat = NewObject<UPlayerCombatComponent>(Player, TEXT("PlayerCombat"));
                Player->AddInstanceComponent(Combat);
                Combat->RegisterComponent();
            }
            Combat->SetCombatActive(Active);
        }
        if (InMatchMap && HasPlayer && !Match)
        {
            FActorSpawnParameters Params;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            World->SpawnActor<ACombatMatchActor>(ACombatMatchActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
        }
    }

    APlayerController* PC = World->GetFirstPlayerController();
    if (!PC || !PC->IsLocalPlayerController() || !World->GetGameViewport()) return;

    if (Controller.Get() != PC)
    {
        if (Controller.IsValid() && CombatInput) Controller->PopInputComponent(CombatInput);
        Controller = PC;
        CombatInput = NewObject<UInputComponent>(PC);
        CombatInput->Priority = 30;
        CombatInput->RegisterComponent();
        // Do not consume LeftMouseButton so chosen 3D hero Blueprints (Miyabi/Yousun/MyCharacter) play their native attack animations.
        CombatInput->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &UCombatSubsystem::Attack).bConsumeInput = false;
        CombatInput->BindKey(EKeys::Four, IE_Pressed, this, &UCombatSubsystem::SelectGun);
        // Block single-player Trio character-switch (1/2/3) & bot toggle (5) keys during match so multiplayer pawns never desync.
        CombatInput->BindKey(EKeys::One, IE_Pressed, this, &UCombatSubsystem::BlockSinglePlayerKey).bConsumeInput = true;
        CombatInput->BindKey(EKeys::Two, IE_Pressed, this, &UCombatSubsystem::BlockSinglePlayerKey).bConsumeInput = true;
        CombatInput->BindKey(EKeys::Three, IE_Pressed, this, &UCombatSubsystem::BlockSinglePlayerKey).bConsumeInput = true;
        CombatInput->BindKey(EKeys::Five, IE_Pressed, this, &UCombatSubsystem::BlockSinglePlayerKey).bConsumeInput = true;
        PC->PushInputComponent(CombatInput);
    }

    if (!SuoMaoHud)
    {
        MountSuoMaoHud(PC);
    }

    if (auto* Character = Cast<ACharacter>(PC->GetPawn()))
    {
        if (auto* Combat = Character->FindComponentByClass<UPlayerCombatComponent>())
        {
            const auto* RoomSub = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<URoomCodeSubsystem>() : nullptr;
            const int32 DesiredHero = RoomSub ? FMath::Clamp(RoomSub->GetSelectedCharacterIndex(), 0, 2) : 0;
            if (Combat->GetCharacterChoice() != DesiredHero)
            {
                Combat->RequestSelectCharacter(DesiredHero);
            }
            InitializedCharacter = Character;
        }
    }

    UpdateSuoMaoHud(DeltaTime);
}

bool UCombatSubsystem::CanUseInput() const
{
    const auto* Forge = GetWorld()->GetSubsystem<UWeaponForgeSubsystem>();
    const auto* Room = GetWorld()->GetGameInstance() ? GetWorld()->GetGameInstance()->GetSubsystem<URoomCodeSubsystem>() : nullptr;
    const auto* Combat = Controller.IsValid() && Controller->GetPawn()
        ? Controller->GetPawn()->FindComponentByClass<UPlayerCombatComponent>() : nullptr;
    return Combat && Combat->GetHealth() > 0
        && (!Forge || !Forge->IsPanelOpen())
        && (!Room || !Room->IsMatchMenuOpen());
}

void UCombatSubsystem::Attack()
{
    if (CanUseInput())
    {
        if (auto* Combat = Controller->GetPawn()->FindComponentByClass<UPlayerCombatComponent>())
        {
            Combat->RequestAttack();
        }
    }
}

void UCombatSubsystem::SelectGun()
{
    if (!CanUseInput()) return;
    if (auto* Character = Cast<ACharacter>(Controller->GetPawn()))
    {
        const FString Cls = Character->GetClass()->GetPathName();
        if (Cls.Contains(TEXT("BP_Miyabi")) || Cls.Contains(TEXT("BP_Yousun")) || Cls.Contains(TEXT("BP_MyCharacter")))
        {
            return; // Native hero already carries their signature weapon.
        }
    }
    if (auto* Forge = GetWorld()->GetSubsystem<UWeaponForgeSubsystem>())
    {
        Forge->EquipInitialGun();
    }
}

void UCombatSubsystem::BlockSinglePlayerKey()
{
    // Intentionally swallows 1/2/3/5 to prevent single-player pawn swapping or AI bot spawning in LAN matches.
}

void UCombatSubsystem::MountSuoMaoHud(APlayerController* PC)
{
    if (!PC) return;
    UClass* HudClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/SuoMao/UI/WBP_HUD.WBP_HUD_C"));
    if (!HudClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("[SuoMaoHUD] Failed to load /Game/SuoMao/UI/WBP_HUD.WBP_HUD_C"));
        return;
    }
    SuoMaoHud = CreateWidget<UUserWidget>(PC, HudClass);
    if (!SuoMaoHud) return;

    SuoMaoHud->SetVisibility(ESlateVisibility::HitTestInvisible);
    SuoMaoHud->AddToViewport(50);

    if (UFunction* OnShowFn = SuoMaoHud->FindFunction(TEXT("OnShow")))
    {
        if (OnShowFn->ParmsSize == 0)
        {
            SuoMaoHud->ProcessEvent(OnShowFn, nullptr);
        }
    }
    SuoMaoHud->SetRenderOpacity(1.0f);
    for (const TCHAR* Group : {TEXT("G_Timer"), TEXT("G_Beacon"), TEXT("G_Score"), TEXT("G_Player"), TEXT("G_Skill"), TEXT("Hint")})
    {
        if (UWidget* W = SuoMaoHud->GetWidgetFromName(Group))
        {
            W->SetVisibility(ESlateVisibility::HitTestInvisible);
            W->SetRenderOpacity(1.0f);
        }
    }
    bHudOverlapAdjusted = false;
    LastHudHeroChoice = -1;
    UE_LOG(LogTemp, Display, TEXT("[SuoMaoHUD] Mounted /Game/SuoMao/UI/WBP_HUD.WBP_HUD_C"));
}

FString UCombatSubsystem::FormatPlayerDisplayName(const APlayerState* PS) const
{
    if (!PS) return TEXT("旅人");
    int32 HeroIdx = -1;
    if (const APawn* Pawn = PS->GetPawn())
    {
        if (const auto* Combat = Pawn->FindComponentByClass<UPlayerCombatComponent>())
        {
            HeroIdx = Combat->GetCharacterChoice();
        }
        if (HeroIdx < 0)
        {
            HeroIdx = InferHeroIndexFromClass(Pawn);
        }
    }
    const TCHAR* Hero = HeroIdx >= 0 ? HeroNameCN(HeroIdx) : TEXT("旅人");
    return FString(Hero);
}

void UCombatSubsystem::UpdateSuoMaoHud(float DeltaTime)
{
    if (!SuoMaoHud || !Controller.IsValid()) return;
    UWorld* World = GetWorld();
    if (!World) return;

    // Move top-left G_Beacon cleanly below UTrioCityHUD's circular minimap (Center 120,120, R=89) so it overlaps neither minimap nor top-center G_Score!
    if (!bHudOverlapAdjusted)
    {
        bHudOverlapAdjusted = true;
        if (UWidget* BeaconGrp = SuoMaoHud->GetWidgetFromName(TEXT("G_Beacon")))
        {
            if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(BeaconGrp->Slot))
            {
                Slot->SetPosition(FVector2D(28.f, 238.f));
                UE_LOG(LogTemp, Display, TEXT("[SuoMaoHUD] Placed G_Beacon cleanly below minimap at (28, 238)"));
            }
        }
    }

    ACharacter* Player = Cast<ACharacter>(Controller->GetPawn());
    const auto* Local = Player ? Player->FindComponentByClass<UPlayerCombatComponent>() : nullptr;
    const auto* Match = ACombatMatchActor::Find(World);

    // 1. Sync hero portrait / name / signature weapon label on WBP_HUD
    int32 HeroIdx = Local && Local->GetCharacterChoice() >= 0 ? Local->GetCharacterChoice()
        : (Player ? InferHeroIndexFromClass(Player) : 0);
    HeroIdx = FMath::Clamp(HeroIdx, 0, 2);
    if (HeroIdx != LastHudHeroChoice)
    {
        LastHudHeroChoice = HeroIdx;
        if (FIntProperty* SelProp = FindFProperty<FIntProperty>(SuoMaoHud->GetClass(), TEXT("Sel")))
        {
            SelProp->SetPropertyValue_InContainer(SuoMaoHud, HeroIdx);
        }
        for (int32 i = 0; i < 4; ++i)
        {
            const bool bShow = (i == HeroIdx);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("PlMe_%d"), i), bShow);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("PlFace_%d"), i), bShow);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("PlName_%d"), i), bShow);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("PlEN_%d"), i), bShow);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SkWp_%d"), i), bShow);
        }
    }

    // 2. Timer
    SetWidgetText(SuoMaoHud, TEXT("TmMode"), TEXT("信标争夺"));
    if (!Match)
    {
        SetWidgetText(SuoMaoHud, TEXT("TmNum"), TEXT("08:00"));
    }
    else
    {
        const ECombatRoundPhase Phase = Match->GetPhase();
        if (Phase == ECombatRoundPhase::Waiting)
        {
            SetWidgetText(SuoMaoHud, TEXT("TmNum"), TEXT("等待就绪"));
            MatchStartedVisualAt = -1.0;
        }
        else if (Phase == ECombatRoundPhase::Countdown)
        {
            const int32 Secs = FMath::Max(1, FMath::CeilToInt(Match->GetSecondsRemaining()));
            SetWidgetText(SuoMaoHud, TEXT("TmNum"), FString::Printf(TEXT("00:%02d"), Secs));
            MatchStartedVisualAt = -1.0;
        }
        else if (Phase == ECombatRoundPhase::Playing)
        {
            if (MatchStartedVisualAt < 0.0) MatchStartedVisualAt = World->GetTimeSeconds();
            const int32 Secs = FMath::Max(0, FMath::CeilToInt(Match->GetSecondsRemaining()));
            SetWidgetText(SuoMaoHud, TEXT("TmNum"), FString::Printf(TEXT("%02d:%02d"), Secs / 60, Secs % 60));
        }
        else
        {
            SetWidgetText(SuoMaoHud, TEXT("TmNum"), TEXT("00:00"));
        }
    }

    // 3. Beacon objective live tracker (G_Beacon: BcT1 / BcT2)
    if (Match && Match->GetHammer())
    {
        const AGoldenHammerActor* Beacon = Match->GetHammer();
        const APlayerState* Holder = Beacon->GetHolderState();
        if (Holder)
        {
            if (Player && Holder == Player->GetPlayerState())
            {
                SetWidgetText(SuoMaoHud, TEXT("BcT1"), TEXT("溯源信标  ·  你正持有"));
                SetWidgetText(SuoMaoHud, TEXT("BcT2"), TEXT("每秒 +1 校准点  ·  击败对手 +3"));
            }
            else
            {
                SetWidgetText(SuoMaoHud, TEXT("BcT1"), FString::Printf(TEXT("溯源信标  ·  %s 持有"), *FormatPlayerDisplayName(Holder)));
                const float DistM = Player ? FVector::Dist(Player->GetActorLocation(), Beacon->GetMapPosition()) / 100.f : 0.f;
                SetWidgetText(SuoMaoHud, TEXT("BcT2"), FString::Printf(TEXT("距离 %.0f m  ·  %s · 击败夺取信标"), DistM, DirectionLabel(Player, Beacon->GetMapPosition())));
            }
        }
        else
        {
            SetWidgetText(SuoMaoHud, TEXT("BcT1"), TEXT("溯源信标  ·  无人持有"));
            const FVector BPos = Beacon->GetGroundPosition();
            const float DistM = Player ? FVector::Dist(Player->GetActorLocation(), BPos) / 100.f : 0.f;
            SetWidgetText(SuoMaoHud, TEXT("BcT2"), FString::Printf(TEXT("距离 %.0f m  ·  %s · 循光柱靠近拾取"), DistM, DirectionLabel(Player, BPos)));
        }
    }
    else
    {
        SetWidgetText(SuoMaoHud, TEXT("BcT1"), TEXT("溯源信标  ·  坐标校准中"));
        SetWidgetText(SuoMaoHud, TEXT("BcT2"), TEXT("准备夺取城市中央能量水晶信标"));
    }

    // 4. Local HP & Spawn Shield bar (G_Player: HpTx / HpFill / EnFill)
    const int32 Hp = Local ? Local->GetHealth() : CombatRules::MaxHealth;
    const float HpRatio = FMath::Clamp(static_cast<float>(Hp) / static_cast<float>(CombatRules::MaxHealth), 0.f, 1.f);
    if (Local && Hp <= 0)
    {
        SetWidgetText(SuoMaoHud, TEXT("HpTx"), FString::Printf(TEXT("重塑中 %.0fs"), FMath::CeilToDouble(Local->GetRespawnRemaining())));
    }
    else
    {
        SetWidgetText(SuoMaoHud, TEXT("HpTx"), FString::Printf(TEXT("%d / %d"), Hp, CombatRules::MaxHealth));
    }
    if (UWidget* HpFill = SuoMaoHud->GetWidgetFromName(TEXT("HpFill")))
    {
        HpFill->SetRenderTransformPivot(FVector2D(0.f, 0.5f));
        HpFill->SetRenderScale(FVector2D(HpRatio, 1.f));
    }
    if (UWidget* EnFill = SuoMaoHud->GetWidgetFromName(TEXT("EnFill")))
    {
        const float ShieldRatio = (Local && Local->IsSpawnProtected())
            ? FMath::Clamp(static_cast<float>(Local->GetSpawnProtectionRemaining() / CombatRules::SpawnProtectionSeconds), 0.f, 1.f)
            : (Hp > 0 ? 1.f : 0.f);
        EnFill->SetRenderTransformPivot(FVector2D(0.f, 0.5f));
        EnFill->SetRenderScale(FVector2D(ShieldRatio, 1.f));
    }

    // 5. 4-Player Scoreboard (G_Score: SbBar_0..3, SbName_0..3, SbPts_0..3, SbFill_0..3, SbYou_0..3)
    TArray<FCombatScoreRow> SortedScores = Match ? Match->GetScores() : TArray<FCombatScoreRow>();
    SortedScores.Sort([](const FCombatScoreRow& A, const FCombatScoreRow& B)
    {
        return A.Score != B.Score ? A.Score > B.Score : A.PlayerId < B.PlayerId;
    });
    const APlayerState* LocalPS = Player ? Player->GetPlayerState() : Controller->PlayerState.Get();
    for (int32 i = 0; i < 4; ++i)
    {
        const bool bValidRow = i < SortedScores.Num() && IsValid(SortedScores[i].Player);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbBar_%d"), i), bValidRow);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbName_%d"), i), bValidRow);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbPts_%d"), i), bValidRow);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbFill_%d"), i), bValidRow);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbFace_%d"), i), bValidRow);
        if (!bValidRow)
        {
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbYou_%d"), i), false);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbYouBg_%d"), i), false);
            SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbYouTx_%d"), i), false);
            continue;
        }
        const FCombatScoreRow& Row = SortedScores[i];
        const bool bIsSelf = (Row.Player == LocalPS);
        SetWidgetText(SuoMaoHud, *FString::Printf(TEXT("SbName_%d"), i), FormatPlayerDisplayName(Row.Player));
        SetWidgetText(SuoMaoHud, *FString::Printf(TEXT("SbPts_%d"), i), FString::Printf(TEXT("%d"), Row.Score));
        if (UImage* FaceImg = Cast<UImage>(SuoMaoHud->GetWidgetFromName(*FString::Printf(TEXT("SbFace_%d"), i))))
        {
            int32 RowHero = 0;
            if (const APawn* RowPawn = Row.Player ? Row.Player->GetPawn() : nullptr)
            {
                const auto* RowCombat = RowPawn->FindComponentByClass<UPlayerCombatComponent>();
                RowHero = (RowCombat && RowCombat->GetCharacterChoice() >= 0) ? RowCombat->GetCharacterChoice() : InferHeroIndexFromClass(RowPawn);
            }
            static const TCHAR* FacePaths[3] = {
                TEXT("/Game/SuoMao/Mat/MI/MI_Face_vio.MI_Face_vio"),
                TEXT("/Game/SuoMao/Mat/MI/MI_Face_laika.MI_Face_laika"),
                TEXT("/Game/SuoMao/Mat/MI/MI_Face_kane.MI_Face_kane")
            };
            if (UMaterialInterface* FaceMat = LoadObject<UMaterialInterface>(nullptr, FacePaths[FMath::Clamp(RowHero, 0, 2)]))
            {
                FaceImg->SetBrushFromMaterial(FaceMat);
            }
        }
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbYou_%d"), i), bIsSelf);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbYouBg_%d"), i), bIsSelf);
        SetWidgetVisible(SuoMaoHud, *FString::Printf(TEXT("SbYouTx_%d"), i), bIsSelf);
        if (UWidget* Fill = SuoMaoHud->GetWidgetFromName(*FString::Printf(TEXT("SbFill_%d"), i)))
        {
            Fill->SetRenderTransformPivot(FVector2D(0.f, 0.5f));
            Fill->SetRenderScale(FVector2D(FMath::Clamp(static_cast<float>(Row.Score) / 100.f, 0.04f, 1.f), 1.f));
        }
    }

    // 6. Tactical Banner (G_Banner: BnCN / BnEN)
    bool bShowBanner = false;
    FString BannerCN;
    FString BannerEN;
    if (Match)
    {
        const ECombatRoundPhase Phase = Match->GetPhase();
        if (Phase == ECombatRoundPhase::Waiting)
        {
            bShowBanner = true;
            BannerCN = TEXT("全员集结中  ·  等待所有玩家加载完成");
            BannerEN = TEXT("STANDBY  ·  SYNCING SQUAD");
        }
        else if (Phase == ECombatRoundPhase::Countdown)
        {
            bShowBanner = true;
            BannerCN = FString::Printf(TEXT("行动即将开始  ·  %d 秒后开启信标争夺"), FMath::Max(1, FMath::CeilToInt(Match->GetSecondsRemaining())));
            BannerEN = TEXT("OPERATION COUNTDOWN");
        }
        else if (Phase == ECombatRoundPhase::Finished)
        {
            bShowBanner = true;
            if (!SortedScores.IsEmpty() && IsValid(SortedScores[0].Player))
            {
                const FCombatScoreRow& Winner = SortedScores[0];
                BannerCN = FString::Printf(TEXT("8 分钟信标争夺结束  ·  第 1 名 %s（%d 分 / %d 击败） ·  按 F1 返回大厅"),
                    *FormatPlayerDisplayName(Winner.Player), Winner.Score, Winner.Kills);
            }
            else
            {
                BannerCN = TEXT("8 分钟信标争夺已结束  ·  按 F1 返回大厅");
            }
            BannerEN = TEXT("OPERATION COMPLETE  ·  SCORES LOCKED");
        }
        else if (Phase == ECombatRoundPhase::Playing)
        {
            const double Elapsed = MatchStartedVisualAt >= 0.0 ? (World->GetTimeSeconds() - MatchStartedVisualAt) : 99.0;
            if (Elapsed < 4.5)
            {
                bShowBanner = true;
                BannerCN = TEXT("行动开始  ·  夺取溯源信标，校准归乡坐标");
                BannerEN = TEXT("OPERATION START  ·  BEACON RUSH");
            }
            else if (Local && Local->GetHealth() <= 0)
            {
                bShowBanner = true;
                BannerCN = FString::Printf(TEXT("已被击败  ·  %.0f 秒后随机道路重塑"), FMath::CeilToDouble(Local->GetRespawnRemaining()));
                BannerEN = TEXT("ELIMINATED  ·  RESPAWNING");
            }
            else if (Local && Local->IsSpawnProtected())
            {
                bShowBanner = true;
                BannerCN = FString::Printf(TEXT("重塑护盾激活  ·  %.0f 秒内免疫伤害"), FMath::CeilToDouble(Local->GetSpawnProtectionRemaining()));
                BannerEN = TEXT("SPAWN PROTECTION ACTIVE");
            }
        }
    }
    SetWidgetVisible(SuoMaoHud, TEXT("G_Banner"), bShowBanner);
    if (bShowBanner)
    {
        SetWidgetText(SuoMaoHud, TEXT("BnCN"), BannerCN);
        SetWidgetText(SuoMaoHud, TEXT("BnEN"), BannerEN);
    }

    // 7. Bottom control hint (clean, authentic controls)
    const FString Feedback = Local ? Local->GetFeedback() : FString();
    const FString HintStr = Feedback.IsEmpty()
        ? TEXT("WASD 移动   ·   左键 攻击   ·   TAB/M 城市地图   ·   F1 房间菜单")
        : FString::Printf(TEXT("%s   |   WASD 移动   ·   左键 攻击   ·   TAB/M 城市地图"), *Feedback);
    SetWidgetText(SuoMaoHud, TEXT("Hint"), HintStr);
}

void UCombatSubsystem::Deinitialize()
{
    if (Controller.IsValid() && CombatInput) Controller->PopInputComponent(CombatInput);
    if (SuoMaoHud)
    {
        SuoMaoHud->RemoveFromParent();
        SuoMaoHud = nullptr;
    }
    Super::Deinitialize();
}
