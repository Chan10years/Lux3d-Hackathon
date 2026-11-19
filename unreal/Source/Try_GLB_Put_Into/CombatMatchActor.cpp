#include "CombatMatchActor.h"
#include "CombatRules.h"
#include "GoldenHammerActor.h"
#include "PlayerCombatComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"

ACombatMatchActor::ACombatMatchActor()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    NetUpdateFrequency = 10;
    PrimaryActorTick.bCanEverTick = true;
}

ACombatMatchActor* ACombatMatchActor::Find(UWorld* World)
{
    if (World) for (TActorIterator<ACombatMatchActor> It(World); It; ++It) return *It;
    return nullptr;
}

void ACombatMatchActor::BeginPlay()
{
    Super::BeginPlay();
    if (!HasAuthority()) return;
    FVector Centre = FVector::ZeroVector;
    int32 Count = 0;
    for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
        if (It->IsPlayerControlled()) { Centre += It->GetActorLocation(); ++Count; }
    if (Count) Centre /= Count;
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Hammer = GetWorld()->SpawnActor<AGoldenHammerActor>(AGoldenHammerActor::StaticClass(), Centre, FRotator::ZeroRotator, Params);
    FVector ObjectiveSpawn = Centre + FVector(500, 0, 0);
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        if (It->ActorHasTag(TEXT("GoldenHammerSpawn"))) { ObjectiveSpawn = It->GetActorLocation(); break; }
    if (Hammer) Hammer->InitializeSpawn(ObjectiveSpawn);
}

void ACombatMatchActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ACombatMatchActor, Phase);
    DOREPLIFETIME(ACombatMatchActor, PhaseEndsAt);
    DOREPLIFETIME(ACombatMatchActor, RoundNumber);
    DOREPLIFETIME(ACombatMatchActor, Scores);
    DOREPLIFETIME(ACombatMatchActor, Hammer);
}

FCombatScoreRow& ACombatMatchActor::EnsurePlayer(APlayerState* Player)
{
    for (FCombatScoreRow& Row : Scores) if (Row.Player == Player) return Row;
    FCombatScoreRow Row;
    Row.Player = Player;
    Row.PlayerId = Player->GetPlayerId();
    const int32 Index = Scores.Add(Row);
    ForceNetUpdate();
    return Scores[Index];
}

void ACombatMatchActor::AwardHoldPoints()
{
    if (!HasAuthority() || !IsValid(ScoringHolder)) return;
    // Clamp at the deadline even if this frame arrives late. Credit the last complete second once.
    const int32 Points = CombatRules::HoldPointsDue(Phase == ECombatRoundPhase::Playing,
        FMath::Min(GetServerTime(), PhaseEndsAt), HoldStarted, AwardedHoldSeconds);
    if (!Points) return;
    EnsurePlayer(ScoringHolder).Score += Points;
    AwardedHoldSeconds += Points;
    ForceNetUpdate();
}

void ACombatMatchActor::ChangeHolder(APlayerState* NewHolder)
{
    if (!HasAuthority() || ScoringHolder == NewHolder) return;
    AwardHoldPoints(); // Credit completed seconds up to the exact drop/death; discard the fraction.
    ScoringHolder = NewHolder;
    HoldStarted = GetServerTime();
    AwardedHoldSeconds = 0;
    if (IsValid(NewHolder)) EnsurePlayer(NewHolder);
}

void ACombatMatchActor::NotifyElimination(APlayerState* Killer, APlayerState* Victim)
{
    if (!HasAuthority() || !IsMatchActive() || !IsValid(Victim)) return;
    if (Hammer && Hammer->GetHolderState() == Victim) Hammer->DropFromHolder();
    const int32 Points = CombatRules::KillPoints(IsMatchActive(), IsValid(Killer), Killer == Victim);
    if (Points)
    {
        FCombatScoreRow& Row = EnsurePlayer(Killer);
        Row.Score += Points;
        ++Row.Kills;
        ForceNetUpdate();
        UE_LOG(LogTemp, Display, TEXT("[Beacon] KILL killer=%s victim=%s score=%d"), *Killer->GetName(), *Victim->GetName(), Row.Score);
    }
}

void ACombatMatchActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    if (!HasAuthority()) return;
    if (Phase != ECombatRoundPhase::Finished)
        if (const auto* State = GetWorld()->GetGameState())
        for (APlayerState* Player : State->PlayerArray) if (IsValid(Player) && !Player->IsOnlyASpectator()) EnsurePlayer(Player);
    if (Phase == ECombatRoundPhase::Waiting && ArePlayersReady())
    {
        if (!bPrepared) PrepareRound();
        Phase = ECombatRoundPhase::Countdown;
        PhaseEndsAt = GetServerTime() + CombatRules::CountdownSeconds;
        ForceNetUpdate();
        UE_LOG(LogTemp, Display, TEXT("[Match] COUNTDOWN round=%d"), RoundNumber);
    }
    else if (Phase == ECombatRoundPhase::Countdown)
    {
        if (!ArePlayersReady()) { Phase = ECombatRoundPhase::Waiting; PhaseEndsAt = 0; ForceNetUpdate(); }
        else if (GetServerTime() >= PhaseEndsAt) BeginRound();
    }
    else if (Phase == ECombatRoundPhase::Playing)
    {
        AwardHoldPoints();
        if (GetServerTime() >= PhaseEndsAt) FinishRound();
    }
}

double ACombatMatchActor::GetServerTime() const
{
    const auto* State = GetWorld()->GetGameState();
    return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

bool ACombatMatchActor::IsMatchActive() const
{
    return CombatRules::RoundActive(Phase == ECombatRoundPhase::Playing, GetServerTime(), PhaseEndsAt);
}

double ACombatMatchActor::GetSecondsRemaining() const
{
    return Phase == ECombatRoundPhase::Countdown || Phase == ECombatRoundPhase::Playing
        ? FMath::Max(0.0, PhaseEndsAt - GetServerTime()) : 0.0;
}

bool ACombatMatchActor::ArePlayersReady() const
{
    int32 Participants = 0, Ready = 0;
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        auto* PC = It->Get();
        if (!PC || !PC->PlayerState || PC->PlayerState->IsOnlyASpectator()) continue;
        ++Participants;
        auto* Character = Cast<ACharacter>(PC->GetPawn());
        const auto* Combat = Character ? Character->FindComponentByClass<UPlayerCombatComponent>() : nullptr;
        if (Combat && Combat->IsReadyForRound()
            && (PC->IsLocalController() || PC->HasClientLoadedCurrentWorld())) ++Ready;
    }
    if (GetWorld()->GetNetMode() == NM_Standalone
        && FString(GetWorld()->URL.GetOption(TEXT("NetPhase="), TEXT(""))) != TEXT("Match")
        && GetWorld()->GetMapName().EndsWith(TEXT("Lvl_ZZZ")))
    {
        return Participants >= 1 && Ready == Participants;
    }
    return CombatRules::PlayersReady(Participants, Ready);
}

void ACombatMatchActor::SetPlayersActive(bool Active)
{
    for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
        if (auto* Combat = It->FindComponentByClass<UPlayerCombatComponent>()) Combat->SetCombatActive(Active);
}

void ACombatMatchActor::PrepareRound()
{
    // Never let drop/reset callbacks carry points or a holder into the new round.
    Phase = ECombatRoundPhase::Waiting;
    ScoringHolder = nullptr;
    AwardedHoldSeconds = 0;
    HoldStarted = 0;
    Scores.Reset();
    if (const auto* State = GetWorld()->GetGameState())
        for (APlayerState* Player : State->PlayerArray) if (IsValid(Player) && !Player->IsOnlyASpectator()) EnsurePlayer(Player);
    if (Hammer) Hammer->ResetToSpawn();
    for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
        if (auto* Combat = It->FindComponentByClass<UPlayerCombatComponent>()) Combat->ResetForRound();
    bPrepared = true;
    ForceNetUpdate();
}

void ACombatMatchActor::BeginRound()
{
    Phase = ECombatRoundPhase::Playing;
    PhaseEndsAt = GetServerTime() + CombatRules::RoundSeconds;
    SetPlayersActive(true);
    ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("[Match] START round=%d end=%.2f"), RoundNumber, PhaseEndsAt);
}

void ACombatMatchActor::FinishRound()
{
    AwardHoldPoints();
    Phase = ECombatRoundPhase::Finished;
    SetPlayersActive(false);
    // Freeze and order the authoritative snapshot; equal scores share a rank.
    Scores.Sort([](const FCombatScoreRow& A, const FCombatScoreRow& B)
    {
        return A.Score != B.Score ? A.Score > B.Score : A.PlayerId < B.PlayerId;
    });
    TArray<int32> Values;
    for (const FCombatScoreRow& Row : Scores) Values.Add(Row.Score);
    for (FCombatScoreRow& Row : Scores) Row.Rank = CombatRules::RankForScore(Row.Score, Values.GetData(), Values.Num());
    ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("[Match] FINISH round=%d rows=%d"), RoundNumber, Scores.Num());
}

void ACombatMatchActor::RestartRound(APlayerController* Requester)
{
    if (!CombatRules::CanRestart(Phase == ECombatRoundPhase::Finished, HasAuthority(),
        Requester && Requester->GetWorld() == GetWorld() && Requester->IsLocalController())) return;
    ++RoundNumber;
    PhaseEndsAt = 0;
    bPrepared = false;
    PrepareRound();
    UE_LOG(LogTemp, Display, TEXT("[Match] RESTART round=%d"), RoundNumber);
}
