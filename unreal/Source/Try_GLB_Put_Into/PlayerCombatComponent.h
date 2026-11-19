#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/NetSerialization.h"
#include "PlayerCombatComponent.generated.h"

class APlayerState;
class UAnimMontage;

UENUM()
enum class ECombatWeaponMode : uint8 { Melee, Rifle };

USTRUCT()
struct FHeroAnimSyncState
{
    GENERATED_BODY()

    UPROPERTY() FVector_NetQuantize100 WorldInput = FVector::ZeroVector;
    UPROPERTY() FVector_NetQuantize10 Velocity = FVector::ZeroVector;
    UPROPERTY() float InputVelocityDir = 0.f;
    UPROPERTY() float ActorYaw = 0.f;
    UPROPERTY() float ControlYaw = 0.f;
    UPROPERTY() float YS_CrouchDepth = 0.f;
    UPROPERTY() float YS_DashDirection = 0.f;
    UPROPERTY() float YS_DepthValue = 0.f;
    UPROPERTY() TObjectPtr<UClass> MeshAnimClass = nullptr;
    UPROPERTY() uint16 Flags = 0;
    UPROPERTY() uint8 PureState = 0;
    UPROPERTY() uint8 SC_Stage = 0;
    UPROPERTY() uint8 GunThirdPhase = 0;
    UPROPERTY() uint8 YS_CombatPhase = 0;
    UPROPERTY() bool bValid = false;
};

/** Authority-owned combat and same-Character death/respawn state. */
UCLASS(ClassGroup=(Custom))
class TRY_GLB_PUT_INTO_API UPlayerCombatComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UPlayerCombatComponent();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
    void RequestAttack();
    void RequestSelectCharacter(int32 ChoiceIndex);
    void RefreshWeaponMode();
    void SetCombatActive(bool Active);
    void ResetForRound();
    void RequestRestartRound();
    bool IsReadyForRound() const { return Health > 0 && !bRoundResetPending; }
    int32 GetHealth() const { return Health; }
    int32 GetCharacterChoice() const { return CharacterChoice; }
    double GetRespawnRemaining() const;
    double GetSpawnProtectionRemaining() const;
    bool IsSpawnProtected() const;
    bool IsCombatActive() const;
    ECombatWeaponMode GetMode() const { return Mode; }
    FString GetFeedback() const;
    void GetAttackPose(float& Pitch, float& Yaw, float& Extension) const;

private:
    UPROPERTY(ReplicatedUsing=OnRep_Health) int32 Health = 100;
    UPROPERTY(Replicated) int32 CharacterChoice = -1;
    UPROPERTY(Replicated) double RespawnAt = 0;
    UPROPERTY(Replicated) double SpawnProtectedUntil = 0;
    UPROPERTY(Replicated) bool bCombatActive = false;
    UPROPERTY(Replicated) ECombatWeaponMode Mode = ECombatWeaponMode::Melee;
    UPROPERTY(ReplicatedUsing=OnRep_SyncedAnimState) FHeroAnimSyncState SyncedAnimState;
    UFUNCTION() void OnRep_Health();
    UFUNCTION() void OnRep_SyncedAnimState();
    UFUNCTION(Server, Reliable) void ServerRequestAttack(FVector_NetQuantizeNormal Direction);
    UFUNCTION(Server, Reliable) void ServerSelectCharacter(int32 ChoiceIndex);
    UFUNCTION(Server, Reliable) void ServerRequestRestartRound();
    UFUNCTION(Server, Unreliable) void ServerSyncAnimState(const FHeroAnimSyncState& InState);
    UFUNCTION(Server, Reliable) void ServerSyncMontage(uint8 Channel, uint16 SeqId, UObject* AnimAsset,
        FName SlotName, FName SectionName, float Position, float PlayRate, bool bStop);
    UFUNCTION(NetMulticast, Reliable) void MulticastSyncMontage(uint8 Channel, uint16 SeqId, UObject* AnimAsset,
        FName SlotName, FName SectionName, float Position, float PlayRate, bool bStop);
    UFUNCTION(Client, Reliable) void ClientSnapToRespawn(FVector_NetQuantize Location, FRotator Rotation);
    UFUNCTION(NetMulticast, Unreliable) void MulticastAttackStarted(ECombatWeaponMode AttackMode);
    UFUNCTION(NetMulticast, Unreliable) void MulticastAttackResult(FVector_NetQuantize Start,
        FVector_NetQuantize End, AActor* Victim, bool bHit, ECombatWeaponMode AttackMode);
    void ResolveAttack(FVector Direction, ECombatWeaponMode AttackMode);
    void ApplyHit(UPlayerCombatComponent* Attacker, const FVector& Direction);
    bool HasValidGun() const;
    void Die(APlayerState* Killer = nullptr);
    bool Respawn();
    void UpdateLifePresentation();
    void UpdateAnimationSync(float DeltaTime);
    void ApplySyncedAnimState(float DeltaTime);
    void PollLocalMontageChannel(uint8 Channel, class USkeletalMeshComponent* MeshComp);
    void ApplyRemoteMontage(uint8 Channel, uint16 SeqId, UObject* AnimAsset, FName SlotName,
        FName SectionName, float Position, float PlayRate, bool bStop);
    bool LoadRespawnOrigins();
    bool FindRespawnTransform(FTransform& OutTransform);
    double GetServerTime() const;

    struct FLocalMontageTrack
    {
        bool bActive = false;
        int32 InstanceId = INDEX_NONE;
        TWeakObjectPtr<UObject> AnimAsset;
        FName SlotName = NAME_None;
        FName SectionName = NAME_None;
        float LastPosition = 0.f;
        float LastPlayRate = 1.f;
        uint16 SeqId = 0;
    };

    FLocalMontageTrack LocalMontages[2];
    TWeakObjectPtr<UAnimMontage> RemoteSyncedMontages[2];
    uint16 LastAppliedMontageSeq[2] = {0, 0};
    FHeroAnimSyncState LastSentAnimState;
    double LastAnimSendTime = -100.0;
    FVector PrevLocalLocation = FVector::ZeroVector;
    bool bHasPrevLocalLocation = false;
    bool bAnimPrereqConfigured = false;
    double SuppressClientAuthPosUntil = 0.0;

    FTimerHandle StrikeTimer;
    double LastServerAttack = -100;
    double LastVisualAttack = -100;
    ECombatWeaponMode VisualAttackMode = ECombatWeaponMode::Melee;
    FString Feedback;
    double FeedbackUntil = 0;
    int32 PreviousHealth = 100;
    bool bAppliedDead = false;
    bool bHasSpawnOrigin = false;
    bool bRoundResetPending = false;
    FTransform SpawnOrigin;
    TArray<FTransform> RespawnOrigins;
    bool bRespawnOriginsLoaded = false;
    ECollisionEnabled::Type LivingCollision = ECollisionEnabled::QueryAndPhysics;
    ECollisionEnabled::Type LivingMeshCollision = ECollisionEnabled::QueryOnly;
};
