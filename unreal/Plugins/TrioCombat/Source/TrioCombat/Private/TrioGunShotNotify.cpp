#include "TrioGunShotNotify.h"
#include "TrioCombat.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"

void UTrioGunShotNotify::Notify(USkeletalMeshComponent* Mesh, UAnimSequenceBase*, const FAnimNotifyEventReference&)
{
    if (!Mesh || !Mesh->DoesSocketExist(MuzzleSocket))
        return;

    UWorld* World = Mesh->GetWorld();
    UTrioCombatWorld* Combat = World ? World->GetSubsystem<UTrioCombatWorld>() : nullptr;
    if (Combat)
    {
        const FVector Location = Mesh->GetSocketTransform(MuzzleSocket).TransformPosition(MuzzleOffset);
        Combat->QueueGunShot(Mesh->GetOwner(), Location);
    }
}

void UTrioGunSweepNotify::Notify(USkeletalMeshComponent* Mesh, UAnimSequenceBase* Animation,
    const FAnimNotifyEventReference& EventReference)
{
    if (!Mesh || !Mesh->DoesSocketExist(SocketName))
        return;
    UWorld* World = Mesh->GetWorld();
    if (!World)
        return;
    auto* Combat = World->GetSubsystem<UTrioCombatWorld>();
    if (!World->IsGameWorld() || (Combat && Combat->QueueGunShot(Mesh->GetOwner(),
        Mesh->GetSocketTransform(SocketName).TransformPosition(LocationOffset), true)))
    {
        Super::Notify(Mesh, Animation, EventReference);
    }
}
