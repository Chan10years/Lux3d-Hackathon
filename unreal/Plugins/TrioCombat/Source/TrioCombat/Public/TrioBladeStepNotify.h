#pragma once

#include "Animation/AnimNotifies/AnimNotify.h"
#include "TrioBladeStepNotify.generated.h"

UCLASS(meta=(DisplayName="Miyabi Attack Approach"))
class UTrioBladeStepNotify : public UAnimNotify
{
    GENERATED_BODY()

public:
    virtual void Notify(USkeletalMeshComponent* Mesh, UAnimSequenceBase* Animation,
        const FAnimNotifyEventReference& EventReference) override;
};
