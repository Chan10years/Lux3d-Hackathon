#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CombatMatchActor.generated.h"

class APlayerState;
class AGoldenHammerActor;
class APlayerController;

UENUM()
enum class ECombatRoundPhase : uint8 { Waiting, Countdown, Playing, Finished };

USTRUCT()
struct FCombatScoreRow
{
    GENERATED_BODY()
    UPROPERTY() TObjectPtr<APlayerState> Player;
    UPROPERTY() int32 Score = 0;
    UPROPERTY() int32 Kills = 0;
    UPROPERTY() int32 PlayerId = INDEX_NONE;
    UPROPERTY() int32 Rank = 0;
};

/** One authority-owned scoreboard per match; PlayerState survives same-Pawn respawns. */
UCLASS()
class TRY_GLB_PUT_INTO_API ACombatMatchActor : public AActor
{
    GENERATED_BODY()
public:
    ACombatMatchActor();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    static ACombatMatchActor* Find(UWorld* World);
    void NotifyElimination(APlayerState* Killer, APlayerState* Victim);
    void ChangeHolder(APlayerState* NewHolder);
    bool IsMatchActive() const;
    ECombatRoundPhase GetPhase() const { return Phase; }
    double GetSecondsRemaining() const;
    int32 GetRoundNumber() const { return RoundNumber; }
    void RestartRound(APlayerController* Requester);
    AGoldenHammerActor* GetHammer() const { return Hammer; }
    const TArray<FCombatScoreRow>& GetScores() const { return Scores; }

private:
    FCombatScoreRow& EnsurePlayer(APlayerState* Player);
    void AwardHoldPoints();
    bool ArePlayersReady() const;
    void PrepareRound();
    void BeginRound();
    void FinishRound();
    void SetPlayersActive(bool Active);
    double GetServerTime() const;
    UPROPERTY(Replicated) ECombatRoundPhase Phase = ECombatRoundPhase::Waiting;
    UPROPERTY(Replicated) double PhaseEndsAt = 0;
    UPROPERTY(Replicated) int32 RoundNumber = 1;
    UPROPERTY(Replicated) TArray<FCombatScoreRow> Scores;
    UPROPERTY(Replicated) TObjectPtr<AGoldenHammerActor> Hammer;
    UPROPERTY() TObjectPtr<APlayerState> ScoringHolder;
    double HoldStarted = 0;
    int32 AwardedHoldSeconds = 0;
    bool bPrepared = false;
};
