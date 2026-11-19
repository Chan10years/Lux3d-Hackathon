#include "TrioBotController.h"
#include "TrioCombat.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace
{
    const FName Guns(TEXT("Guns"));
    const FName Miyabi(TEXT("Miyabi"));
    const FName Yousun(TEXT("Yousun"));
    const FName SniperSlot(TEXT("YousunCombat"));
    constexpr double LostSightDelay = 2.5;
    constexpr double SightDistance = 3200.0;

    bool IsAlive(const ACharacter* Actor)
    {
        const auto* Receiver = IsValid(Actor) ? Actor->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
        return Receiver && !Receiver->IsDefeated() && Receiver->GetVitals().Health > 0;
    }

    FVector Chest(const ACharacter& Character)
    {
        return Character.GetActorLocation() + FVector(0, 0, Character.GetSimpleCollisionHalfHeight() * 0.26);
    }

    bool CanSee(const ACharacter& Observer, const ACharacter& Target)
    {
        if (Target.IsHidden() || FVector::DistSquared(Observer.GetActorLocation(), Target.GetActorLocation()) >
            FMath::Square(SightDistance))
            return false;

        FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioBotSight), false, &Observer);
        FHitResult Hit;
        return !Observer.GetWorld()->LineTraceSingleByChannel(Hit, Observer.GetPawnViewLocation(), Chest(Target),
            ECC_Visibility, Query) || Hit.GetActor() == &Target;
    }

    int32 ReadInt(const AActor& Actor, FName Name)
    {
        const auto* Value = FindFProperty<FIntProperty>(Actor.GetClass(), Name);
        return Value ? Value->GetPropertyValue_InContainer(&Actor) : 0;
    }

    double ReadFloat(const AActor& Actor, FName Name)
    {
        if (const auto* Value = FindFProperty<FDoubleProperty>(Actor.GetClass(), Name))
            return Value->GetPropertyValue_InContainer(&Actor);
        if (const auto* Value = FindFProperty<FFloatProperty>(Actor.GetClass(), Name))
            return Value->GetPropertyValue_InContainer(&Actor);
        return 0;
    }

    bool ReadBool(const AActor& Actor, FName Name)
    {
        const auto* Value = FindFProperty<FBoolProperty>(Actor.GetClass(), Name);
        return Value && Value->GetPropertyValue_InContainer(&Actor);
    }

    void WriteBool(AActor& Actor, FName Name, bool bValue)
    {
        if (const auto* Value = FindFProperty<FBoolProperty>(Actor.GetClass(), Name))
            Value->SetPropertyValue_InContainer(&Actor, bValue);
    }

    void ResetInt(AActor& Actor, FName Name)
    {
        if (const auto* Value = FindFProperty<FIntProperty>(Actor.GetClass(), Name))
            Value->SetPropertyValue_InContainer(&Actor, 0);
    }

    void RunEvent(AActor& Actor, FName Name)
    {
        if (auto* Event = Actor.FindFunction(Name); Event && Event->ParmsSize == 0)
            Actor.ProcessEvent(Event, nullptr);
    }

    USkeletalMeshComponent* SniperWeapon(const ACharacter& Fighter)
    {
        const auto* Weapon = FindFProperty<FObjectPropertyBase>(Fighter.GetClass(), TEXT("YousunWeapon"));
        return Weapon ? Cast<USkeletalMeshComponent>(Weapon->GetObjectPropertyValue_InContainer(&Fighter)) : nullptr;
    }

    bool FireTrace(const ACharacter& Fighter, FVector Muzzle, FVector Aim, double Spread, FHitResult& Hit)
    {
        const FVector Direction = (Aim - Muzzle).GetSafeNormal();
        const FVector Scattered = FMath::VRandCone(Direction, FMath::DegreesToRadians(Spread));
        const FVector End = Muzzle + Scattered * (FVector::Dist(Muzzle, Aim) + 130.0);
        FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioBotShot), false, &Fighter);
        return Fighter.GetWorld()->LineTraceSingleByChannel(Hit, Muzzle, End, ECC_Visibility, Query);
    }

    void PlaySniperVFX(ACharacter& Fighter, FVector Muzzle, FVector Aim, const FHitResult& Hit)
    {
        UFunction* Function = Fighter.FindFunction(TEXT("PlayYousunSniperVFX"));
        if (!Function)
            return;
        FStructOnScope Params(Function);
        void* Data = Params.GetStructMemory();
        const auto VectorParam = [&](FName Name, FVector Value)
        {
            if (const auto* Param = FindFProperty<FStructProperty>(Function, Name);
                Param && Param->Struct == TBaseStructure<FVector>::Get())
                *Param->ContainerPtrToValuePtr<FVector>(Data) = Value;
        };
        VectorParam(TEXT("Muzzle"), Muzzle);
        VectorParam(TEXT("TraceStart"), Muzzle);
        VectorParam(TEXT("AimPoint"), Hit.TraceEnd);
        VectorParam(TEXT("ImpactPoint"), Hit.bBlockingHit ? FVector(Hit.ImpactPoint) : Aim);
        VectorParam(TEXT("ImpactNormal"), Hit.ImpactNormal);
        if (const auto* Param = FindFProperty<FBoolProperty>(Function, TEXT("BlockingHit")))
            Param->SetPropertyValue_InContainer(Data, Hit.bBlockingHit);
        Fighter.ProcessEvent(Function, Data);
    }
}

void UTrioCombatWorld::UpdateBots(APlayerController& Player)
{
    const bool bPressed = Player.IsInputKeyDown(EKeys::Five) || Player.IsInputKeyDown(EKeys::NumPadFive);
    if (bPressed && !bWasBotKeyDown)
    {
        bBotsEnabled = !bBotsEnabled;
        if (!bBotsEnabled)
            StopBots();
    }
    bWasBotKeyDown = bPressed;
    if (!bBotsEnabled || GetWorld()->GetNetMode() == NM_Client)
        return;

    Bots.RemoveAll([](const auto& Weak)
    {
        if (auto* Bot = Weak.Get())
        {
            if (IsAlive(Cast<ACharacter>(Bot->GetPawn())))
                return false;
            Bot->Destroy();
        }
        return true;
    });
    for (const auto& Pair : RosterActors)
    {
        auto* Fighter = Cast<ACharacter>(Pair.Value.Get());
        if (!IsAlive(Fighter) || Fighter->GetController())
            continue;
        if (auto* Bot = GetWorld()->SpawnActor<ATrioBotController>())
        {
            Bot->Possess(Fighter);
            Bots.Add(Bot);
        }
    }
}

void UTrioCombatWorld::StopBots()
{
    for (const auto& Weak : Bots)
        if (auto* Bot = Weak.Get())
            Bot->Destroy();
    Bots.Reset();
}

ACharacter* UTrioCombatWorld::FindBotTarget(const ACharacter& Character, const ACharacter* Excluded, bool bSearching) const
{
    ACharacter* Best = nullptr;
    double BestDistance = TNumericLimits<double>::Max();
    int32 Candidates = 0;
    for (const auto& Pair : RosterActors)
    {
        auto* Other = Cast<ACharacter>(Pair.Value.Get());
        if (Other == &Character || Other == Excluded || !IsAlive(Other) || Other->IsHidden())
            continue;
        if (bSearching)
        {
            // Choose uniformly without allocating a list of targets.
            if (FMath::RandHelper(++Candidates) == 0)
                Best = Other;
            continue;
        }

        const double Distance = FVector::DistSquared(Character.GetActorLocation(), Other->GetActorLocation());
        if (Distance < BestDistance && CanSee(Character, *Other))
        {
            BestDistance = Distance;
            Best = Other;
        }
    }
    return Best;
}

ATrioBotController::ATrioBotController()
{
    PrimaryActorTick.bCanEverTick = true;
    bSetControlRotationFromPawnOrientation = false;
}

void ATrioBotController::OnPossess(APawn* NewPawn)
{
    Super::OnPossess(NewPawn);
    if (auto* Combat = GetWorld()->GetSubsystem<UTrioCombatWorld>())
        Role = Combat->Identify(NewPawn);
    StrafeSide = FMath::RandBool() ? 1 : -1;
    NextAction = GetWorld()->GetTimeSeconds() + FMath::FRandRange(0.25, 0.65);
    SetControlRotation(NewPawn->GetActorRotation());
}

void ATrioBotController::OnUnPossess()
{
    if (auto* Anim = ManagedAnim.Get())
        Anim->SetRootMotionMode(PreviousRootMotion);
    ManagedAnim.Reset();
    ACharacter* Fighter = Cast<ACharacter>(GetPawn());
    if (IsAlive(Fighter))
    {
        auto* Movement = Fighter->GetCharacterMovement();
        Movement->StopMovementImmediately();
        Fighter->ConsumeMovementInputVector();
        if (auto* Anim = Fighter->GetMesh()->GetAnimInstance())
        {
            if (Role == Guns)
            {
                RunEvent(*Fighter, TEXT("HandleGunFireReleased"));
                if (UAnimMontage* Montage = Anim->GetCurrentActiveMontage();
                    Montage && Montage->GetName().StartsWith(TEXT("AM_DG_Attack")))
                    Anim->Montage_Stop(0.13f, Montage);
                ResetInt(*Fighter, TEXT("GunThirdPhase"));
            }
            else if (Role == Miyabi && ReadInt(*Fighter, TEXT("SC_Stage")) > 0)
            {
                Anim->Montage_StopGroupByName(0.13f, TEXT("MiyabiCombatGroup"));
                Movement->bOrientRotationToMovement = ReadBool(*Fighter, TEXT("SC_SavedOrient"));
                ResetInt(*Fighter, TEXT("SC_Stage"));
                WriteBool(*Fighter, TEXT("SC_Queued"), false);
            }
            else if (Role == Yousun)
            {
                Anim->StopSlotAnimation(0.13f, SniperSlot);
                if (auto* Weapon = SniperWeapon(*Fighter))
                    if (auto* WeaponAnim = Weapon->GetAnimInstance())
                        WeaponAnim->StopSlotAnimation(0.13f, SniperSlot);
            }
        }
    }
    Super::OnUnPossess();
}

void ATrioBotController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    ACharacter* Fighter = Cast<ACharacter>(GetPawn());
    if (!IsAlive(Fighter) || Fighter->IsPlayerControlled() || Role.IsNone())
        return;
    auto* Combat = GetWorld()->GetSubsystem<UTrioCombatWorld>();
    if (!Combat || !Combat->AreBotsEnabled())
        return;

    // Locomotion input must drive the capsule. Keep the attack montages' authored displacement.
    if (auto* Anim = Fighter->GetMesh()->GetAnimInstance(); Anim && Anim != ManagedAnim.Get())
    {
        if (auto* PreviousAnim = ManagedAnim.Get())
            PreviousAnim->SetRootMotionMode(PreviousRootMotion);
        ManagedAnim = Anim;
        PreviousRootMotion = Anim->RootMotionMode;
        Anim->SetRootMotionMode(ERootMotionMode::RootMotionFromMontagesOnly);
    }

    const double Now = GetWorld()->GetTimeSeconds();
    ACharacter* Existing = Target.Get();
    if (!IsAlive(Existing) || Existing->IsHidden())
        Existing = nullptr;

    bool bVisible = Existing && CanSee(*Fighter, *Existing);
    if (bVisible)
    {
        LostSightAt = -1;
        SearchPoint = Existing->GetActorLocation();
    }
    else
    {
        if (LostSightAt < 0)
            LostSightAt = Now;

        if (Now >= NextTargetCheck)
        {
            NextTargetCheck = Now + 0.25;
            // Engage anyone who comes into view while searching.
            if (auto* Seen = Combat->FindBotTarget(*Fighter, nullptr, false))
            {
                Target = Seen;
                Existing = Seen;
                ChargeStart = -1;
                LostSightAt = -1;
                SearchPoint = Seen->GetActorLocation();
                bVisible = true;
            }
            else if (!Existing || Now - LostSightAt >= LostSightDelay)
            {
                ACharacter* Alternative = Combat->FindBotTarget(*Fighter, Existing, true);
                if (Alternative)
                {
                    SearchPoint = Alternative->GetActorLocation();
                    Existing = Alternative;
                }
                if (Existing)
                {
                    const FVector Offset = FVector::ForwardVector.RotateAngleAxis(
                        FMath::FRandRange(0.f, 360.f), FVector::UpVector);
                    SearchPoint += Offset * FMath::FRandRange(140.0, 320.0);
                }
                Target = Existing;
                LostSightAt = Now;
            }
        }
    }

    if (!Existing || !Fighter->GetCharacterMovement()->IsMovingOnGround())
        return;

    const FVector ToTarget = (bVisible ? Existing->GetActorLocation() : SearchPoint) - Fighter->GetActorLocation();
    const double Distance = ToTarget.Size2D();
    const FRotator Facing = ToTarget.GetSafeNormal2D().Rotation();
    SetControlRotation(Facing);
    Fighter->SetActorRotation(FMath::RInterpTo(Fighter->GetActorRotation(), Facing, DeltaSeconds, 14.0f));

    if (Role == Guns)
        UpdateGuns(*Fighter, Now, Distance, bVisible);
    else if (Role == Miyabi)
        UpdateBlade(*Fighter, Now, Distance, bVisible);
    else if (Role == Yousun)
        UpdateSniper(*Fighter, Now, Distance, bVisible);

    const bool bSwinging = Role == Miyabi && ReadInt(*Fighter, TEXT("SC_Stage")) != 0;
    const auto* Anim = Fighter->GetMesh()->GetAnimInstance();
    const auto* ActiveMontage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
    // The locomotion graph also extracts root motion, even when the guns are idle.
    const bool bShooting = Role == Guns && ActiveMontage &&
        ActiveMontage->GetName().StartsWith(TEXT("AM_DG_Attack"));
    if (!bSwinging && !bShooting && ChargeStart < 0)
        MoveInCombat(*Fighter, ToTarget, Distance, DeltaSeconds, !bVisible);
    else
        Steer = FVector::ZeroVector;
}

void ATrioBotController::MoveInCombat(ACharacter& Fighter, FVector ToTarget, double Distance, float DeltaSeconds,
    bool bSearching)
{
    const double Now = GetWorld()->GetTimeSeconds();
    const double PreferredRange = Role == Miyabi ? 145.0 : Role == Guns ? 435.0 : 990.0;
    const double DesiredDistance = bSearching ? 0.0 : PreferredRange;
    if (Now >= NextStrafe)
    {
        StrafeSide = -StrafeSide;
        NextStrafe = Now + FMath::FRandRange(0.75, 1.55);
    }

    const FVector Forward = ToTarget.IsNearlyZero(1.0) ? Fighter.GetActorForwardVector() : ToTarget.GetSafeNormal2D();
    const FVector Side = FVector::CrossProduct(FVector::UpVector, Forward) * StrafeSide;
    const double Radial = FMath::Clamp((Distance - DesiredDistance) / 215.0, -0.82, 1.0);
    const double Lateral = bSearching ? 0.42 : Role == Miyabi && Radial > 0.65 ? 0.3 : 0.72;
    FVector Desired = (Forward * Radial + Side * Lateral).GetClampedToMaxSize(1.0);

    const auto* Capsule = Fighter.GetCapsuleComponent();
    const FVector Origin = Fighter.GetActorLocation();
    const FCollisionShape Shape = Capsule->GetCollisionShape(-5.0f);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioBotMove), false, &Fighter);
    FCollisionResponseParams Responses;
    Capsule->InitSweepCollisionParams(Query, Responses);
    const auto CanMove = [&](FVector Direction)
    {
        const FVector End = Origin + Direction * 135.0;
        if (GetWorld()->SweepTestByChannel(Origin, End, FQuat::Identity, Capsule->GetCollisionObjectType(), Shape, Query, Responses))
            return false;
        FHitResult Floor;
        return GetWorld()->LineTraceSingleByChannel(Floor, End, End - FVector(0, 0, 200),
            Capsule->GetCollisionObjectType(), Query, Responses) && Fighter.GetCharacterMovement()->IsWalkable(Floor);
    };
    if (!CanMove(Desired))
    {
        Desired = Side;
        if (!CanMove(Desired))
        {
            Desired = -Side;
            if (!CanMove(Desired))
                Desired = FVector::ZeroVector;
        }
    }

    Steer = FMath::VInterpTo(Steer, Desired, DeltaSeconds, 7.0f);
    Fighter.AddMovementInput(Steer.GetSafeNormal(), Steer.Size());
}

void ATrioBotController::UpdateGuns(ACharacter& Fighter, double Now, double Distance, bool bVisible)
{
    const auto* Anim = Fighter.GetMesh()->GetAnimInstance();
    UAnimMontage* Montage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
    const int32 Phase = ReadInt(Fighter, TEXT("GunThirdPhase"));
    if ((!bVisible || Distance > 1450.0) && (Phase == 1 || Phase == 2))
    {
        RunEvent(Fighter, TEXT("HandleGunFireReleased"));
        return;
    }
    if (!bVisible && Phase == 0 && Montage &&
        ((Montage->GetFName() == TEXT("AM_DG_Attack01") && Anim->Montage_GetPosition(Montage) > 0.22f) ||
         (Montage->GetFName() == TEXT("AM_DG_Attack02_BiliMatched") && Anim->Montage_GetPosition(Montage) > 0.34f)))
    {
        Fighter.GetMesh()->GetAnimInstance()->Montage_Stop(0.14f, Montage);
        NextAction = Now + 0.3;
    }
    if (!bVisible || Distance > 1450.0 || Now < NextAction || Phase == 1 || Phase == 2 || Phase == 3 ||
        !ReadBool(Fighter, TEXT("bGunMovementMode")))
        return;

    const FName Name = Montage ? Montage->GetFName() : NAME_None;
    if (Name == TEXT("AM_DG_Attack02_BiliMatched"))
    {
        if (!bRequestedSweep && ReadBool(Fighter, TEXT("GunThirdArmed")) &&
            Anim->Montage_GetPosition(Montage) >= 0.33f)
        {
            RunEvent(Fighter, TEXT("RequestGunThirdHold"));
            bRequestedSweep = true;
            NextAction = Now + 0.78;
        }
        // Slow combos recover early between bursts so there is room to reposition.
        else if (!ReadBool(Fighter, TEXT("GunThirdArmed")) && Anim->Montage_GetPosition(Montage) > 0.48f)
        {
            Fighter.GetMesh()->GetAnimInstance()->Montage_Stop(0.15f, Montage);
            NextAction = Now + FMath::FRandRange(0.34, 0.58);
        }
    }
    else if (!Montage || Name == TEXT("AM_DG_Attack01"))
    {
        if (!Montage)
            bRequestedSweep = false;
        RunEvent(Fighter, TEXT("HandleGunFirePressed"));
        NextAction = Now + (Montage ? 0.18 : 0.34);
    }
}

void ATrioBotController::UpdateBlade(ACharacter& Fighter, double Now, double Distance, bool bVisible)
{
    if (!bVisible || Now < NextAction)
        return;

    const int32 Stage = ReadInt(Fighter, TEXT("SC_Stage"));
    if (Stage == 0 && Distance <= 640.0)
    {
        RunEvent(Fighter, TEXT("TrioBotBladeAttack"));
        NextAction = Now + 0.16;
    }
    else if (Stage > 0 && Distance < 340.0 && ReadFloat(Fighter, TEXT("SC_Time")) > 0.15 &&
        !ReadBool(Fighter, TEXT("SC_Queued")))
    {
        RunEvent(Fighter, TEXT("TrioBotBladeAttack"));
        NextAction = Now + FMath::FRandRange(0.11, 0.17);
    }
}

void ATrioBotController::UpdateSniper(ACharacter& Fighter, double Now, double Distance, bool bVisible)
{
    if (!bVisible || Distance < 260.0 || Distance > 2300.0)
    {
        if (ChargeStart >= 0)
        {
            ChargeStart = -1;
            NextShot = Now + 0.28;
            if (auto* Anim = Fighter.GetMesh()->GetAnimInstance())
                Anim->StopSlotAnimation(0.16f, SniperSlot);
            if (auto* Weapon = SniperWeapon(Fighter))
                if (auto* Anim = Weapon->GetAnimInstance())
                    Anim->StopSlotAnimation(0.16f, SniperSlot);
        }
        return;
    }
    if (ChargeStart < 0 && Now >= FMath::Max(NextShot, NextAction))
    {
        ChargeStart = Now;
        FireAt = Now + FMath::FRandRange(0.78, 1.35);
        Fighter.GetCharacterMovement()->StopMovementImmediately();
        PlaySniperAnimations(Fighter, false);
    }
    else if (ChargeStart >= 0 && Now >= FireAt)
    {
        const double Charge = FMath::Clamp((Now - ChargeStart) * 0.30, 0.0, 1.0);
        ChargeStart = -1;
        NextShot = Now + FMath::FRandRange(1.15, 1.75);
        PlaySniperAnimations(Fighter, true);
        FireSniper(Fighter, *Target.Get(), Charge);
    }
}

void ATrioBotController::PlaySniperAnimations(ACharacter& Fighter, bool bFiring)
{
    static const TCHAR* BodyPaths[] =
    {
        TEXT("/Game/character/yousun/Combat/Animations/Body_Aim.Body_Aim"),
        TEXT("/Game/character/yousun/Combat/Animations/Body_Fire.Body_Fire")
    };
    static const TCHAR* WeaponPaths[] =
    {
        TEXT("/Game/character/yousun/Combat/Animations/Weapon_Aim.Weapon_Aim"),
        TEXT("/Game/character/yousun/Combat/Animations/Weapon_Fire.Weapon_Fire")
    };
    const int32 Index = bFiring ? 1 : 0;
    const auto Play = [](USkeletalMeshComponent* Mesh, const TCHAR* Path)
    {
        if (auto* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr)
            if (auto* Sequence = LoadObject<UAnimSequenceBase>(nullptr, Path))
                Anim->PlaySlotAnimationAsDynamicMontage(Sequence, SniperSlot, 0.08f, 0.2f);
    };
    Play(Fighter.GetMesh(), BodyPaths[Index]);
    Play(SniperWeapon(Fighter), WeaponPaths[Index]);
}

void ATrioBotController::FireGunShot(FVector Muzzle, bool bSweep)
{
    auto* Fighter = Cast<ACharacter>(GetPawn());
    ACharacter* AimTarget = Target.Get();
    if (!IsAlive(Fighter) || !IsAlive(AimTarget) || !CanSee(*Fighter, *AimTarget))
        return;

    FHitResult Hit;
    if (!FireTrace(*Fighter, Muzzle, Chest(*AimTarget), bSweep ? 3.6 : 1.6, Hit))
        return;
    if (auto* Impact = LoadObject<UNiagaraSystem>(nullptr,
        TEXT("/Game/Shooter_VFXPack/Particles/HitsAndExplosions/P_Hit_Fire.P_Hit_Fire")))
        UNiagaraFunctionLibrary::SpawnSystemAtLocation(Fighter, Impact, Hit.ImpactPoint, Hit.ImpactNormal.Rotation());
    UTrioCombatLibrary::ResolveGunHit(Fighter, Hit, bSweep);
}

void ATrioBotController::FireSniper(ACharacter& Fighter, const ACharacter& AimTarget, double Charge)
{
    auto* Weapon = SniperWeapon(Fighter);
    if (!Weapon || !Weapon->DoesSocketExist(TEXT("Ctr_Weapon_01")))
        return;
    const FVector Muzzle = Weapon->GetSocketLocation(TEXT("Ctr_Weapon_01"));
    const FVector Aim = Chest(AimTarget) + AimTarget.GetVelocity() * 0.045;
    FHitResult Hit;
    const bool bHit = FireTrace(Fighter, Muzzle, Aim, 1.15, Hit);
    PlaySniperVFX(Fighter, Weapon->GetSocketTransform(TEXT("Ctr_Weapon_04")).TransformPosition(
        FVector(-0.01734, -12.06113, -5.14991)), Aim, Hit);
    if (bHit)
    {
        if (auto* Receiver = Fighter.FindComponentByClass<UTrioCombatReceiver>())
            Receiver->LockedCharge = Charge;
        UTrioCombatLibrary::ResolveSniperHit(&Fighter, Hit);
    }
}
