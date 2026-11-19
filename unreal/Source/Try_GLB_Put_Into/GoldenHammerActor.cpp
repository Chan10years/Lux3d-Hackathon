#include "GoldenHammerActor.h"
#include "BeaconProceduralMesh.h"
#include "CombatMatchActor.h"
#include "CombatRules.h"
#include "PlayerCombatComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "ProceduralMeshComponent.h"

AGoldenHammerActor::AGoldenHammerActor()
{
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("ObjectiveRoot")));
    bReplicates = true;
    bAlwaysRelevant = true;
    SetReplicateMovement(false);
    SetActorEnableCollision(false);
    NetUpdateFrequency = 10;
    PrimaryActorTick.bCanEverTick = true;
}

void AGoldenHammerActor::BeginPlay()
{
    Super::BeginPlay();
    Tags.AddUnique(TEXT("BeaconObjective"));
    CreateVisual();
}

void AGoldenHammerActor::EndPlay(const EEndPlayReason::Type Reason)
{
    Super::EndPlay(Reason);
}

void AGoldenHammerActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AGoldenHammerActor, Holder);
    DOREPLIFETIME(AGoldenHammerActor, GroundPosition);
    DOREPLIFETIME(AGoldenHammerActor, MapPosition);
}

ACharacter* AGoldenHammerActor::GetHolder() const { return IsValid(Holder) ? Holder.Get() : nullptr; }
APlayerState* AGoldenHammerActor::GetHolderState() const { return GetHolder() ? GetHolder()->GetPlayerState() : nullptr; }

bool AGoldenHammerActor::FindGround(const FVector& Desired, FVector& Out) const
{
    if (Desired.ContainsNaN() || Desired.Z < GetWorld()->GetWorldSettings()->KillZ + 150) return false;
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic);
    Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
    FCollisionQueryParams Params(SCENE_QUERY_STAT(GoldGround), false, this);
    for (int32 Try = 0; Try < 9; ++Try)
    {
        const float Angle = Try * PI / 4;
        const FVector Offset = Try == 0 ? FVector::ZeroVector : FVector(FMath::Cos(Angle) * 180, FMath::Sin(Angle) * 180, 0);
        const FVector Base = Desired + Offset;
        FHitResult Hit;
        if (GetWorld()->LineTraceSingleByObjectType(Hit, Base + FVector(0, 0, 150), Base - FVector(0, 0, 1200), Objects, Params)
            && Hit.ImpactNormal.Z >= .6f && Hit.ImpactPoint.Z > GetWorld()->GetWorldSettings()->KillZ + 150
            && Hit.ImpactPoint.Z <= Desired.Z + 20)
        {
            // Centre the visual above the floor, with enough empty space for the objective.
            Out = Hit.ImpactPoint + FVector(0, 0, 65);
            if (!GetWorld()->OverlapBlockingTestByChannel(Out, FQuat::Identity, ECC_WorldStatic,
                FCollisionShape::MakeSphere(35), Params)) return true;
        }
    }
    return false;
}

void AGoldenHammerActor::InitializeSpawn(const FVector& Desired)
{
    if (!HasAuthority()) return;
    SpawnPosition = GetActorLocation() + FVector(0, 0, 65);
    FVector Found;
    if (FindGround(Desired, Found) || FindGround(GetActorLocation(), Found)) SpawnPosition = Found;
    ResetToSpawn();
}

void AGoldenHammerActor::ResetToSpawn()
{
    if (!HasAuthority()) return;
    if (auto* Match = ACombatMatchActor::Find(GetWorld())) Match->ChangeHolder(nullptr);
    Holder = nullptr;
    GroundPosition = SpawnPosition;
    MapPosition = GroundPosition;
    PickupAllowedAt = GetWorld()->GetTimeSeconds() + .5;
    ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("[Beacon] RESET position=%s"), *SpawnPosition.ToString());
}

void AGoldenHammerActor::DropFromHolder()
{
    if (!HasAuthority() || !Holder) return;
    const FVector Desired = GetHolder() ? GetHolder()->GetActorLocation() : LastHolderPosition;
    if (auto* Match = ACombatMatchActor::Find(GetWorld())) Match->ChangeHolder(nullptr);
    Holder = nullptr;
    FVector Found;
    const bool SafeDrop = FindGround(Desired, Found) && CombatRules::ObjectiveDropInBounds(
        FVector::DistSquared2D(Found, SpawnPosition), Found.Z - SpawnPosition.Z);
    GroundPosition = SafeDrop ? Found : SpawnPosition;
    MapPosition = GroundPosition;
    PickupAllowedAt = GetWorld()->GetTimeSeconds() + .5;
    ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("[Beacon] DROP position=%s"), *FVector(GroundPosition).ToString());
}

void AGoldenHammerActor::CreateVisual()
{
    if (!FApp::CanEverRender() || CrystalPivot || !GetRootComponent()) return;

    UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(
        nullptr, TEXT("/Game/LevelPrototyping/Interactable/JumpPad/Assets/Materials/M_SimpleGlow.M_SimpleGlow"));
    if (!BaseMaterial)
    {
        BaseMaterial = LoadObject<UMaterialInterface>(
            nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    }

    if (BaseMaterial)
    {
        CrystalMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        RingMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
        PillarMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, this);
    }

    CrystalPivot = NewObject<USceneComponent>(this, TEXT("CrystalPivot"));
    CrystalPivot->SetupAttachment(GetRootComponent());
    AddInstanceComponent(CrystalPivot);
    CrystalPivot->RegisterComponent();

    auto MakeProcMesh = [&](const TCHAR* Name, USceneComponent* Parent, UMaterialInterface* Material, int32 SortPriority)
    {
        UProceduralMeshComponent* Comp = NewObject<UProceduralMeshComponent>(this, Name);
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Comp->SetCastShadow(false);
        Comp->bReceivesDecals = false;
        Comp->SetTranslucentSortPriority(SortPriority);
        if (Material) Comp->SetMaterial(0, Material);
        Comp->SetupAttachment(Parent);
        AddInstanceComponent(Comp);
        return Comp;
    };

    CoreCrystalComp = MakeProcMesh(TEXT("CoreCrystal"), CrystalPivot, CrystalMaterial, 4);
    BeaconProceduralMesh::BuildCoreCrystal(CoreCrystalComp);
    CoreCrystalComp->RegisterComponent();

    OrbitShardsComp = MakeProcMesh(TEXT("OrbitShards"), CrystalPivot, CrystalMaterial, 3);
    BeaconProceduralMesh::BuildOrbitShards(OrbitShardsComp);
    OrbitShardsComp->RegisterComponent();

    const FLinearColor UnclaimedRingTint(0.95f, 5.0f, 1.6f, 1.f);
    InnerRingComp = MakeProcMesh(TEXT("InnerRing"), CrystalPivot, RingMaterial, 2);
    BeaconProceduralMesh::BuildCalibrationRing(InnerRingComp, 56.f, 2.2f, UnclaimedRingTint);
    InnerRingComp->RegisterComponent();

    OuterRingComp = MakeProcMesh(TEXT("OuterRing"), CrystalPivot, RingMaterial, 2);
    BeaconProceduralMesh::BuildCalibrationRing(OuterRingComp, 74.f, 2.6f, UnclaimedRingTint);
    OuterRingComp->RegisterComponent();

    GroundHaloComp = MakeProcMesh(TEXT("GroundHalo"), GetRootComponent(), RingMaterial, 1);
    GroundHaloComp->SetRelativeLocation(FVector(0.f, 0.f, -58.f));
    BeaconProceduralMesh::BuildGroundHalo(GroundHaloComp, 130.f);
    GroundHaloComp->RegisterComponent();

    PillarRoot = NewObject<USceneComponent>(this, TEXT("PillarRoot"));
    PillarRoot->SetupAttachment(GetRootComponent());
    AddInstanceComponent(PillarRoot);
    PillarRoot->RegisterComponent();

    SkyLightPillarComp = MakeProcMesh(TEXT("SkyLightPillar"), PillarRoot, PillarMaterial, 0);
    BeaconProceduralMesh::BuildLightPillar(SkyLightPillarComp, 2800.f);
    SkyLightPillarComp->RegisterComponent();

    BeaconLight = NewObject<UPointLightComponent>(this, TEXT("BeaconLight"));
    BeaconLight->SetMobility(EComponentMobility::Movable);
    BeaconLight->CastShadows = false;
    BeaconLight->Intensity = 6500.f;
    BeaconLight->AttenuationRadius = 520.f;
    BeaconLight->LightColor = FColor(70, 235, 255);
    BeaconLight->SetupAttachment(GetRootComponent());
    AddInstanceComponent(BeaconLight);
    BeaconLight->RegisterComponent();

    if (UNiagaraSystem* ParticleSystem = LoadObject<UNiagaraSystem>(
            nullptr, TEXT("/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad.NS_JumpPad")))
    {
        BeaconParticles = NewObject<UNiagaraComponent>(this, TEXT("BeaconParticles"));
        BeaconParticles->SetAsset(ParticleSystem);
        BeaconParticles->SetAutoActivate(true);
        BeaconParticles->SetRelativeLocation(FVector(0.f, 0.f, -55.f));
        BeaconParticles->SetRelativeScale3D(FVector(0.75f, 0.75f, 1.15f));
        BeaconParticles->SetupAttachment(GetRootComponent());
        AddInstanceComponent(BeaconParticles);
        BeaconParticles->RegisterComponent();
        BeaconParticles->Activate(true);
    }

    ApplyVisualState(false);
}

void AGoldenHammerActor::ApplyVisualState(bool bCarried)
{
    bHasAppliedVisualState = true;
    bLastVisualCarried = bCarried;

    const FLinearColor CrystalColor = bCarried
        ? FLinearColor(3.6f, 5.2f, 0.45f, 1.f)
        : FLinearColor(0.25f, 4.8f, 6.0f, 1.f);
    const FLinearColor RingColor = bCarried
        ? FLinearColor(3.6f, 5.2f, 0.45f, 1.f)
        : FLinearColor(0.95f, 5.0f, 1.6f, 1.f);
    const FLinearColor PillarColor = bCarried
        ? FLinearColor(3.6f, 5.2f, 0.45f, 1.f)
        : FLinearColor(0.18f, 3.2f, 5.2f, 1.f);

    if (CrystalMaterial)
    {
        CrystalMaterial->SetVectorParameterValue(TEXT("Color"), CrystalColor);
        CrystalMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.95f);
    }
    if (RingMaterial)
    {
        RingMaterial->SetVectorParameterValue(TEXT("Color"), RingColor);
        RingMaterial->SetScalarParameterValue(TEXT("Opacity"), 0.92f);
    }
    if (PillarMaterial)
    {
        PillarMaterial->SetVectorParameterValue(TEXT("Color"), PillarColor);
        PillarMaterial->SetScalarParameterValue(TEXT("Opacity"), bCarried ? 0.24f : 0.36f);
    }

    if (GroundHaloComp)
    {
        GroundHaloComp->SetVisibility(!bCarried);
    }

    if (BeaconLight)
    {
        BeaconLight->SetLightFColor(bCarried ? FColor(225, 255, 70) : FColor(70, 235, 255));
        BeaconLight->SetIntensity(bCarried ? 4500.f : 6500.f);
        BeaconLight->SetAttenuationRadius(bCarried ? 420.f : 520.f);
    }

    if (BeaconParticles)
    {
        BeaconParticles->SetRelativeLocation(bCarried ? FVector(0.f, 0.f, -20.f) : FVector(0.f, 0.f, -55.f));
        BeaconParticles->SetRelativeScale3D(bCarried ? FVector(0.45f, 0.45f, 0.65f) : FVector(0.75f, 0.75f, 1.15f));
        BeaconParticles->SetVariableLinearColor(
            FName(TEXT("User.Color")),
            bCarried ? FLinearColor(0.85f, 1.0f, 0.15f, 1.f) : FLinearColor(0.20f, 0.90f, 1.0f, 1.f));
    }
}

void AGoldenHammerActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    auto* Match = ACombatMatchActor::Find(GetWorld());
    if (HasAuthority() && Match && Match->IsMatchActive())
    {
        if (Holder)
        {
            auto* Combat = GetHolder() ? Holder->FindComponentByClass<UPlayerCombatComponent>() : nullptr;
            if (!Combat || Combat->GetHealth() <= 0 || !Holder->IsPlayerControlled()) DropFromHolder();
            else LastHolderPosition = Holder->GetActorLocation();
        }
        else if (GetWorld()->GetTimeSeconds() >= PickupAllowedAt)
        {
            for (TActorIterator<ACharacter> It(GetWorld()); It; ++It)
            {
                auto* Combat = It->FindComponentByClass<UPlayerCombatComponent>();
                if (!It->IsPlayerControlled() || !It->GetPlayerState() || !Combat || Combat->GetHealth() <= 0 || !Combat->IsCombatActive()
                    || FVector::DistSquared(It->GetActorLocation(), GroundPosition) > FMath::Square(130.f)) continue;
                FHitResult Barrier;
                FCollisionQueryParams Params(SCENE_QUERY_STAT(GoldPickup), false, *It);
                if (GetWorld()->LineTraceSingleByChannel(Barrier, It->GetActorLocation(), GroundPosition, ECC_Visibility, Params)) continue;
                Holder = *It;
                LastHolderPosition = Holder->GetActorLocation();
                Match->ChangeHolder(Holder->GetPlayerState());
                ForceNetUpdate();
                UE_LOG(LogTemp, Display, TEXT("[Beacon] PICKUP holder=%s"), *Holder->GetName());
                break;
            }
        }
    }

    ACharacter* Carrier = GetHolder();
    if (HasAuthority()) MapPosition = Carrier ? Carrier->GetActorLocation() : FVector(GroundPosition);
    const FVector Position = Carrier
        ? Carrier->GetActorLocation() - Carrier->GetActorForwardVector() * 45.f + FVector(0.f, 0.f, 95.f)
        : FVector(GroundPosition);
    SetActorLocationAndRotation(Position, FRotator::ZeroRotator);

    if (!FApp::CanEverRender() || !CrystalPivot) return;

    const bool bCarried = (Carrier != nullptr);
    if (!bHasAppliedVisualState || bLastVisualCarried != bCarried)
    {
        ApplyVisualState(bCarried);
    }

    const double Time = GetWorld()->GetTimeSeconds();
    const float HoverZ = bCarried ? 0.f : static_cast<float>(FMath::Sin(Time * 2.4) * 9.0);
    CrystalPivot->SetRelativeLocation(FVector(0.f, 0.f, HoverZ));
    CrystalPivot->SetRelativeScale3D(FVector(bCarried ? 0.55f : 1.0f));

    if (CoreCrystalComp)
    {
        CoreCrystalComp->SetRelativeRotation(FRotator(0.f, static_cast<float>(FMath::Fmod(Time * 48.0, 360.0)), 0.f));
    }
    if (OrbitShardsComp)
    {
        OrbitShardsComp->SetRelativeRotation(FRotator(0.f, static_cast<float>(-FMath::Fmod(Time * 75.0, 360.0)), 0.f));
    }
    if (InnerRingComp)
    {
        InnerRingComp->SetRelativeRotation(FRotator(28.f, static_cast<float>(FMath::Fmod(Time * 95.0, 360.0)), 18.f));
    }
    if (OuterRingComp)
    {
        OuterRingComp->SetRelativeRotation(FRotator(-26.f, static_cast<float>(-FMath::Fmod(Time * 68.0, 360.0)), -22.f));
    }
    if (GroundHaloComp && !bCarried)
    {
        const float HaloScale = static_cast<float>(1.0 + 0.04 * FMath::Sin(Time * 4.0));
        GroundHaloComp->SetRelativeScale3D(FVector(HaloScale, HaloScale, 1.0f));
    }
    if (SkyLightPillarComp)
    {
        if (bCarried)
        {
            SkyLightPillarComp->SetRelativeScale3D(FVector(0.55f, 0.55f, 0.12f));
        }
        else
        {
            const float PillarScaleXY = static_cast<float>(1.0 + 0.08 * FMath::Sin(Time * 3.5));
            SkyLightPillarComp->SetRelativeScale3D(FVector(PillarScaleXY, PillarScaleXY, 1.0f));
        }
    }
}
