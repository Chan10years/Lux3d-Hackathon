#include "WeaponSyncComponent.h"
#include "PlayerCombatComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Components/SkeletalMeshComponent.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Net/UnrealNetwork.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
 bool ValidModel(const TArray<uint8>& Bytes)
 {
  return Bytes.Num() >= 20 && Bytes.Num() <= 32*1024*1024 && FMemory::Memcmp(Bytes.GetData(), "glTF", 4) == 0;
 }
 bool ValidEquipment(const FWeaponEquipment& E)
 {
  return E.Preset >= -1 && E.Preset < 10 && FMath::IsFinite(E.LengthCm) && E.LengthCm >= 25 && E.LengthCm <= 220
   && FMath::IsFinite(E.Roll) && FMath::IsFinite(E.Shift) && FMath::Abs(E.Roll) <= 360 && FMath::Abs(E.Shift) <= 6
   && (E.bSample ? E.Preset >= 0 : (E.ModelUrl.Len() <= 2048 && (E.ModelUrl.StartsWith(TEXT("http://")) || E.ModelUrl.StartsWith(TEXT("https://")))));
 }
 const TCHAR* Samples[] = {TEXT("sword"),TEXT("spear"),TEXT("longsword"),TEXT("staff"),TEXT("axe"),TEXT("hammer"),TEXT("hook"),TEXT("gauntlet"),TEXT("bow"),TEXT("hand_cannon")};
}

UWeaponSyncComponent::UWeaponSyncComponent()
{
 SetIsReplicatedByDefault(true); PrimaryComponentTick.bCanEverTick = true;
}
void UWeaponSyncComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
 Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UWeaponSyncComponent, Equipment);
}
void UWeaponSyncComponent::EndPlay(const EEndPlayReason::Type Reason)
{
 for (FHttpRequestPtr* Request : {&Download})
  if (*Request) { (*Request)->OnProcessRequestComplete().Unbind(); (*Request)->CancelRequest(); Request->Reset(); }
 Visual.Reset(); Super::EndPlay(Reason);
}
void UWeaponSyncComponent::PublishEquipment(const TArray<uint8>& Bytes, float LengthCm, bool bReverse, int32 Preset, bool bSample, const FString& ModelUrl)
{
 ACharacter* Character = Cast<ACharacter>(GetOwner());
 if (!Character || !Character->IsLocallyControlled() || !ValidModel(Bytes)) return;
 FWeaponEquipment Descriptor;
 Descriptor.LengthCm = LengthCm; Descriptor.Preset = Preset; Descriptor.bReverse = bReverse; Descriptor.bSample = bSample; Descriptor.ModelUrl = ModelUrl;
 SendDescriptor(Descriptor);
}
void UWeaponSyncComponent::SendDescriptor(FWeaponEquipment Descriptor)
{
 if (!ValidEquipment(Descriptor)) { Status = TEXT("武器同步参数无效，保留旧武器。"); return; }
 PublishedEquipment = Descriptor;
 if (GetOwner()->HasAuthority()) ServerSetEquipment_Implementation(Descriptor); else ServerSetEquipment(Descriptor);
 Status = TEXT("武器状态已提交服务器");
 UE_LOG(LogTemp, Display, TEXT("[WeaponSync] descriptor sent sample=%d preset=%d"), Descriptor.bSample, Descriptor.Preset);
}
void UWeaponSyncComponent::PublishGrip(float Roll, float Shift)
{
 if ((PublishedEquipment.Preset == INDEX_NONE && PublishedEquipment.ModelUrl.IsEmpty())) return;
 FWeaponEquipment Next = PublishedEquipment; Next.Roll = Roll; Next.Shift = Shift; SendDescriptor(Next);
}
void UWeaponSyncComponent::ServerSetEquipment_Implementation(const FWeaponEquipment& Descriptor)
{
 if (!ValidEquipment(Descriptor)) return;
 const int32 Revision = Equipment.Revision + 1;
 Equipment = Descriptor; Equipment.Revision = Revision; GetOwner()->ForceNetUpdate();
 if (auto* Combat = GetOwner()->FindComponentByClass<UPlayerCombatComponent>()) Combat->RefreshWeaponMode();
 UE_LOG(LogTemp, Display, TEXT("[WeaponSync] authority accepted revision=%d owner=%s"), Revision, *GetOwner()->GetName());
 OnRep_Equipment();
}
void UWeaponSyncComponent::OnRep_Equipment()
{
 if (Download) { Download->OnProcessRequestComplete().Unbind(); Download->CancelRequest(); Download.Reset(); }
 ACharacter* Character = Cast<ACharacter>(GetOwner());
 if (!Character || Equipment.Revision == 0) return;
 if (Character->IsLocallyControlled()) { AppliedRevision = Equipment.Revision; return; }
 DownloadAttempts = 0; NextRetry = 0;
}
void UWeaponSyncComponent::StartDownload()
{
 const int32 Revision = Equipment.Revision; ++DownloadAttempts;
 if (Equipment.bSample)
 {
  TArray<uint8> Bytes;
  if (Equipment.Preset >= 0 && Equipment.Preset < UE_ARRAY_COUNT(Samples))
   FFileHelper::LoadFileToArray(Bytes, *(FPaths::ProjectContentDir() / TEXT("WeaponSamples") / FString::Printf(TEXT("trial-%s.glb"), Samples[Equipment.Preset])));
  ApplyBytes(Bytes, Revision); NextRetry = FPlatformTime::Seconds() + 3; return;
 }
 Download = FHttpModule::Get().CreateRequest();
 Download->SetURL(Equipment.ModelUrl); Download->SetVerb(TEXT("GET")); Download->SetTimeout(60);
 Download->OnProcessRequestComplete().BindWeakLambda(this, [this, Revision](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bOk)
 {
  if (Download != Request || Equipment.Revision != Revision) return;
  Download.Reset();
  if (bOk && Response && Response->GetResponseCode() == 200) ApplyBytes(Response->GetContent(), Revision);
  else UE_LOG(LogTemp, Warning, TEXT("[WeaponSync] model download failed revision=%d; old visual retained"), Revision);
  NextRetry = FPlatformTime::Seconds() + 3;
 });
 if (!Download->ProcessRequest()) { Download.Reset(); NextRetry = FPlatformTime::Seconds() + 3; }
}
void UWeaponSyncComponent::ApplyBytes(const TArray<uint8>& Bytes, int32 Revision)
{
 if (Revision != Equipment.Revision || !ValidModel(Bytes)) return;
 ACharacter* Character = Cast<ACharacter>(GetOwner());
 if (!Character || Character->IsLocallyControlled() || !Visual.Equip(Character, Bytes, Equipment.LengthCm, Equipment.bReverse, Equipment.Preset)) return;
 Visual.GripRoll = Equipment.Roll; Visual.GripShift = Equipment.Shift; AppliedRevision = Revision;
 UE_LOG(LogTemp, Display, TEXT("[WeaponSync] remote equipped revision=%d owner=%s bytes=%d"), Revision, *GetOwner()->GetName(), Bytes.Num());
}
void UWeaponSyncComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
 Super::TickComponent(DeltaTime, TickType, TickFunction);
 ACharacter* Character = Cast<ACharacter>(GetOwner());
 if (!Character || Character->IsLocallyControlled()) return;
 if (AppliedRevision != Equipment.Revision && Equipment.Revision > 0 && !Download && DownloadAttempts < 3 && FPlatformTime::Seconds() >= NextRetry && Character->GetMesh()) StartDownload();
 if (Visual.GripVisual.IsValid())
 {
  float Pitch = 0, Yaw = 0, Extension = 0;
  if (auto* Combat = Character->FindComponentByClass<UPlayerCombatComponent>()) Combat->GetAttackPose(Pitch, Yaw, Extension);
  Visual.PoseWeapon(Character, Visual.GripVisual.Get(), Pitch, Yaw, Extension);
 }
}
