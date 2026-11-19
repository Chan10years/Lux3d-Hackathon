#include "TrioBladeStepNotify.h"
#include "TrioCombat.h"
#include "TrioBotController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "SceneView.h"

namespace
{
    constexpr double BladeStepRange = 700.0;
    constexpr double BladeStepDuration = 0.12;

    bool IsVisibleInView(UWorld& World, const ACharacter& Character, const ACharacter& Target,
        const FVector& Eye, const FSceneViewProjectionData& Projection)
    {
        const auto* Capsule = Target.GetCapsuleComponent();
        const FVector Center = Capsule->GetComponentLocation();
        const FVector UpperBody = Center + FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight() * 0.55);
        const FMatrix ViewProjection = Projection.ComputeViewProjectionMatrix();
        const FIntRect ViewRect = Projection.GetConstrainedViewRect();
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioBladeView), false, &Character);

        for (const FVector& Point : {UpperBody, Center})
        {
            FVector2D Screen;
            if (!FSceneView::ProjectWorldToScreen(Point, ViewRect, ViewProjection, Screen) ||
                Screen.X < ViewRect.Min.X || Screen.X >= ViewRect.Max.X ||
                Screen.Y < ViewRect.Min.Y || Screen.Y >= ViewRect.Max.Y)
                continue;
            FHitResult Hit;
            if (!World.LineTraceSingleByChannel(Hit, Eye, Point, ECC_Visibility, Query) || Hit.GetActor() == &Target)
                return true;
        }
        return false;
    }

    bool FindStepDestination(ACharacter& Character, const ACharacter& Target, const FVector& Facing,
        FVector& Destination)
    {
        const FVector Start = Character.GetActorLocation();
        const FVector Center = Target.GetActorLocation();
        const auto* Capsule = Character.GetCapsuleComponent();
        const auto* TargetCapsule = Target.GetCapsuleComponent();
        const auto* Movement = Character.GetCharacterMovement();
        const double Spacing = FMath::Max(95.0, Capsule->GetScaledCapsuleRadius() +
            TargetCapsule->GetScaledCapsuleRadius() + 8.0);
        Destination = Start;
        if (FVector::DistSquared2D(Start, Center) <= FMath::Square(Spacing))
            return true;

        Destination = Center - Facing * Spacing;
        Destination.Z += Capsule->GetScaledCapsuleHalfHeight() - TargetCapsule->GetScaledCapsuleHalfHeight() + 12.0;
        FFindFloorResult Floor;
        Movement->FindFloor(Destination, Floor, false);
        if (!Floor.IsWalkableFloor())
            return false;
        Destination.Z -= Floor.GetDistanceToFloor() - 2.15;
        if (FMath::Abs(Destination.Z - Start.Z) > Movement->MaxStepHeight)
            return false;

        FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioBladeApproach), false, &Character);
        FCollisionResponseParams Responses;
        Capsule->InitSweepCollisionParams(Query, Responses);
        return !Character.GetWorld()->SweepTestByChannel(Start, Destination, Capsule->GetComponentQuat(),
            Capsule->GetCollisionObjectType(), Capsule->GetCollisionShape(), Query, Responses);
    }
}

void UTrioBladeStepNotify::Notify(USkeletalMeshComponent* Mesh, UAnimSequenceBase* Animation,
    const FAnimNotifyEventReference&)
{
    auto* Character = Mesh ? Cast<ACharacter>(Mesh->GetOwner()) : nullptr;
    auto* Montage = Cast<UAnimMontage>(Animation);
    if (Character && Montage && Mesh == Character->GetMesh())
        if (auto* Combat = Character->GetWorld()->GetSubsystem<UTrioCombatWorld>())
            Combat->BeginBladeStep(Character, Montage);
}

void UTrioCombatWorld::BeginBladeStep(ACharacter* Character, UAnimMontage* Montage)
{
    if (const auto* Bot = Character->GetController<ATrioBotController>())
    {
        BladeSteps.Remove(Character);
        ACharacter* Target = Bot->GetCombatTarget();
        if (!IsValid(Target) || !Character->GetCharacterMovement()->IsMovingOnGround())
            return;

        const FVector Start = Character->GetActorLocation();
        const FVector ToTarget = Target->GetActorLocation() - Start;
        const FVector Facing = ToTarget.GetSafeNormal2D();
        FVector Destination;
        if (ToTarget.SizeSquared() <= FMath::Square(BladeStepRange) && !Facing.IsNearlyZero() &&
            FindStepDestination(*Character, *Target, Facing, Destination))
        {
            BladeSteps.Add(Character, {Montage, Target, Start, Destination, Character->GetActorQuat(),
                Facing.ToOrientationQuat(), GetWorld()->GetTimeSeconds()});
        }
        return;
    }

    auto* Player = Cast<APlayerController>(Character->GetController());
    const auto* LocalPlayer = Player ? Player->GetLocalPlayer() : nullptr;
    const auto* Fighter = Character->FindComponentByClass<UTrioCombatReceiver>();
    if (!LocalPlayer || !LocalPlayer->ViewportClient || !LocalPlayer->ViewportClient->Viewport ||
        !Fighter || Fighter->CharacterId != TEXT("Miyabi") || Fighter->IsDefeated() ||
        !Character->GetCharacterMovement()->IsMovingOnGround())
        return;

    BladeSteps.Remove(Character);
    FSceneViewProjectionData Projection;
    if (!LocalPlayer->GetProjectionData(LocalPlayer->ViewportClient->Viewport, Projection))
        return;

    FVector Eye;
    FRotator ViewRotation;
    Player->GetPlayerViewPoint(Eye, ViewRotation);
    const FVector ViewDirection = ViewRotation.Vector();
    const FVector Start = Character->GetActorLocation();
    double BestAlignment = -1.0;
    double BestDistance = TNumericLimits<double>::Max();
    for (const auto& Weak : Receivers)
    {
        const auto* Candidate = Weak.Get();
        auto* Target = Candidate ? Cast<ACharacter>(Candidate->GetOwner()) : nullptr;
        if (!IsValid(Target) || Target == Character || Target->IsHidden() || !Target->GetMesh()->IsVisible() ||
            Candidate->IsDefeated() || Candidate->GetVitals().Health <= 0)
            continue;

        const FVector ToTarget = Target->GetActorLocation() - Start;
        const double Distance = ToTarget.SizeSquared();
        if (Distance > FMath::Square(BladeStepRange))
            continue;
        const FVector Facing = ToTarget.GetSafeNormal2D();
        const double Alignment = FVector::DotProduct(ViewDirection, (Target->GetActorLocation() - Eye).GetSafeNormal());
        if (Facing.IsNearlyZero() || Alignment < BestAlignment ||
            (Alignment == BestAlignment && Distance >= BestDistance))
            continue;

        FVector Destination;
        if (!IsVisibleInView(*GetWorld(), *Character, *Target, Eye, Projection) ||
            !FindStepDestination(*Character, *Target, Facing, Destination))
            continue;

        BestAlignment = Alignment;
        BestDistance = Distance;
        BladeSteps.Add(Character, {Montage, Target, Start, Destination, Character->GetActorQuat(),
            Facing.ToOrientationQuat(), GetWorld()->GetTimeSeconds()});
    }
}

void UTrioCombatWorld::UpdateBladeSteps()
{
    const double Now = GetWorld()->GetTimeSeconds();
    for (auto Entry = BladeSteps.CreateIterator(); Entry; ++Entry)
    {
        ACharacter* Character = Entry.Key().Get();
        const FBladeStep& Step = Entry.Value();
        const auto* Fighter = Character ? Character->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
        const auto* Target = Step.Target.IsValid() ? Step.Target->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
        const auto* Anim = Character ? Character->GetMesh()->GetAnimInstance() : nullptr;
        if (!Fighter || Fighter->IsDefeated() || !Target || Target->IsDefeated() ||
            !Character->IsLocallyControlled() || !Anim || !Step.Montage.IsValid() ||
            !Anim->Montage_IsPlaying(Step.Montage.Get()))
        {
            Entry.RemoveCurrent();
            continue;
        }

        const double Progress = FMath::Clamp((Now - Step.StartTime) / BladeStepDuration, 0.0, 1.0);
        if (Progress <= 0)
            continue;
        const double Blend = FMath::SmoothStep(0.0, 1.0, Progress);
        const FQuat Facing = FQuat::Slerp(Step.StartRotation, Step.Facing, Blend);
        // Keep the animation's movement when the target is already in blade range.
        FVector Delta = FVector::ZeroVector;
        if (!Step.Start.Equals(Step.Destination))
            Delta = FMath::Lerp(Step.Start, Step.Destination, Blend) - Character->GetActorLocation();
        FHitResult Hit;
        Character->GetCharacterMovement()->MoveUpdatedComponent(Delta, Facing, true, &Hit);
        if (Progress >= 1 || Hit.bBlockingHit)
            Entry.RemoveCurrent();
    }
}
