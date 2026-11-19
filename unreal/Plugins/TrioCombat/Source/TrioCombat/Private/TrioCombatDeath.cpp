#include "TrioCombat.h"
#include "Blueprint/UserWidget.h"
#include "Components/CapsuleComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UObject/UnrealType.h"

namespace
{
    constexpr float FallBlend = 0.23f;
    constexpr float EarliestFade = 1.45f;
    constexpr float LatestFade = 2.55f;
    constexpr float FadeDuration = 1.05f;
    const FName DeathVisibility(TEXT("TrioDeathVisibility"));

    FString DeathAssetPath(const TCHAR* Prefix, const UObject& Source)
    {
        const FString Name = FString(Prefix) + Source.GetName();
        return FString(TEXT("/TrioCombat/Death/")) + Name + TEXT(".") + Name;
    }
}

UTrioCombatReceiver::UTrioCombatReceiver()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UTrioCombatReceiver::BeginDeath(const AActor* Attacker)
{
    bDefeated = true;
    AActor* Owner = GetOwner();
    Owner->SetCanBeDamaged(false);
    Owner->SetActorTickEnabled(false);

    if (ACharacter* Character = Cast<ACharacter>(Owner))
    {
        const FVector Velocity = Character->GetVelocity();
        Character->GetCharacterMovement()->DisableMovement();
        Character->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        if (auto* Controller = Cast<APlayerController>(Character->GetController()))
            Character->DisableInput(Controller);

        // Close weapon-specific overlays before the controller changes pawns.
        for (TFieldIterator<FObjectPropertyBase> Property(Owner->GetClass()); Property; ++Property)
            if (Property->PropertyClass->IsChildOf(UUserWidget::StaticClass()))
                if (auto* Widget = Cast<UUserWidget>(Property->GetObjectPropertyValue_InContainer(Owner)))
                    Widget->RemoveFromParent();

        USkeletalMeshComponent* Mesh = Character->GetMesh();
        if (const auto* Asset = Mesh->GetSkeletalMeshAsset())
        {
            if (auto* DeathPhysics = LoadObject<UPhysicsAsset>(nullptr, *DeathAssetPath(TEXT("PA_Death_"), *Asset), nullptr, LOAD_NoWarn))
                Mesh->SetPhysicsAsset(DeathPhysics, true);
        }
        if (Mesh->GetPhysicsAsset())
        {
            Mesh->SetOwnerNoSee(false);
            Mesh->SetCollisionProfileName(TEXT("Ragdoll"));
            Mesh->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
            Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
            Mesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
            Mesh->SetAllBodiesSimulatePhysics(true);
            Mesh->SetPhysicsBlendWeight(0.01f);
            Mesh->SetAllUseCCD(true);
            Mesh->WakeAllRigidBodies();
            FVector Motion = Velocity.GetClampedToMaxSize(420);
            if (Attacker && Attacker != Owner)
                Motion += (Owner->GetActorLocation() - Attacker->GetActorLocation()).GetSafeNormal2D() * 95;
            Mesh->SetAllPhysicsLinearVelocity(Motion);
        }
        TInlineComponentArray<USkeletalMeshComponent*> SecondaryMeshes(Character);
        for (USkeletalMeshComponent* Child : SecondaryMeshes)
            if (Child != Mesh)
                Child->SetOwnerNoSee(false);
    }
    else
    {
        Owner->SetActorEnableCollision(false);
    }
    SetComponentTickEnabled(true);
}

void UTrioCombatReceiver::PrepareDeathMaterials()
{
    TInlineComponentArray<UMeshComponent*> Meshes(GetOwner());
    for (UMeshComponent* Mesh : Meshes)
    {
        if (!Mesh->IsVisible() || Mesh->bHiddenInGame)
            continue;
        for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
        {
            UMaterialInterface* Original = Mesh->GetMaterial(Slot);
            const UMaterial* Base = Original ? Original->GetMaterial() : nullptr;
            UMaterialInterface* Template = Base ? LoadObject<UMaterialInterface>(nullptr,
                *DeathAssetPath(TEXT("M_Death_"), *Base), nullptr, LOAD_NoWarn) : nullptr;
            if (!Template)
                continue;

            UMaterialInstanceDynamic* Fade = UMaterialInstanceDynamic::Create(Template, Mesh);
            Fade->CopyMaterialUniformParameters(Original);
            Fade->SetScalarParameterValue(DeathVisibility, 1);
            Mesh->SetMaterial(Slot, Fade);
            DeathMaterials.Add(Fade);
        }
    }
}

void UTrioCombatReceiver::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    const float PreviousElapsed = DeathElapsed;
    DeathElapsed += DeltaTime;
    if (PreviousElapsed < FallBlend)
    {
        if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
        {
            USkeletalMeshComponent* Mesh = Character->GetMesh();
            const float Alpha = FMath::Clamp(DeathElapsed / FallBlend, 0.0f, 1.0f);
            Mesh->SetAllBodiesPhysicsBlendWeight(FMath::SmoothStep(0.0f, 1.0f, Alpha));
        }
    }
    if (FadeStart < 0 && DeathElapsed >= EarliestFade)
    {
        const ACharacter* Character = Cast<ACharacter>(GetOwner());
        auto* Mesh = Character ? Character->GetMesh() : nullptr;
        if (DeathElapsed >= LatestFade || !Mesh || !Mesh->IsAnyRigidBodyAwake())
        {
            FadeStart = DeathElapsed;
            PrepareDeathMaterials();
        }
    }
    if (FadeStart < 0)
        return;

    const float Fade = FMath::Clamp((DeathElapsed - FadeStart) / FadeDuration, 0.0f, 1.0f);
    const float Visibility = 1 - FMath::SmoothStep(0.0f, 1.0f, Fade);
    for (UMaterialInstanceDynamic* Material : DeathMaterials)
        Material->SetScalarParameterValue(DeathVisibility, Visibility);
    if (Fade >= 1)
        GetOwner()->Destroy();
}

