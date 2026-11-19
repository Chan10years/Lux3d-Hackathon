#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "GoldenHammerActor.generated.h"

class ACharacter;
class APlayerState;
class UMaterialInstanceDynamic;
class UNiagaraComponent;
class UPointLightComponent;
class UProceduralMeshComponent;
class USceneComponent;

/** A single replicated objective. Clients only render the authority's holder/ground position. */
UCLASS()
class TRY_GLB_PUT_INTO_API AGoldenHammerActor : public AActor
{
    GENERATED_BODY()
public:
    AGoldenHammerActor();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaTime) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    void InitializeSpawn(const FVector& Desired);
    void DropFromHolder();
    void ResetToSpawn();
    ACharacter* GetHolder() const;
    APlayerState* GetHolderState() const;
    FVector GetGroundPosition() const { return GroundPosition; }
    FVector GetMapPosition() const { return MapPosition; }

private:
    bool FindGround(const FVector& Desired, FVector& Out) const;
    void CreateVisual();
    void ApplyVisualState(bool bCarried);
    UPROPERTY(Replicated) TObjectPtr<ACharacter> Holder;
    UPROPERTY(Replicated) FVector_NetQuantize GroundPosition;
    // Always-relevant objective location, even when its carrier Pawn is outside relevancy.
    UPROPERTY(Replicated) FVector_NetQuantize MapPosition;
    UPROPERTY() TObjectPtr<USceneComponent> CrystalPivot;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> CoreCrystalComp;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> OrbitShardsComp;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> InnerRingComp;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> OuterRingComp;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> GroundHaloComp;
    UPROPERTY() TObjectPtr<USceneComponent> PillarRoot;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> SkyLightPillarComp;
    UPROPERTY() TObjectPtr<UPointLightComponent> BeaconLight;
    UPROPERTY() TObjectPtr<UNiagaraComponent> BeaconParticles;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> CrystalMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RingMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> PillarMaterial;
    FVector SpawnPosition = FVector::ZeroVector;
    FVector LastHolderPosition = FVector::ZeroVector;
    double PickupAllowedAt = 0;
    bool bHasAppliedVisualState = false;
    bool bLastVisualCarried = false;
};
