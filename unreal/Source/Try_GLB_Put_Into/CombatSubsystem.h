#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CombatSubsystem.generated.h"

class APlayerController;
class ACharacter;
class APlayerState;
class UInputComponent;
class UUserWidget;

/** Local input, hero spawn sync, SuoMao WBP_HUD driver, and authority component bootstrap. */
UCLASS()
class TRY_GLB_PUT_INTO_API UCombatSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual void Deinitialize() override;

private:
    bool CanUseInput() const;
    void Attack();
    void SelectGun();
    void BlockSinglePlayerKey();
    void MountSuoMaoHud(APlayerController* PC);
    void UpdateSuoMaoHud(float DeltaTime);
    FString FormatPlayerDisplayName(const APlayerState* PS) const;

    TWeakObjectPtr<APlayerController> Controller;
    TWeakObjectPtr<ACharacter> InitializedCharacter;
    UPROPERTY(Transient) TObjectPtr<UInputComponent> CombatInput;
    UPROPERTY(Transient) TObjectPtr<UUserWidget> SuoMaoHud;
    int32 LastHudHeroChoice = -1;
    double MatchStartedVisualAt = -1.0;
    bool bHudOverlapAdjusted = false;
};
