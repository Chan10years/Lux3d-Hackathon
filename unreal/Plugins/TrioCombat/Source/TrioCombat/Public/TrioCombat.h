#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/DataTable.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Subsystems/WorldSubsystem.h"
#include "TrioCombat.generated.h"

class ACharacter;
class ATrioBotController;
class UAnimMontage;
class UTrioCombatHUD;
class UTrioCityHUD;
class UMaterialInstanceDynamic;

USTRUCT(BlueprintType)
struct TRIOCOMBAT_API FTrioCombatantRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double MaxHealth = 100;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double MaxShield = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double ShieldPerSecond = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Attack = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double CritChance = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double CritMultiplier = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double HeadMultiplier = 1;
};

USTRUCT(BlueprintType)
struct TRIOCOMBAT_API FTrioAttackRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly) FName Character;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Multiplier = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double FullChargeMultiplier = 1;
};

USTRUCT(BlueprintType)
struct TRIOCOMBAT_API FTrioVitals
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) double Health = 0;
    UPROPERTY(BlueprintReadOnly) double Shield = 0;

    void RestoreShield(const FTrioCombatantRow& Stats, double Seconds);
};

USTRUCT(BlueprintType)
struct TRIOCOMBAT_API FTrioHitResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) double Damage = 0;
    UPROPERTY(BlueprintReadOnly) double ShieldAbsorbed = 0;
    UPROPERTY(BlueprintReadOnly) double HealthLost = 0;
    UPROPERTY(BlueprintReadOnly) bool bCritical = false;
    UPROPERTY(BlueprintReadOnly) bool bHeadshot = false;
};

UCLASS(ClassGroup=(Combat), meta=(BlueprintSpawnableComponent))
class TRIOCOMBAT_API UTrioCombatReceiver : public UActorComponent
{
    GENERATED_BODY()

public:
    UTrioCombatReceiver();

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Combat") FTrioCombatantRow Stats;

    UFUNCTION(BlueprintPure, Category="Combat") FTrioVitals GetVitals() const;
    FTrioVitals& MutableVitals();
    FTrioHitResult ConsumeDamage(FTrioHitResult Result, const AActor* Attacker = nullptr);
    bool IsDefeated() const { return bDefeated; }
    static constexpr int32 MaxGunSweepShots = 12;
    bool ShowsGunSweepMeter(double Now) const { return bGunSweepActive || Now < GunSweepMeterUntil; }
    void UpdateGunSweep(int32 GunThirdPhase, double Now);
    bool AcceptGunSweepShot(int32 GunThirdPhase, double Now);
    UPROPERTY(Transient, BlueprintReadOnly, Category="Combat") int32 GunSweepShotsFired = 0;
    FName CharacterId;
    double LockedCharge = 0;

protected:
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;

private:
    void BeginDeath(const AActor* Attacker);
    void PrepareDeathMaterials();

    FTrioVitals LocalVitals;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> DeathMaterials;
    float DeathElapsed = 0;
    float FadeStart = -1;
    bool bDefeated = false;
    bool bGunSweepActive = false;
    double GunSweepMeterUntil = 0;

    UFUNCTION()
    void OnEngineDamage(AActor* Actor, float Damage, const UDamageType* Type, AController* Instigator, AActor* Causer);
};

UCLASS()
class TRIOCOMBAT_API UTrioCombatWorld : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
    virtual void OnWorldBeginPlay(UWorld& World) override;
    virtual void Tick(float Seconds) override;
    virtual TStatId GetStatId() const override;
    virtual void Deinitialize() override;

    FName Identify(const AActor* Actor) const;
    UTrioCombatReceiver* Receiver(AActor* Actor);
    FTrioVitals& VitalsFor(AActor* Actor, FName Id, FTrioVitals& Local);
    const FTrioVitals* FindVitals(FName Id) const { return Roster.Find(Id); }
    const AActor* FindRosterActor(FName Id) const
    {
        const TWeakObjectPtr<AActor>* Actor = RosterActors.Find(Id);
        return Actor ? Actor->Get() : nullptr;
    }
    const FTrioCombatantRow* FindStats(FName Id) const;
    const FTrioAttackRow* FindAttack(FName Id) const;
    bool QueueGunShot(AActor* Shooter, FVector MuzzleLocation, bool bSweep = false);
    void BeginBladeStep(ACharacter* Character, UAnimMontage* Montage);
    bool AreBotsEnabled() const { return bBotsEnabled; }
    ACharacter* FindBotTarget(const ACharacter& Character, const ACharacter* Excluded, bool bSearching) const;
    void ShowDamage(AActor* Attacker, AActor* Target, FVector Location, const FTrioHitResult& Result);

private:
    UPROPERTY() TObjectPtr<UDataTable> Combatants;
    UPROPERTY() TObjectPtr<UDataTable> Attacks;
    UPROPERTY() TObjectPtr<UTrioCombatHUD> HUD;
    UPROPERTY() TObjectPtr<UTrioCityHUD> CityHUD;
    TMap<FName, FTrioVitals> Roster;
    TMap<FName, TWeakObjectPtr<AActor>> RosterActors;
    TArray<TWeakObjectPtr<UTrioCombatReceiver>> Receivers;
    struct FGunShot
    {
        TWeakObjectPtr<AActor> Shooter;
        FVector MuzzleLocation;
        bool bEndsSweep = false;
        bool bBotShot = false;
    };
    TArray<FGunShot> PendingGunShots;
    struct FBladeStep
    {
        TWeakObjectPtr<UAnimMontage> Montage;
        TWeakObjectPtr<AActor> Target;
        FVector Start;
        FVector Destination;
        FQuat StartRotation;
        FQuat Facing;
        double StartTime;
    };
    TMap<TWeakObjectPtr<ACharacter>, FBladeStep> BladeSteps;
    void UpdateBladeSteps();
    void UpdateBots(APlayerController& Player);
    void StopBots();
    TArray<TWeakObjectPtr<ATrioBotController>> Bots;
    bool bBotsEnabled = false;
    bool bWasBotKeyDown = false;
    FDelegateHandle SpawnHandle;
    FDelegateHandle PostActorTickHandle;

    void FlushGunShots(UWorld* World, ELevelTick TickType, float DeltaSeconds);

    void OnSpawn(AActor* Actor);
    void SelectSurvivingPawn(APlayerController& Player);
};

UCLASS()
class TRIOCOMBAT_API UTrioCombatLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Combat")
    static FTrioHitResult ResolveHit(AActor* Attacker, FName Attack, AActor* Target, FVector Location,
        bool bHeadshot = false, double Charge = 0);

    UFUNCTION(BlueprintCallable, Category="Combat")
    static void ResolveGunHit(AActor* DamageCauser, const FHitResult& HitInfo, bool bSweep);

    UFUNCTION(BlueprintCallable, Category="Combat")
    static void ResolveSniperHit(AActor* DamageCauser, const FHitResult& HitInfo);

    UFUNCTION(BlueprintCallable, Category="Combat")
    static void LockSniperCharge(AActor* Attacker);

    UFUNCTION(BlueprintCallable, Category="Combat")
    static void ResolveBladeHit(AActor* DamageCauser, const FHitResult& HitInfo, FName Attack);

    UFUNCTION(BlueprintCallable, Category="Combat")
    static void StartBladeAfterimage(AActor* Attacker);

    UFUNCTION(BlueprintCallable, Category="Combat")
    static void ResolveBladePulse(AActor* Attacker, const FHitResult& Contact, FVector Center, FName Attack, float Radius);

    static FTrioHitResult Calculate(const FTrioCombatantRow& Stats, const FTrioAttackRow& Attack,
        bool bHeadshot, double Charge, double CritRoll);
    static double ScopeCharge(const AActor* Actor, bool bUseLocked);
};

