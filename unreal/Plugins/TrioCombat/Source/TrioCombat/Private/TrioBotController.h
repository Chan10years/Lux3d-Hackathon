#pragma once

#include "AIController.h"
#include "Animation/AnimEnums.h"
#include "TrioBotController.generated.h"

class UAnimInstance;
class USkeletalMeshComponent;

UCLASS()
class ATrioBotController : public AAIController
{
    GENERATED_BODY()

public:
    ATrioBotController();
    void FireGunShot(FVector Muzzle, bool bSweep);
    UFUNCTION(BlueprintPure, Category="Combat AI")
    ACharacter* GetCombatTarget() const { return Target.Get(); }

protected:
    virtual void Tick(float DeltaSeconds) override;
    virtual void OnPossess(APawn* NewPawn) override;
    virtual void OnUnPossess() override;

private:
    void MoveInCombat(ACharacter& Fighter, FVector ToTarget, double Distance, float DeltaSeconds, bool bSearching);
    void UpdateGuns(ACharacter& Fighter, double Now, double Distance, bool bVisible);
    void UpdateBlade(ACharacter& Fighter, double Now, double Distance, bool bVisible);
    void UpdateSniper(ACharacter& Fighter, double Now, double Distance, bool bVisible);
    void PlaySniperAnimations(ACharacter& Fighter, bool bFiring);
    void FireSniper(ACharacter& Fighter, const ACharacter& AimTarget, double Charge);

    TWeakObjectPtr<ACharacter> Target;
    TWeakObjectPtr<UAnimInstance> ManagedAnim;
    TEnumAsByte<ERootMotionMode::Type> PreviousRootMotion = ERootMotionMode::RootMotionFromEverything;
    FName Role;
    FVector Steer = FVector::ZeroVector;
    FVector SearchPoint = FVector::ZeroVector;
    double LostSightAt = -1;
    double NextTargetCheck = 0;
    double NextAction = 0;
    double NextStrafe = 0;
    double ChargeStart = -1;
    double FireAt = 0;
    double NextShot = 0;
    int32 StrafeSide = 1;
    bool bRequestedSweep = false;
};

