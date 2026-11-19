#include "TrioGroundCamera.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"

namespace
{
    constexpr double MinOrbitPitch = -72.0;
    constexpr double MaxOrbitPitch = 43.0;
    constexpr float CameraClearance = 28.0f;

    const USpringArmComponent* FindActiveOrbitArm(AActor* ViewTarget)
    {
        if (!Cast<ACharacter>(ViewTarget))
            return nullptr;

        TInlineComponentArray<UCameraComponent*> Cameras;
        ViewTarget->GetComponents(Cameras);
        for (const UCameraComponent* Camera : Cameras)
        {
            if (!Camera->IsActive())
                continue;
            for (const USceneComponent* Parent = Camera->GetAttachParent(); Parent; Parent = Parent->GetAttachParent())
                if (const auto* Arm = Cast<USpringArmComponent>(Parent))
                    return Arm;
            // A directly mounted camera (for example Yousun's scope) has no orbit arm.
            return nullptr;
        }
        return nullptr;
    }
}

bool UTrioGroundCamera::ProcessViewRotation(AActor* ViewTarget, float DeltaTime, FRotator& OutViewRotation,
    FRotator& OutDeltaRotation)
{
    if (FindActiveOrbitArm(ViewTarget))
    {
        const double Pitch = FRotator::NormalizeAxis(OutViewRotation.Pitch + OutDeltaRotation.Pitch);
        OutViewRotation.Pitch = FMath::Clamp(Pitch, MinOrbitPitch, MaxOrbitPitch);
        OutDeltaRotation.Pitch = 0.0;
    }
    return Super::ProcessViewRotation(ViewTarget, DeltaTime, OutViewRotation, OutDeltaRotation);
}

bool UTrioGroundCamera::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
    AActor* ViewTarget = GetViewTarget();
    const auto* Arm = FindActiveOrbitArm(ViewTarget);
    if (!Arm || !Arm->bDoCollisionTest)
        return false;

    const FVector Pivot = Arm->GetComponentLocation() + Arm->TargetOffset;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioOrbitCamera), false, ViewTarget);
    UWorld* World = ViewTarget->GetWorld();
    FHitResult Obstacle;

    // The camera itself is offset past the arm's socket, so sweep to the final view location.
    if (World->SweepSingleByChannel(Obstacle, Pivot, InOutPOV.Location, FQuat::Identity,
        Arm->ProbeChannel, FCollisionShape::MakeSphere(CameraClearance), Query))
    {
        InOutPOV.Location = Obstacle.Location;
    }

    if (InOutPOV.Location.Z < Pivot.Z)
    {
        // Some floors ignore the camera channel. Keep vertical clearance above walkable surfaces there too.
        const FVector Start(InOutPOV.Location.X, InOutPOV.Location.Y, Pivot.Z + CameraClearance);
        const FVector End = InOutPOV.Location - FVector(0, 0, CameraClearance);
        const FCollisionObjectQueryParams Surfaces(ECC_TO_BITFIELD(ECC_WorldStatic) | ECC_TO_BITFIELD(ECC_WorldDynamic));
        FHitResult Ground;
        if (World->LineTraceSingleByObjectType(Ground, Start, End, Surfaces, Query) &&
            CastChecked<ACharacter>(ViewTarget)->GetCharacterMovement()->IsWalkable(Ground))
        {
            const double SafeHeight = Ground.ImpactPoint.Z + CameraClearance;
            InOutPOV.Location.Z = FMath::Max(InOutPOV.Location.Z, SafeHeight);
        }
    }
    return false;
}
