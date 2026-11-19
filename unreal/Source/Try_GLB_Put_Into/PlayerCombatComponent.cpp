#include "PlayerCombatComponent.h"
#include "CombatRules.h"
#include "CombatMatchActor.h"
#include "WeaponSyncComponent.h"
#include "Misc/App.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/StructOnScope.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UPlayerCombatComponent::UPlayerCombatComponent()
{
    SetIsReplicatedByDefault(true);
    PrimaryComponentTick.bCanEverTick = true;
}

void UPlayerCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UPlayerCombatComponent, Health);
    DOREPLIFETIME(UPlayerCombatComponent, CharacterChoice);
    DOREPLIFETIME(UPlayerCombatComponent, RespawnAt);
    DOREPLIFETIME(UPlayerCombatComponent, SpawnProtectedUntil);
    DOREPLIFETIME(UPlayerCombatComponent, bCombatActive);
    DOREPLIFETIME(UPlayerCombatComponent, Mode);
    DOREPLIFETIME_CONDITION(UPlayerCombatComponent, SyncedAnimState, COND_SkipOwner);
}

void UPlayerCombatComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(StrikeTimer);
    Super::EndPlay(Reason);
}

void UPlayerCombatComponent::SetCombatActive(bool Active)
{
    if (GetOwner()->HasAuthority())
    {
        bCombatActive = Active;
        if (!bHasSpawnOrigin)
        {
            SpawnOrigin = GetOwner()->GetActorTransform();
            bHasSpawnOrigin = true;
        }
        if (!Active)
        {
            if (!bRoundResetPending) RespawnAt = 0;
            GetWorld()->GetTimerManager().ClearTimer(StrikeTimer);
        }
        RefreshWeaponMode();
    }
}

bool UPlayerCombatComponent::IsCombatActive() const
{
    const auto* Match = ACombatMatchActor::Find(GetWorld());
    return bCombatActive && Match && Match->IsMatchActive();
}

bool UPlayerCombatComponent::HasValidGun() const
{
    const auto* Sync = GetOwner()->FindComponentByClass<UWeaponSyncComponent>();
    if (Sync && !Sync->GetEquipment().bSample && !Sync->GetEquipment().ModelUrl.IsEmpty())
    {
        return false; // Custom forged melee weapon overrides native character weapon mode.
    }
    const FString ClassPath = GetOwner() ? GetOwner()->GetClass()->GetPathName() : FString();
    if (ClassPath.Contains(TEXT("BP_Yousun")) || ClassPath.Contains(TEXT("BP_MyCharacter")))
    {
        return true;
    }
    if (ClassPath.Contains(TEXT("BP_Miyabi")))
    {
        return false;
    }
    return Sync && CombatRules::IsInitialGun(Sync->GetEquipment().bSample, Sync->GetEquipment().Preset);
}

void UPlayerCombatComponent::RequestSelectCharacter(int32 ChoiceIndex)
{
    const auto* Character = Cast<ACharacter>(GetOwner());
    if (!Character || !Character->IsLocallyControlled()) return;
    ChoiceIndex = FMath::Clamp(ChoiceIndex, 0, 2);
    if (CharacterChoice == ChoiceIndex) return;
    ServerSelectCharacter(ChoiceIndex);
}

void UPlayerCombatComponent::ServerSelectCharacter_Implementation(int32 ChoiceIndex)
{
    auto* OwnerChar = Cast<ACharacter>(GetOwner());
    if (!OwnerChar || !OwnerChar->HasAuthority()) return;
    ChoiceIndex = FMath::Clamp(ChoiceIndex, 0, 2);
    if (CharacterChoice == ChoiceIndex) return;

    static const TCHAR* HeroClasses[3] = {
        TEXT("/Game/character/Miyabi/Player/BP_Miyabi.BP_Miyabi_C"),
        TEXT("/Game/character/yousun/Movement/BP_Yousun.BP_Yousun_C"),
        TEXT("/Game/baseAdvRootMotionSys_BP/BP_MyCharacter.BP_MyCharacter_C")
    };
    const TCHAR* TargetPath = HeroClasses[ChoiceIndex];
    if (OwnerChar->GetClass()->GetPathName() == TargetPath)
    {
        CharacterChoice = ChoiceIndex;
        RefreshWeaponMode();
        OwnerChar->ForceNetUpdate();
        return;
    }

    APlayerController* PC = Cast<APlayerController>(OwnerChar->GetController());
    UWorld* World = GetWorld();
    UClass* HeroClass = LoadClass<ACharacter>(nullptr, TargetPath);
    if (!PC || !World || !HeroClass)
    {
        CharacterChoice = ChoiceIndex;
        RefreshWeaponMode();
        return;
    }

    const FTransform SpawnXF = OwnerChar->GetActorTransform();
    ACharacter* NewChar = World->SpawnActorDeferred<ACharacter>(
        HeroClass, SpawnXF, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
    if (!NewChar) return;
    NewChar->SetReplicates(true);
    NewChar->SetReplicateMovement(true);
    NewChar->SetNetUpdateFrequency(60.f);
    NewChar->SetMinNetUpdateFrequency(30.f);
    UGameplayStatics::FinishSpawningActor(NewChar, SpawnXF);

    auto* NewCombat = NewChar->FindComponentByClass<UPlayerCombatComponent>();
    if (!NewCombat)
    {
        NewCombat = NewObject<UPlayerCombatComponent>(NewChar, TEXT("PlayerCombat"));
        NewChar->AddInstanceComponent(NewCombat);
        NewCombat->RegisterComponent();
    }
    NewCombat->CharacterChoice = ChoiceIndex;
    NewCombat->bHasSpawnOrigin = true;
    NewCombat->SpawnOrigin = SpawnXF;
    NewCombat->bCombatActive = bCombatActive;
    NewCombat->Health = Health;
    NewCombat->SpawnProtectedUntil = SpawnProtectedUntil;

    if (!NewChar->FindComponentByClass<UWeaponSyncComponent>())
    {
        auto* NewSync = NewObject<UWeaponSyncComponent>(NewChar, TEXT("WeaponEquipment"));
        NewChar->AddInstanceComponent(NewSync);
        NewSync->RegisterComponent();
    }

    PC->Possess(NewChar);
    NewCombat->RefreshWeaponMode();
    NewChar->ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("[Combat] HERO_SPAWNED choice=%d class=%s"), ChoiceIndex, *HeroClass->GetName());
    OwnerChar->Destroy();
}

void UPlayerCombatComponent::RefreshWeaponMode()
{
    if (GetOwner()->HasAuthority()) Mode = HasValidGun() ? ECombatWeaponMode::Rifle : ECombatWeaponMode::Melee;
}

void UPlayerCombatComponent::RequestAttack()
{
    auto* Character = Cast<ACharacter>(GetOwner());
    auto* PC = Character ? Cast<APlayerController>(Character->GetController()) : nullptr;
    if (!PC || !PC->IsLocalController() || !IsCombatActive() || Health <= 0) return;
    FVector ViewOrigin;
    FRotator ViewRotation;
    PC->GetPlayerViewPoint(ViewOrigin, ViewRotation);
    FVector Direction = ViewRotation.Vector();
    if (Mode == ECombatWeaponMode::Rifle)
    {
        // Converge the third-person crosshair onto an aim direction. Only the server decides hits.
        FHitResult CameraHit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(CombatCameraAim), false, Character);
        FVector AimPoint = ViewOrigin + Direction * CombatRules::RifleReach;
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_Pawn);
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        if (GetWorld()->LineTraceSingleByObjectType(CameraHit, ViewOrigin, AimPoint, Objects, Params))
            AimPoint = CameraHit.ImpactPoint;
        Direction = (AimPoint - Character->GetPawnViewLocation()).GetSafeNormal();
    }
    ServerRequestAttack(Direction);
}

void UPlayerCombatComponent::ServerRequestAttack_Implementation(FVector_NetQuantizeNormal InDirection)
{
    auto* Character = Cast<ACharacter>(GetOwner());
    const double Now = GetWorld()->GetTimeSeconds();
    FVector Direction = FVector(InDirection);
    if (!Character || !Character->IsPlayerControlled() || Direction.ContainsNaN()
        || !CombatRules::CanAttack(Health, IsCombatActive(), Now, LastServerAttack)) return;
    Direction = Direction.GetSafeNormal();
    if (Direction.IsNearlyZero() || FVector::DotProduct(Direction, Character->GetBaseAimRotation().Vector()) < .25f) return;
    if (Mode == ECombatWeaponMode::Rifle && !HasValidGun()) return;
    LastServerAttack = Now;
    const ECombatWeaponMode AttackMode = Mode;
    MulticastAttackStarted(AttackMode);
    if (AttackMode == ECombatWeaponMode::Melee)
    {
        Direction.Z = 0;
        Direction.Normalize();
        Character->SetActorRotation(Direction.Rotation());
        GetWorld()->GetTimerManager().SetTimer(StrikeTimer,
            FTimerDelegate::CreateWeakLambda(this, [this, Direction, AttackMode]() { ResolveAttack(Direction, AttackMode); }),
            CombatRules::MeleeWindup, false);
    }
    else ResolveAttack(Direction, AttackMode);
}

void UPlayerCombatComponent::ResolveAttack(FVector Direction, ECombatWeaponMode AttackMode)
{
    auto* Character = Cast<ACharacter>(GetOwner());
    if (!Character || Health <= 0 || !IsCombatActive()) return;
    const FVector Start = AttackMode == ECombatWeaponMode::Rifle ? Character->GetPawnViewLocation()
        : Character->GetActorLocation() + FVector(0, 0, 35);
    const FVector End = Start + Direction * (AttackMode == ECombatWeaponMode::Rifle ? CombatRules::RifleReach : CombatRules::MeleeReach);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(PlayerCombatHit), false, Character);
    ACharacter* Victim = nullptr;
    FVector Impact = End;
    if (AttackMode == ECombatWeaponMode::Rifle)
    {
        FHitResult Hit;
        FCollisionObjectQueryParams Objects;
        Objects.AddObjectTypesToQuery(ECC_Pawn);
        Objects.AddObjectTypesToQuery(ECC_WorldStatic);
        Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
        if (GetWorld()->LineTraceSingleByObjectType(Hit, Start, End, Objects, Params))
        {
            Impact = Hit.ImpactPoint;
            Victim = Cast<ACharacter>(Hit.GetActor());
        }
    }
    else
    {
        TArray<FHitResult> Hits;
        GetWorld()->SweepMultiByObjectType(Hits, Start + Direction * 30, End, FQuat::Identity,
            FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeSphere(35), Params);
        float Nearest = FLT_MAX;
        for (const FHitResult& Hit : Hits)
        {
            auto* Candidate = Cast<ACharacter>(Hit.GetActor());
            auto* Combat = Candidate ? Candidate->FindComponentByClass<UPlayerCombatComponent>() : nullptr;
            if (!Candidate || Candidate == Character || !Combat || Combat->Health <= 0) continue;
            const FVector Delta = Candidate->GetActorLocation() - Character->GetActorLocation();
            if (FVector::DotProduct(Delta.GetSafeNormal2D(), Direction) < .2f || FMath::Abs(Delta.Z) > 110) continue;
            FHitResult Barrier;
            const FVector Target = Candidate->GetActorLocation() + FVector(0, 0, 35);
            if (GetWorld()->LineTraceSingleByChannel(Barrier, Start, Target, ECC_Visibility, Params)
                && Barrier.GetActor() != Candidate) continue;
            if (Delta.SizeSquared() < Nearest) { Nearest = Delta.SizeSquared(); Victim = Candidate; Impact = Target; }
        }
    }
    auto* TargetCombat = Victim ? Victim->FindComponentByClass<UPlayerCombatComponent>() : nullptr;
    const bool bHit = TargetCombat && CombatRules::CanReceiveDamage(TargetCombat->Health,
        TargetCombat->IsCombatActive(), GetServerTime(), TargetCombat->SpawnProtectedUntil);
    if (bHit) TargetCombat->ApplyHit(this, Direction);
    MulticastAttackResult(Start, Impact, bHit ? Victim : nullptr, bHit, AttackMode);
}

void UPlayerCombatComponent::ApplyHit(UPlayerCombatComponent* Attacker, const FVector& Direction)
{
    if (!GetOwner()->HasAuthority() || Attacker == this || !Attacker
        || !CombatRules::CanReceiveDamage(Health, IsCombatActive(), GetServerTime(), SpawnProtectedUntil)) return;
    const bool Lethal = CombatRules::IsLethalHit(Health, CombatRules::Damage);
    Health = CombatRules::ApplyDamage(Health, CombatRules::Damage);
    if (Lethal)
    {
        const auto* KillerCharacter = Attacker ? Cast<ACharacter>(Attacker->GetOwner()) : nullptr;
        Die(KillerCharacter ? KillerCharacter->GetPlayerState() : nullptr);
    }
    else if (auto* Character = Cast<ACharacter>(GetOwner()))
        Character->LaunchCharacter(Direction.GetSafeNormal2D() * 650 + FVector(0, 0, 180), true, true);
    GetOwner()->ForceNetUpdate();
    OnRep_Health();
    UE_LOG(LogTemp, Display, TEXT("[Combat] HIT victim=%s hp=%d attacker=%s"), *GetOwner()->GetName(), Health, *Attacker->GetOwner()->GetName());
}

void UPlayerCombatComponent::OnRep_Health()
{
    UpdateLifePresentation();
    Feedback = Health < PreviousHealth ? FString::Printf(TEXT("受到攻击 -%d"), PreviousHealth - Health) : TEXT("生命已恢复");
    PreviousHealth = Health;
    FeedbackUntil = GetWorld()->GetTimeSeconds() + 2;
}

void UPlayerCombatComponent::MulticastAttackStarted_Implementation(ECombatWeaponMode AttackMode)
{
    LastVisualAttack = GetWorld()->GetTimeSeconds();
    VisualAttackMode = AttackMode;
}

void UPlayerCombatComponent::MulticastAttackResult_Implementation(FVector_NetQuantize Start, FVector_NetQuantize End,
    AActor* Victim, bool bHit, ECombatWeaponMode AttackMode)
{
    if (FApp::CanEverRender() && GetWorld())
    {
        const FVector AimDir = (FVector(End) - FVector(Start)).GetSafeNormal();
        const FRotator FacingRot = AimDir.IsNearlyZero() ? FRotator::ZeroRotator : AimDir.Rotation();
        if (AttackMode == ECombatWeaponMode::Melee)
        {
            if (auto* SlashVfx = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/StylizedSlashesV2/Particles/NiagaraSystems/NS_Slash_Cyber.NS_Slash_Cyber")))
            {
                UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), SlashVfx, Start + AimDir * 70.f, FacingRot, FVector(0.65f));
            }
        }
        else if (AttackMode == ECombatWeaponMode::Rifle)
        {
            if (auto* OwnerChar = Cast<ACharacter>(GetOwner()); OwnerChar && !OwnerChar->IsLocallyControlled())
            {
                if (UFunction* SniperVfxFn = OwnerChar->FindFunction(TEXT("PlayYousunSniperVFX")))
                {
                    FStructOnScope Params(SniperVfxFn);
                    void* Data = Params.GetStructMemory();
                    const auto SetVec = [&](FName Name, const FVector& Val)
                    {
                        if (const auto* Prop = FindFProperty<FStructProperty>(SniperVfxFn, Name);
                            Prop && Prop->Struct == TBaseStructure<FVector>::Get())
                        {
                            *Prop->ContainerPtrToValuePtr<FVector>(Data) = Val;
                        }
                    };
                    FVector MuzzleLoc = FVector(Start);
                    if (const auto* WeaponProp = FindFProperty<FObjectPropertyBase>(OwnerChar->GetClass(), TEXT("YousunWeapon")))
                    {
                        if (auto* WeaponMesh = Cast<USkeletalMeshComponent>(WeaponProp->GetObjectPropertyValue_InContainer(OwnerChar)))
                        {
                            if (WeaponMesh->DoesSocketExist(TEXT("Ctr_Weapon_04")))
                                MuzzleLoc = WeaponMesh->GetSocketTransform(TEXT("Ctr_Weapon_04")).TransformPosition(FVector(-0.01734, -12.06113, -5.14991));
                            else if (WeaponMesh->DoesSocketExist(TEXT("Ctr_Weapon_01")))
                                MuzzleLoc = WeaponMesh->GetSocketLocation(TEXT("Ctr_Weapon_01"));
                        }
                    }
                    SetVec(TEXT("Muzzle"), MuzzleLoc);
                    SetVec(TEXT("TraceStart"), FVector(Start));
                    SetVec(TEXT("AimPoint"), FVector(End));
                    SetVec(TEXT("ImpactPoint"), FVector(End));
                    SetVec(TEXT("ImpactNormal"), -AimDir);
                    if (const auto* HitProp = FindFProperty<FBoolProperty>(SniperVfxFn, TEXT("BlockingHit")))
                        HitProp->SetPropertyValue_InContainer(Data, bHit);
                    OwnerChar->ProcessEvent(SniperVfxFn, Data);
                }
                else if (auto* GunImpact = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/Shooter_VFXPack/Particles/HitsAndExplosions/P_Hit_Fire.P_Hit_Fire")))
                {
                    UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), GunImpact, End, (-AimDir).Rotation());
                }
            }
        }
        if (bHit)
        {
            if (auto* HitVfx = LoadObject<UNiagaraSystem>(nullptr, TEXT("/Game/StylizedSlashesV2/Particles/NiagaraSystems/NS_SlashImpact_Nature.NS_SlashImpact_Nature")))
            {
                UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), HitVfx, End, FacingRot, FVector(0.8f));
            }
        }
    }
    Feedback = bHit ? TEXT("命中 -25") : TEXT("未命中");
    FeedbackUntil = GetWorld()->GetTimeSeconds() + 1.5;
}

FString UPlayerCombatComponent::GetFeedback() const
{
    return GetWorld()->GetTimeSeconds() <= FeedbackUntil ? Feedback : FString();
}

void UPlayerCombatComponent::GetAttackPose(float& Pitch, float& Yaw, float& Extension) const
{
    Pitch = Yaw = Extension = 0;
    if (Health <= 0) return;
    const float Time = GetWorld()->GetTimeSeconds() - LastVisualAttack;
    if (Time < 0 || Time > .5f) return;
    if (VisualAttackMode == ECombatWeaponMode::Melee)
        Yaw = Time < .2f ? FMath::Lerp(-60.f, 0.f, Time / .2f) : FMath::Lerp(0.f, 60.f, (Time - .2f) / .3f);
    else
    {
        Pitch = -10 * FMath::Max(0.f, 1 - Time / .2f);
        Extension = -4 * FMath::Max(0.f, 1 - Time / .2f);
    }
}

double UPlayerCombatComponent::GetServerTime() const
{
    const auto* State = GetWorld()->GetGameState();
    return State ? State->GetServerWorldTimeSeconds() : GetWorld()->GetTimeSeconds();
}

double UPlayerCombatComponent::GetRespawnRemaining() const
{
    return Health <= 0 ? FMath::Max(0.0, RespawnAt - GetServerTime()) : 0.0;
}

bool UPlayerCombatComponent::IsSpawnProtected() const
{
    return CombatRules::IsSpawnProtected(Health, GetServerTime(), SpawnProtectedUntil);
}

double UPlayerCombatComponent::GetSpawnProtectionRemaining() const
{
    return IsSpawnProtected() ? FMath::Max(0.0, SpawnProtectedUntil-GetServerTime()) : 0.0;
}

void UPlayerCombatComponent::UpdateLifePresentation()
{
    auto* Character = Cast<ACharacter>(GetOwner());
    if (!Character) return;
    const bool Dead = Health <= 0;
    if (Dead != bAppliedDead)
    {
        if (Dead)
        {
            LivingCollision = Character->GetCapsuleComponent()->GetCollisionEnabled();
            LivingMeshCollision = Character->GetMesh()->GetCollisionEnabled();
            Character->StopJumping();
            Character->GetCharacterMovement()->StopMovementImmediately();
            Character->GetCharacterMovement()->DisableMovement();
            Character->GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Character->GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        }
        else
        {
            Character->GetCapsuleComponent()->SetCollisionEnabled(LivingCollision);
            Character->GetMesh()->SetCollisionEnabled(LivingMeshCollision);
            Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        }
        Character->GetMesh()->SetVisibility(!Dead, true);
        LastVisualAttack = -100;
        bAppliedDead = Dead;
    }
}

void UPlayerCombatComponent::Die(APlayerState* Killer)
{
    // Called on the authority only, once per living -> dead transition.
    Health = 0;
    SpawnProtectedUntil = 0;
    RespawnAt = GetServerTime() + CombatRules::RespawnDelay;
    GetWorld()->GetTimerManager().ClearTimer(StrikeTimer);
    UpdateLifePresentation();
    const auto* Character = Cast<ACharacter>(GetOwner());
    if (auto* Match = ACombatMatchActor::Find(GetWorld()))
        Match->NotifyElimination(Killer, Character ? Character->GetPlayerState() : nullptr);
    GetOwner()->ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("[Combat] DEATH owner=%s respawnAt=%.2f"), *GetOwner()->GetName(), RespawnAt);
}

bool UPlayerCombatComponent::LoadRespawnOrigins()
{
    bRespawnOriginsLoaded = true;
    if (!GetWorld()->GetMapName().EndsWith(TEXT("Lvl_ZZZ")))
    {
        for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It) RespawnOrigins.Add(It->GetActorTransform());
        return !RespawnOrigins.IsEmpty();
    }
    FString Text;
    TSharedPtr<FJsonObject> Root;
    const TArray<TSharedPtr<FJsonValue>>* Points=nullptr;
    double Version=0;
    FString Map;
    if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectContentDir()/TEXT("Map/ZZZ_respawns.json")))
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root
        || !Root->TryGetNumberField(TEXT("version"), Version) || Version!=1
        || !Root->TryGetStringField(TEXT("source_map"), Map) || Map!=TEXT("/Game/CityZZZ/Lvl_ZZZ")
        || !Root->TryGetArrayField(TEXT("points"), Points) || Points->Num()!=8) return false;
    for (const auto& Value : *Points)
    {
        const TSharedPtr<FJsonObject>* Point=nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Position=nullptr;
        double XYZ[3], Yaw;
        if (!Value->TryGetObject(Point) || !Point->IsValid()
            || !(*Point)->TryGetArrayField(TEXT("position"), Position) || Position->Num()!=3
            || !(*Point)->TryGetNumberField(TEXT("yaw"), Yaw) || !FMath::IsFinite(Yaw)) return false;
        for (int32 I=0; I<3; ++I)
            if (!(*Position)[I]->TryGetNumber(XYZ[I]) || !FMath::IsFinite(XYZ[I])) return false;
        RespawnOrigins.Add(FTransform(FRotator(0,Yaw,0),FVector(XYZ[0],XYZ[1],XYZ[2])));
    }
    UE_LOG(LogTemp,Display,TEXT("[Combat] Loaded 8 fixed ZZZ road spawns; server random, collision checks only"));
    return true;
}

bool UPlayerCombatComponent::FindRespawnTransform(FTransform& OutTransform)
{
    auto* Character = Cast<ACharacter>(GetOwner());
    if (!Character || !Character->HasAuthority()) return false;
    if (!bRespawnOriginsLoaded && !LoadRespawnOrigins())
    {
        RespawnOrigins.Reset();
        UE_LOG(LogTemp,Error,TEXT("[Combat] Invalid fixed spawn data; respawn waits rather than using old offsets"));
    }
    // A random permutation gives each fixed point an equal first chance. Only collision can reject it.
    TArray<int32> Order;
    for (int32 I=0; I<RespawnOrigins.Num(); ++I) Order.Add(I);
    for (int32 I=Order.Num()-1; I>0; --I) Order.Swap(I,FMath::RandRange(0,I));
    const auto* Capsule = Character->GetCapsuleComponent();
    const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    const float Radius = Capsule->GetScaledCapsuleRadius();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(CombatRespawn), false, Character);
    FCollisionObjectQueryParams GroundObjects;
    GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    GroundObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
    for (const int32 Index : Order)
    {
        const FTransform& Origin=RespawnOrigins[Index];
        const FVector Base=Origin.GetLocation();
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByObjectType(Ground, Base+FVector(0,0,300),
            Base-FVector(0,0,650),GroundObjects,Params) || Ground.ImpactNormal.Z<.6f) continue;
        const FVector Position=Ground.ImpactPoint+FVector(0,0,HalfHeight+5);
        if (GetWorld()->OverlapBlockingTestByChannel(Position,FQuat::Identity,ECC_Pawn,
            FCollisionShape::MakeCapsule(Radius,HalfHeight),Params)) continue;
        OutTransform=FTransform(Origin.GetRotation(),Position);
        UE_LOG(LogTemp,Display,TEXT("[Combat] RANDOM_SPAWN point=%d position=%s"),Index+1,*Position.ToString());
        return true;
    }
    return false;
}

bool UPlayerCombatComponent::Respawn()
{
    auto* Character = Cast<ACharacter>(GetOwner());
    if (!Character || !Character->HasAuthority()) return false;
    FTransform Destination;
    if (!FindRespawnTransform(Destination))
    {
        RespawnAt = GetServerTime() + 1; // Occupied spawn: retry instead of spawning inside a player/wall.
        return false;
    }
    if (!Character->TeleportTo(Destination.GetLocation(), Destination.Rotator(), false, true))
    {
        RespawnAt = GetServerTime()+1;
        return false;
    }
    Character->GetCharacterMovement()->StopMovementImmediately();
    SuppressClientAuthPosUntil = GetWorld()->GetTimeSeconds() + 0.6;
    ClientSnapToRespawn(Destination.GetLocation(), Destination.Rotator());
    if (auto* PC = Cast<APlayerController>(Character->GetController()))
    {
        PC->SetControlRotation(Destination.Rotator());
        PC->ClientSetRotation(Destination.Rotator(), true);
    }
    Health = CombatRules::MaxHealth;
    SpawnProtectedUntil = GetServerTime()+CombatRules::SpawnProtectionSeconds;
    bRoundResetPending = false;
    RespawnAt = 0;
    LastServerAttack = -100;
    LastVisualAttack = -100;
    GetWorld()->GetTimerManager().ClearTimer(StrikeTimer);
    GetOwner()->ForceNetUpdate();
    OnRep_Health();
    UE_LOG(LogTemp, Display, TEXT("[Combat] RESPAWN owner=%s position=%s protectedUntil=%.2f"),
        *GetOwner()->GetName(),*Destination.GetLocation().ToString(),SpawnProtectedUntil);
    return true;
}

void UPlayerCombatComponent::ResetForRound()
{
    if (!GetOwner()->HasAuthority()) return;
    bRoundResetPending = true;
    bCombatActive = false;
    SpawnProtectedUntil = 0;
    GetWorld()->GetTimerManager().ClearTimer(StrikeTimer);
    Health = 0;
    UpdateLifePresentation();
    Respawn(); // All eight points occupied: retry before the readiness gate can open.
}

void UPlayerCombatComponent::RequestRestartRound()
{
    const auto* Character = Cast<ACharacter>(GetOwner());
    if (Character && Character->IsLocallyControlled()) ServerRequestRestartRound();
}

void UPlayerCombatComponent::ServerRequestRestartRound_Implementation()
{
    auto* Character = Cast<ACharacter>(GetOwner());
    if (auto* Match = ACombatMatchActor::Find(GetWorld()))
        Match->RestartRound(Character ? Cast<APlayerController>(Character->GetController()) : nullptr);
}

void UPlayerCombatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    UpdateLifePresentation();
    UpdateAnimationSync(DeltaTime);
    if (Health <= 0)
        if (auto* DeadCharacter = Cast<ACharacter>(GetOwner())) DeadCharacter->GetCharacterMovement()->DisableMovement();
    if (!GetOwner()->HasAuthority() || !bHasSpawnOrigin) return;
    if (SpawnProtectedUntil>0 && GetServerTime()>=SpawnProtectedUntil)
    {
        SpawnProtectedUntil=0;
        GetOwner()->ForceNetUpdate();
    }
    if (bRoundResetPending && GetServerTime() >= RespawnAt) { Respawn(); return; }
    auto* Character = Cast<ACharacter>(GetOwner());
    if (!Character) return;
    const double FallBoundary = CombatRules::FallBoundary(SpawnOrigin.GetLocation().Z, GetWorld()->GetWorldSettings()->KillZ);
    if (Health > 0 && Character->GetActorLocation().Z < FallBoundary)
    {
        if (IsCombatActive()) Die();
        else Character->GetCharacterMovement()->StopMovementImmediately();
        // Keep the dead Pawn away from the engine's destructive KillZ boundary.
        Character->TeleportTo(SpawnOrigin.GetLocation(), SpawnOrigin.Rotator(), false, true);
        OnRep_Health();
    }
    if (CombatRules::CanRespawn(Health, IsCombatActive(), GetServerTime(), RespawnAt)) Respawn();
}
