#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/IHttpRequest.h"
#include "WeaponGripState.h"
#include "WeaponSyncComponent.generated.h"

USTRUCT()
struct FWeaponEquipment
{
 GENERATED_BODY()
 UPROPERTY() FString ModelUrl;
 UPROPERTY() int32 Preset = INDEX_NONE;
 UPROPERTY() bool bSample = true;
 UPROPERTY() float LengthCm = 110;
 UPROPERTY() bool bReverse = false;
 UPROPERTY() float Roll = 0;
 UPROPERTY() float Shift = 0;
 UPROPERTY() int32 Revision = 0;
};

/** Authority-created component. Game traffic contains descriptors, never GLB bytes. */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class TRY_GLB_PUT_INTO_API UWeaponSyncComponent : public UActorComponent
{
 GENERATED_BODY()
public:
 UWeaponSyncComponent();
 virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
 virtual void EndPlay(const EEndPlayReason::Type Reason) override;
 virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
 void PublishEquipment(const TArray<uint8>& Bytes, float LengthCm, bool bReverse, int32 Preset, bool bSample, const FString& ModelUrl);
 void PublishGrip(float Roll, float Shift);
 const FString& GetStatus() const { return Status; }
 const FWeaponEquipment& GetEquipment() const { return Equipment; }
private:
 UFUNCTION(Server, Reliable) void ServerSetEquipment(const FWeaponEquipment& Descriptor);
 UPROPERTY(ReplicatedUsing=OnRep_Equipment) FWeaponEquipment Equipment;
 UFUNCTION() void OnRep_Equipment();
 void SendDescriptor(FWeaponEquipment Descriptor);
 void ApplyBytes(const TArray<uint8>& Bytes, int32 Revision);
 void StartDownload();
 FWeaponGripState Visual;
 FHttpRequestPtr Download;
 int32 AppliedRevision = 0;
 int32 DownloadAttempts = 0;
 double NextRetry = 0;
 FString Status;
 FWeaponEquipment PublishedEquipment;
};
