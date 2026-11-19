#pragma once

#include "Animation/AnimNotifies/AnimNotify.h"
#include "AnimNotify_PlayNiagaraEffect.h"
#include "TrioGunShotNotify.generated.h"

UCLASS(meta=(DisplayName="Player 1 Shot"))
class UTrioGunShotNotify : public UAnimNotify
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category="Shot")
    FName MuzzleSocket;

    UPROPERTY(EditAnywhere, Category="Shot")
    FVector MuzzleOffset = FVector::ZeroVector;

    virtual void Notify(USkeletalMeshComponent* Mesh, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
};

UCLASS(meta=(DisplayName="Player 1 Sweep Shot"))
class UTrioGunSweepNotify : public UAnimNotify_PlayNiagaraEffect
{
    GENERATED_BODY()

public:
    virtual void Notify(USkeletalMeshComponent* Mesh, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
};
