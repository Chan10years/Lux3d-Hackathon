#include "TrioCombat.h"
#include "TrioCityHUD.h"
#include "TrioCombatHUD.h"
#include "TrioGroundCamera.h"
#include "Camera/PlayerCameraManager.h"
#include "TrioBotController.h"
#include "Blueprint/UserWidget.h"
#include "Components/Image.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "TimerManager.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, TrioCombat)

namespace
{
    const FName Guns(TEXT("Guns"));
    const FName Miyabi(TEXT("Miyabi"));
    const FName Yousun(TEXT("Yousun"));
    const FName SniperLock(TEXT("TrioSniperCharge"));

    bool HasClass(const AActor* Actor, const TCHAR* Path)
    {
        for (const UClass* Class = Actor ? Actor->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
        {
            if (Class->GetPathName() == Path)
                return true;
        }
        return false;
    }

    bool IsHeadBone(const USkeletalMeshComponent& Mesh, FName Bone)
    {
        for (int32 Depth = 0; !Bone.IsNone() && Depth < 32; ++Depth)
        {
            const FString Name = Bone.ToString().ToLower();
            if (Name == TEXT("head") || Name.EndsWith(TEXT(":head")) || Name.EndsWith(TEXT("_head")) ||
                Name == TEXT("bip001 head") || Name == TEXT("bip001-head"))
                return true;
            Bone = Mesh.GetParentBone(Bone);
        }
        return false;
    }

    int32 ReadInt(const AActor* Actor, FName Name)
    {
        const auto* Property = Actor ? FindFProperty<FIntProperty>(Actor->GetClass(), Name) : nullptr;
        return Property ? Property->GetPropertyValue_InContainer(Actor) : 0;
    }

    double ReadReal(const AActor* Actor, FName Name)
    {
        if (const auto* Value = FindFProperty<FDoubleProperty>(Actor->GetClass(), Name))
            return Value->GetPropertyValue_InContainer(Actor);
        if (const auto* Value = FindFProperty<FFloatProperty>(Actor->GetClass(), Name))
            return Value->GetPropertyValue_InContainer(Actor);
        return 0;
    }

    bool IsGunHeadshot(const AActor* Attacker, const FHitResult& Hit, FVector& Point)
    {
        Point = Hit.ImpactPoint;
        const ACharacter* Target = Cast<ACharacter>(Hit.GetActor());
        USkeletalMeshComponent* Mesh = Target ? Target->GetMesh() : nullptr;
        if (!Mesh || !Attacker)
            return false;
        if (IsHeadBone(*Mesh, Hit.BoneName))
            return true;

        const FVector Direction = (Hit.TraceEnd - Hit.TraceStart).GetSafeNormal();
        FVector End = Point + Direction * 150.0;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioHead), false, Attacker);
        Query.AddIgnoredActor(Target);
        FHitResult Wall;
        if (Target->GetWorld()->LineTraceSingleByChannel(Wall, Point, End, ECC_Visibility, Query))
            End = Wall.ImpactPoint;

        FHitResult MeshHit;
        if (Mesh->LineTraceComponent(MeshHit, Hit.TraceStart, End, Query) && !MeshHit.BoneName.IsNone())
        {
            if (!IsHeadBone(*Mesh, MeshHit.BoneName))
                return false;
            Point = MeshHit.ImpactPoint;
            return true;
        }

        // Some meshes have no query-enabled head body. Test a sphere bound to their head bone.
        for (const FName Bone : {FName(TEXT("head")), FName(TEXT("Bip001-Head"))})
        {
            if (Mesh->GetBoneIndex(Bone) == INDEX_NONE)
                continue;
            const FVector Head = Mesh->GetBoneLocation(Bone) + FVector(0, 0, 8);
            const FVector Closest = FMath::ClosestPointOnSegment(Head, Point, End);
            if (FVector::DistSquared(Head, Closest) <= FMath::Square(11.0))
            {
                Point = Closest;
                return true;
            }
        }
        return false;
    }

    const AActor* ScopeOwner(const AActor* Actor)
    {
        const FBoolProperty* Active = Actor ? FindFProperty<FBoolProperty>(Actor->GetClass(), TEXT("YS_ScopeActive")) : nullptr;
        return Active && Active->GetPropertyValue_InContainer(Actor) ? Actor : nullptr;
    }
}

void FTrioVitals::RestoreShield(const FTrioCombatantRow& Stats, double Seconds)
{
    if (Health > 0 && Shield < Stats.MaxShield)
        Shield = FMath::Min(Stats.MaxShield, Shield + Stats.ShieldPerSecond * Seconds);
}

void UTrioCombatReceiver::BeginPlay()
{
    Super::BeginPlay();
    LocalVitals.Health = Stats.MaxHealth;
    LocalVitals.Shield = Stats.MaxShield;
    GetOwner()->OnTakeAnyDamage.AddUniqueDynamic(this, &UTrioCombatReceiver::OnEngineDamage);
}

FTrioVitals& UTrioCombatReceiver::MutableVitals()
{
    if (UWorld* World = GetWorld())
        if (auto* Combat = World->GetSubsystem<UTrioCombatWorld>())
            return Combat->VitalsFor(GetOwner(), CharacterId, LocalVitals);
    return LocalVitals;
}

FTrioVitals UTrioCombatReceiver::GetVitals() const
{
    return const_cast<UTrioCombatReceiver*>(this)->MutableVitals();
}

FTrioHitResult UTrioCombatReceiver::ConsumeDamage(FTrioHitResult Result, const AActor* Attacker)
{
    if (GetWorld() && GetWorld()->GetNetMode() != NM_Standalone)
        return {};
    FTrioVitals& Vitals = MutableVitals();
    if (!FMath::IsFinite(Result.Damage) || Result.Damage <= 0 || Vitals.Health <= 0 || !GetOwner()->CanBeDamaged())
        return {};

    Result.ShieldAbsorbed = FMath::Min(Result.Damage, Vitals.Shield);
    Vitals.Shield -= Result.ShieldAbsorbed;
    Result.HealthLost = FMath::Min(Result.Damage - Result.ShieldAbsorbed, Vitals.Health);
    Vitals.Health -= Result.HealthLost;
    if (Vitals.Health <= 0)
        BeginDeath(Attacker);
    return Result;
}

void UTrioCombatReceiver::OnEngineDamage(AActor*, float Damage, const UDamageType*, AController*, AActor* Causer)
{
    FTrioHitResult Result;
    Result.Damage = Damage;
    ConsumeDamage(Result, Causer);
}

bool UTrioCombatWorld::DoesSupportWorldType(EWorldType::Type Type) const
{
    return Type == EWorldType::Game || Type == EWorldType::PIE;
}

void UTrioCombatWorld::OnWorldBeginPlay(UWorld& World)
{
    Super::OnWorldBeginPlay(World);
    Combatants = LoadObject<UDataTable>(nullptr, TEXT("/TrioCombat/DT_Combatants.DT_Combatants"));
    Attacks = LoadObject<UDataTable>(nullptr, TEXT("/TrioCombat/DT_Attacks.DT_Attacks"));
    if (!ensure(Combatants && Attacks))
        return;

    for (FName Id : {Guns, Miyabi, Yousun})
    {
        if (const FTrioCombatantRow* Stats = FindStats(Id))
        {
            FTrioVitals& Vitals = Roster.Add(Id);
            Vitals.Health = Stats->MaxHealth;
            Vitals.Shield = Stats->MaxShield;
        }
    }
    for (TActorIterator<APawn> Actor(&World); Actor; ++Actor)
        OnSpawn(*Actor);
    SpawnHandle = World.AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(this, &UTrioCombatWorld::OnSpawn));
    PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UTrioCombatWorld::FlushGunShots);
}

void UTrioCombatWorld::OnSpawn(AActor* Actor)
{
    if (!Identify(Actor).IsNone() || Actor->FindComponentByClass<UTrioCombatReceiver>())
        Receiver(Actor);
}

FName UTrioCombatWorld::Identify(const AActor* Actor) const
{
    if (HasClass(Actor, TEXT("/Game/character/Miyabi/Player/BP_Miyabi.BP_Miyabi_C")))
        return Miyabi;
    if (HasClass(Actor, TEXT("/Game/character/yousun/Movement/BP_Yousun.BP_Yousun_C")))
        return Yousun;
    if (Actor && Actor->GetClass()->GetPathName() == TEXT("/Game/baseAdvRootMotionSys_BP/BP_MyCharacter.BP_MyCharacter_C"))
        return Guns;
    return NAME_None;
}

UTrioCombatReceiver* UTrioCombatWorld::Receiver(AActor* Actor)
{
    if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
        return nullptr;
    UTrioCombatReceiver* Component = Actor->FindComponentByClass<UTrioCombatReceiver>();
    if (!Component)
    {
        const FName Id = Identify(Actor);
        const FTrioCombatantRow* Stats = FindStats(Id);
        if (!Stats)
            return nullptr;
        Component = NewObject<UTrioCombatReceiver>(Actor, TEXT("TrioCombatReceiver"));
        Component->CharacterId = Id;
        Component->Stats = *Stats;
        Actor->AddInstanceComponent(Component);
        Component->RegisterComponent();
    }
    Receivers.AddUnique(Component);
    return Component;
}

FTrioVitals& UTrioCombatWorld::VitalsFor(AActor* Actor, FName Id, FTrioVitals& Local)
{
    if (FTrioVitals* Stored = Roster.Find(Id))
    {
        TWeakObjectPtr<AActor>& Owner = RosterActors.FindOrAdd(Id);
        if (!Owner.IsValid() || Owner.Get() == Actor)
        {
            Owner = Actor;
            return *Stored;
        }
    }
    return Local;
}

const FTrioCombatantRow* UTrioCombatWorld::FindStats(FName Id) const
{
    return Combatants && !Id.IsNone() ? Combatants->FindRow<FTrioCombatantRow>(Id, TEXT("Combat"), false) : nullptr;
}

const FTrioAttackRow* UTrioCombatWorld::FindAttack(FName Id) const
{
    return Attacks ? Attacks->FindRow<FTrioAttackRow>(Id, TEXT("Combat"), false) : nullptr;
}

void UTrioCombatWorld::Tick(float Seconds)
{
    for (auto& Pair : Roster)
        if (const FTrioCombatantRow* Stats = FindStats(Pair.Key))
            Pair.Value.RestoreShield(*Stats, Seconds);

    Receivers.RemoveAll([](const auto& Receiver) { return !Receiver.IsValid(); });
    for (const auto& Weak : Receivers)
    {
        auto* Component = Weak.Get();
        if (Component->CharacterId == Guns && !Component->IsDefeated())
            Component->UpdateGunSweep(ReadInt(Component->GetOwner(), TEXT("GunThirdPhase")), GetWorld()->GetTimeSeconds());
        FTrioVitals& Vitals = Component->MutableVitals();
        if (&Vitals != Roster.Find(Component->CharacterId))
            Vitals.RestoreShield(Component->Stats, Seconds);
    }

    UpdateBladeSteps();

    APlayerController* Player = GetWorld()->GetFirstPlayerController();
    if (!Player || !Player->IsLocalController())
        return;
    if (APlayerCameraManager* CameraManager = Player->PlayerCameraManager; CameraManager &&
        !CameraManager->FindCameraModifierByClass(UTrioGroundCamera::StaticClass()))
    {
        CameraManager->AddNewCameraModifier(UTrioGroundCamera::StaticClass());
    }
    const bool bStandalone = GetWorld()->GetNetMode() == NM_Standalone;
    if (bStandalone)
    {
        SelectSurvivingPawn(*Player);
        UpdateBots(*Player);
    }
    if (!CityHUD && Player->GetPawn() && UTrioCityHUD::Supports(*GetWorld()))
    {
        CityHUD = CreateWidget<UTrioCityHUD>(Player, UTrioCityHUD::StaticClass());
        if (CityHUD)
            CityHUD->AddToViewport(80);
    }
    if (CityHUD)
        CityHUD->Update(*Player);
    if (bStandalone)
    {
        if (!HUD && !RosterActors.IsEmpty())
        {
            HUD = CreateWidget<UTrioCombatHUD>(Player, UTrioCombatHUD::StaticClass());
            if (HUD)
                HUD->AddToViewport(75);
        }
        if (HUD)
            HUD->Update(*this, *Player, Seconds);
    }
}

void UTrioCombatWorld::SelectSurvivingPawn(APlayerController& Player)
{
    APawn* Pawn = Player.GetPawn();
    const auto* Current = Pawn ? Pawn->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
    if (!Current || !Current->IsDefeated())
        return;

    for (FName Id : {Guns, Miyabi, Yousun})
    {
        const auto* Candidate = RosterActors.Find(Id);
        APawn* Survivor = Candidate ? Cast<APawn>(Candidate->Get()) : nullptr;
        const auto* Vitals = FindVitals(Id);
        if (Survivor && Survivor != Pawn && Vitals && Vitals->Health > 0 &&
            (!Survivor->GetController() || Survivor->GetController()->IsA<ATrioBotController>()))
        {
            Player.Possess(Survivor);
            return;
        }
    }
}

void UTrioCombatWorld::ShowDamage(AActor* Attacker, AActor* Target, FVector Location, const FTrioHitResult& Result)
{
    const auto* Source = Cast<APawn>(Attacker);
    const auto* ReceiverPawn = Cast<APawn>(Target);
    if (HUD && ((Source && Source->IsPlayerControlled()) || (ReceiverPawn && ReceiverPawn->IsPlayerControlled())))
        HUD->AddDamage(Location, Result);
}

TStatId UTrioCombatWorld::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTrioCombatWorld, STATGROUP_Tickables);
}

void UTrioCombatWorld::Deinitialize()
{
    FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
    PendingGunShots.Reset();
    BladeSteps.Reset();
    StopBots();
    if (HUD)
        HUD->RemoveFromParent();
    if (CityHUD)
        CityHUD->RemoveFromParent();
    if (SpawnHandle.IsValid())
        GetWorld()->RemoveOnActorSpawnedHandler(SpawnHandle);
    Super::Deinitialize();
}

FTrioHitResult UTrioCombatLibrary::Calculate(const FTrioCombatantRow& Stats, const FTrioAttackRow& Attack,
    bool bHeadshot, double Charge, double CritRoll)
{
    FTrioHitResult Result;
    Result.bCritical = CritRoll < Stats.CritChance;
    Result.bHeadshot = bHeadshot && Stats.HeadMultiplier > 1;
    Result.Damage = Stats.Attack * FMath::Lerp(Attack.Multiplier, Attack.FullChargeMultiplier, FMath::Clamp(Charge, 0.0, 1.0))
        * (Result.bHeadshot ? Stats.HeadMultiplier : 1) * (Result.bCritical ? Stats.CritMultiplier : 1);
    return Result;
}

FTrioHitResult UTrioCombatLibrary::ResolveHit(AActor* Attacker, FName Attack, AActor* Target, FVector Location,
    bool bHeadshot, double Charge)
{
    if (!IsValid(Attacker) || !IsValid(Target) || Attacker == Target || Attacker->GetWorld() != Target->GetWorld())
        return {};
    auto* Combat = Attacker->GetWorld()->GetSubsystem<UTrioCombatWorld>();
    if (!Combat)
        return {};
    const auto* Move = Combat->FindAttack(Attack);
    const auto* Stats = Move && Move->Character == Combat->Identify(Attacker) ? Combat->FindStats(Move->Character) : nullptr;
    auto* TargetReceiver = Stats ? Combat->Receiver(Target) : nullptr;
    auto* AttackerReceiver = Stats ? Combat->Receiver(Attacker) : nullptr;
    if (!TargetReceiver || !AttackerReceiver || AttackerReceiver->GetVitals().Health <= 0)
        return {};
    FTrioHitResult Result = TargetReceiver->ConsumeDamage(Calculate(*Stats, *Move, bHeadshot, Charge, FMath::FRand()), Attacker);
    if (Result.Damage > 0)
        Combat->ShowDamage(Attacker, Target, Location, Result);
    return Result;
}

void UTrioCombatReceiver::UpdateGunSweep(int32 GunThirdPhase, double Now)
{
    const bool bActive = GunThirdPhase == 1 || GunThirdPhase == 2;
    if (bActive && !bGunSweepActive)
        GunSweepShotsFired = 0;
    else if (!bActive && bGunSweepActive)
        GunSweepMeterUntil = Now + 0.95;
    bGunSweepActive = bActive;
}

bool UTrioCombatReceiver::AcceptGunSweepShot(int32 GunThirdPhase, double Now)
{
    UpdateGunSweep(GunThirdPhase, Now);
    if (GunThirdPhase != 2 || GunSweepShotsFired >= MaxGunSweepShots)
        return false;
    ++GunSweepShotsFired;
    return true;
}

bool UTrioCombatWorld::QueueGunShot(AActor* Shooter, FVector MuzzleLocation, bool bSweep)
{
    auto* Source = Shooter ? Shooter->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
    if (!Source || Source->IsDefeated() || Identify(Shooter) != Guns)
        return false;
    if (bSweep && !Source->AcceptGunSweepShot(ReadInt(Shooter, TEXT("GunThirdPhase")), GetWorld()->GetTimeSeconds()))
        return false;
    const APawn* Pawn = Cast<APawn>(Shooter);
    const bool bBot = Pawn && Pawn->GetController<ATrioBotController>();
    PendingGunShots.Add({Shooter, MuzzleLocation, bSweep && Source->GunSweepShotsFired == UTrioCombatReceiver::MaxGunSweepShots, bBot});
    return true;
}

void UTrioCombatWorld::FlushGunShots(UWorld* World, ELevelTick, float)
{
    if (World != GetWorld() || PendingGunShots.IsEmpty())
        return;

    // Fire after the frame's animation and player camera updates, once per muzzle notification.
    const TArray<FGunShot> Shots = MoveTemp(PendingGunShots);
    for (const FGunShot& Shot : Shots)
    {
        AActor* Shooter = Shot.Shooter.Get();
        if (World->GetNetMode() != NM_Standalone)
        {
            const APawn* ShooterPawn = Cast<APawn>(Shooter);
            if (!ShooterPawn || !ShooterPawn->IsLocallyControlled())
                continue;
        }
        const auto* Source = Shooter ? Shooter->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
        UFunction* Fire = Source && !Source->IsDefeated() ? Shooter->FindFunction(TEXT("Player1FireHit")) : nullptr;
        const auto* Muzzle = Fire ? FindFProperty<FStructProperty>(Fire, TEXT("MuzzleLocation")) : nullptr;
        if (Fire && Shot.bBotShot)
        {
            auto* Pawn = Cast<APawn>(Shooter);
            if (auto* Bot = Pawn ? Pawn->GetController<ATrioBotController>() : nullptr)
                Bot->FireGunShot(Shot.MuzzleLocation, ReadInt(Shooter, TEXT("GunThirdPhase")) == 2);
        }
        else if (Muzzle && Muzzle->Struct == TBaseStructure<FVector>::Get())
        {
            FStructOnScope Parameters(Fire);
            *Muzzle->ContainerPtrToValuePtr<FVector>(Parameters.GetStructMemory()) = Shot.MuzzleLocation;
            Shooter->ProcessEvent(Fire, Parameters.GetStructMemory());
        }
        // Close the held-fire input and play the existing recovery outside the notify dispatch.
        if (Fire && Shot.bEndsSweep && IsValid(Shooter) && ReadInt(Shooter, TEXT("GunThirdPhase")) == 2)
        {
            if (UFunction* Release = Shooter->FindFunction(TEXT("HandleGunFireReleased")))
                Shooter->ProcessEvent(Release, nullptr);
            if (ReadInt(Shooter, TEXT("GunThirdPhase")) == 2)
                if (UFunction* End = Shooter->FindFunction(TEXT("EndGunThirdAttack")))
                    Shooter->ProcessEvent(End, nullptr);
        }
    }
}

void UTrioCombatLibrary::ResolveGunHit(AActor* DamageCauser, const FHitResult& HitInfo, bool bSweep)
{
    FVector Point;
    const bool bHead = IsGunHeadshot(DamageCauser, HitInfo, Point);
    const bool bThird = bSweep || ReadInt(DamageCauser, TEXT("GunThirdPhase")) > 0;
    ResolveHit(DamageCauser, bThird ? TEXT("GunSweep") : TEXT("GunSingle"), HitInfo.GetActor(), Point, bHead);
}

void UTrioCombatLibrary::ResolveSniperHit(AActor* DamageCauser, const FHitResult& HitInfo)
{
    FVector Point;
    const bool bHead = IsGunHeadshot(DamageCauser, HitInfo, Point);
    ResolveHit(DamageCauser, TEXT("Sniper"), HitInfo.GetActor(), Point, bHead, ScopeCharge(DamageCauser, true));
}

double UTrioCombatLibrary::ScopeCharge(const AActor* Actor, bool bUseLocked)
{
    if (!Actor)
        return 0;
    if (bUseLocked)
    {
        if (const auto* Store = Actor->FindComponentByClass<UTrioCombatReceiver>())
        {
            // A shot's captured charge lives on the shooting pawn, not in its persistent vitals.
            return Store->LockedCharge;
        }
        return 0;
    }
    if (!ScopeOwner(Actor))
        return 0;
    const auto* Scope = FindFProperty<FObjectPropertyBase>(Actor->GetClass(), TEXT("YS_ScopeWidget"));
    const auto* Widget = Scope ? Cast<UUserWidget>(Scope->GetObjectPropertyValue_InContainer(Actor)) : nullptr;
    auto* Overlay = Widget ? Cast<UImage>(Widget->GetWidgetFromName(TEXT("ChargeOverlay"))) : nullptr;
    auto* Material = Overlay ? Overlay->GetDynamicMaterial() : nullptr;
    float Start = 0;
    if (!Material || !Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("AimStart")), Start))
        return 0;
    return FMath::Clamp((Actor->GetWorld()->GetTimeSeconds() - Start) * 0.30, 0.0, 1.0);
}

void UTrioCombatLibrary::LockSniperCharge(AActor* Attacker)
{
    if (Attacker)
        if (auto* Combat = Attacker->GetWorld()->GetSubsystem<UTrioCombatWorld>())
            if (auto* Component = Combat->Receiver(Attacker))
                Component->LockedCharge = ScopeCharge(Attacker, false);
}

void UTrioCombatLibrary::ResolveBladeHit(AActor* DamageCauser, const FHitResult& HitInfo, FName Attack)
{
    if (Attack != TEXT("BladeRift"))
    {
        ResolveHit(DamageCauser, Attack, HitInfo.GetActor(), HitInfo.ImpactPoint);
        return;
    }
    if (!IsValid(DamageCauser) || !HitInfo.bBlockingHit)
        return;

    // The five Action05 Niagara emitters burst at these delays after the first cut.
    constexpr float Delays[] = {0.0f, 0.10f, 0.20f, 0.35f, 0.40f};
    const TWeakObjectPtr<AActor> Attacker(DamageCauser);
    const TWeakObjectPtr<UPrimitiveComponent> Surface(HitInfo.GetComponent());
    const FVector LocalPoint = Surface.IsValid() ? Surface->GetComponentTransform().InverseTransformPosition(HitInfo.ImpactPoint) : FVector(HitInfo.ImpactPoint);
    const auto Pulse = [Attacker, Surface, LocalPoint, HitInfo]()
    {
        if (!Attacker.IsValid())
            return;
        const FVector Point = Surface.IsValid() ? Surface->GetComponentTransform().TransformPosition(LocalPoint) : FVector(HitInfo.ImpactPoint);
        FHitResult Contact = HitInfo;
        Contact.ImpactPoint = Point;
        ResolveBladePulse(Attacker.Get(), Contact, Point, TEXT("BladeRift"), 68.0f);
    };
    Pulse();
    for (int32 Index = 1; Index < UE_ARRAY_COUNT(Delays); ++Index)
    {
        FTimerHandle Timer;
        DamageCauser->GetWorldTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(DamageCauser, Pulse), Delays[Index], false);
    }
}

void UTrioCombatLibrary::StartBladeAfterimage(AActor* Attacker)
{
    if (!IsValid(Attacker))
        return;
    const int32 Sequence = ReadInt(Attacker, TEXT("SC_AttacksStarted"));
    const float Delay = FMath::Max(0.001, ReadReal(Attacker, TEXT("SC_Hit5")) - 0.20);
    const TWeakObjectPtr<AActor> Source(Attacker);
    FTimerHandle Timer;
    Attacker->GetWorldTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(Attacker, [Source, Sequence]()
    {
        AActor* Actor = Source.Get();
        if (!Actor || ReadInt(Actor, TEXT("SC_Stage")) != 5 || ReadInt(Actor, TEXT("SC_AttacksStarted")) != Sequence)
            return;
        const FVector Center = Actor->GetActorLocation() + Actor->GetActorForwardVector() * 130;
        UClass* Afterimage = LoadClass<AActor>(nullptr, TEXT("/Game/character/Miyabi/VFX/FrameMatched/BP_FrameAfterimage.BP_FrameAfterimage_C"));
        if (Afterimage)
        {
            FActorSpawnParameters Spawn;
            Spawn.Owner = Actor;
            Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            const auto* Character = Cast<ACharacter>(Actor);
            FTransform Transform = Character ? Character->GetMesh()->GetComponentTransform() : Actor->GetActorTransform();
            Transform.AddToTranslation(Actor->GetActorForwardVector() * 78);
            Actor->GetWorld()->SpawnActor<AActor>(Afterimage, Transform, Spawn);
        }
        ResolveBladePulse(Actor, {}, Center, TEXT("BladeAfterimage"), 112.0f);
    }), Delay, false);
}

void UTrioCombatLibrary::ResolveBladePulse(AActor* Attacker, const FHitResult& Contact, FVector Center, FName Attack, float Radius)
{
    if (!IsValid(Attacker) || Radius <= 0)
        return;
    UWorld* World = Attacker->GetWorld();
    TArray<FOverlapResult> Hits;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(TrioBlade), false, Attacker);
    TSet<AActor*> Resolved;
    if (Contact.bBlockingHit && Contact.GetActor() && FVector::DistSquared(Contact.ImpactPoint, Center) <= FMath::Square(Radius))
    {
        Resolved.Add(Contact.GetActor());
        ResolveHit(Attacker, Attack, Contact.GetActor(), Contact.ImpactPoint);
    }
    World->OverlapMultiByObjectType(Hits, Center, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
        FCollisionShape::MakeSphere(Radius), Query);
    for (const FOverlapResult& Overlap : Hits)
    {
        AActor* Target = Overlap.GetActor();
        if (!IsValid(Target) || Resolved.Contains(Target))
            continue;
        Resolved.Add(Target);
        FHitResult Obstruction;
        if (!World->LineTraceSingleByChannel(Obstruction, Center, Target->GetActorLocation(), ECC_Visibility, Query)
            || Obstruction.GetActor() == Target)
            ResolveHit(Attacker, Attack, Target, Obstruction.bBlockingHit ? Obstruction.ImpactPoint : Target->GetActorLocation());
    }
}

