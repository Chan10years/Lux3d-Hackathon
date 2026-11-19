#pragma once

#include "CoreMinimal.h"

class ACharacter;

/** One rendered weapon per character. Shared by local forge and replicated equipment. */
struct FWeaponGripState
{
	struct FContactSpan { FVector Start, End; float Radius; };
	TWeakObjectPtr<AActor> GripVisual;
	FVector ModelGrip = FVector::ZeroVector;
	FVector ModelAxis = FVector::ForwardVector;
	FVector ModelNormal = FVector::UpVector;
	float ModelScale = 1;
	float ModelStrikeReachCm = 0;
	TArray<FContactSpan> ModelContacts;
	int32 EquippedGripPreset = INDEX_NONE;
	float GripRoll = 0;
	float GripShift = 0;
	TWeakObjectPtr<UWorld> World;

	bool Equip(ACharacter* Character, const TArray<uint8>& Bytes, float LengthCm, bool bReverse, int32 Preset);
	bool PrepareGrip(AActor* Visual, const TArray<uint8>& Bytes, float LengthCm, bool bReverse, int32 Preset);
	void PoseWeapon(ACharacter* Character, AActor* Visual, float Pitch = 0, float Yaw = 0, float Extension = 0);
	bool TraceWeaponBarrier(const ACharacter* Holder, const FVector& Start, const FVector& End, float Radius, FHitResult& Hit) const;
	bool WeaponPoseClear(const ACharacter* Holder, const FTransform& Pose, const FVector& Grip) const;
	void Reset();
	UWorld* GetWorld() const { return World.Get(); }
};
