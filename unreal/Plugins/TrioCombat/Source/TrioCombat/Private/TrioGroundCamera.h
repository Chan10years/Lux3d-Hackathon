#pragma once

#include "Camera/CameraModifier.h"
#include "TrioGroundCamera.generated.h"

UCLASS()
class UTrioGroundCamera : public UCameraModifier
{
    GENERATED_BODY()

public:
    virtual bool ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV) override;
    virtual bool ProcessViewRotation(AActor* ViewTarget, float DeltaTime, FRotator& OutViewRotation,
        FRotator& OutDeltaRotation) override;
};
