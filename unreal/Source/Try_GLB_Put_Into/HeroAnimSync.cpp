#include "PlayerCombatComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr uint16 FlagGunMode       = 1 << 0;
	constexpr uint16 FlagFalling       = 1 << 1;
	constexpr uint16 FlagHasInput      = 1 << 2;
	constexpr uint16 FlagSprinting     = 1 << 3;
	constexpr uint16 FlagYSDepthActive = 1 << 4;
	constexpr uint16 FlagYSDepthFire   = 1 << 5;
	constexpr uint16 FlagYSScopeActive = 1 << 6;
	constexpr uint16 FlagCrouched      = 1 << 7;
	constexpr uint16 FlagGunsVisible   = 1 << 8;

	bool ReadNumericProp(const UObject* Obj, FName Name, float& OutVal)
	{
		if (!Obj) return false;
		if (const auto* Prop = FindFProperty<FDoubleProperty>(Obj->GetClass(), Name))
		{
			OutVal = static_cast<float>(Prop->GetPropertyValue_InContainer(Obj));
			return true;
		}
		if (const auto* Prop = FindFProperty<FFloatProperty>(Obj->GetClass(), Name))
		{
			OutVal = Prop->GetPropertyValue_InContainer(Obj);
			return true;
		}
		return false;
	}

	void WriteNumericProp(UObject* Obj, FName Name, float Val)
	{
		if (!Obj) return;
		if (const auto* Prop = FindFProperty<FDoubleProperty>(Obj->GetClass(), Name))
		{
			Prop->SetPropertyValue_InContainer(Obj, static_cast<double>(Val));
		}
		else if (const auto* FloatProp = FindFProperty<FFloatProperty>(Obj->GetClass(), Name))
		{
			FloatProp->SetPropertyValue_InContainer(Obj, Val);
		}
	}

	bool ReadByteOrEnumProp(const UObject* Obj, FName Name, uint8& OutVal)
	{
		if (!Obj) return false;
		if (const auto* ByteProp = FindFProperty<FByteProperty>(Obj->GetClass(), Name))
		{
			OutVal = ByteProp->GetPropertyValue_InContainer(Obj);
			return true;
		}
		if (const auto* EnumProp = FindFProperty<FEnumProperty>(Obj->GetClass(), Name))
		{
			if (const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty())
			{
				OutVal = static_cast<uint8>(Underlying->GetUnsignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(Obj)));
				return true;
			}
		}
		return false;
	}

	void WriteByteOrEnumProp(UObject* Obj, FName Name, uint8 Val)
	{
		if (!Obj) return;
		if (const auto* ByteProp = FindFProperty<FByteProperty>(Obj->GetClass(), Name))
		{
			ByteProp->SetPropertyValue_InContainer(Obj, Val);
		}
		else if (const auto* EnumProp = FindFProperty<FEnumProperty>(Obj->GetClass(), Name))
		{
			if (const FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty())
			{
				Underlying->SetIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(Obj), static_cast<uint64>(Val));
			}
		}
	}

	bool ReadBoolProp(const UObject* Obj, FName Name, bool& OutVal)
	{
		if (!Obj) return false;
		if (const auto* Prop = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		{
			OutVal = Prop->GetPropertyValue_InContainer(Obj);
			return true;
		}
		return false;
	}

	void WriteBoolProp(UObject* Obj, FName Name, bool Val)
	{
		if (!Obj) return;
		if (const auto* Prop = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		{
			Prop->SetPropertyValue_InContainer(Obj, Val);
		}
	}

	bool ReadIntProp(const UObject* Obj, FName Name, int32& OutVal)
	{
		if (!Obj) return false;
		if (const auto* Prop = FindFProperty<FIntProperty>(Obj->GetClass(), Name))
		{
			OutVal = Prop->GetPropertyValue_InContainer(Obj);
			return true;
		}
		return false;
	}

	void WriteIntProp(UObject* Obj, FName Name, int32 Val)
	{
		if (!Obj) return;
		if (const auto* Prop = FindFProperty<FIntProperty>(Obj->GetClass(), Name))
		{
			Prop->SetPropertyValue_InContainer(Obj, Val);
		}
	}

	USkeletalMeshComponent* FindSniperWeaponMesh(const ACharacter* Character)
	{
		if (!Character) return nullptr;
		if (const auto* Prop = FindFProperty<FObjectPropertyBase>(Character->GetClass(), TEXT("YousunWeapon")))
		{
			return Cast<USkeletalMeshComponent>(Prop->GetObjectPropertyValue_InContainer(Character));
		}
		return nullptr;
	}

	USceneComponent* FindNamedSceneComp(const ACharacter* Character, FName Name)
	{
		if (!Character) return nullptr;
		if (const auto* Prop = FindFProperty<FObjectPropertyBase>(Character->GetClass(), Name))
		{
			return Cast<USceneComponent>(Prop->GetObjectPropertyValue_InContainer(Character));
		}
		return nullptr;
	}

	bool IsAnimStateMateriallyDifferent(const FHeroAnimSyncState& A, const FHeroAnimSyncState& B)
	{
		if (A.bValid != B.bValid || A.Flags != B.Flags || A.PureState != B.PureState
			|| A.SC_Stage != B.SC_Stage || A.GunThirdPhase != B.GunThirdPhase
			|| A.YS_CombatPhase != B.YS_CombatPhase || A.MeshAnimClass != B.MeshAnimClass)
		{
			return true;
		}
		if (FVector::DistSquared(FVector(A.WorldInput), FVector(B.WorldInput)) > 0.0025) return true;
		if (FVector::DistSquared(FVector(A.Velocity), FVector(B.Velocity)) > 225.0) return true;
		if (FMath::Abs(FMath::FindDeltaAngleDegrees(A.ActorYaw, B.ActorYaw)) > 2.0f) return true;
		if (FMath::Abs(A.InputVelocityDir - B.InputVelocityDir) > 3.0f) return true;
		if (FMath::Abs(A.YS_CrouchDepth - B.YS_CrouchDepth) > 0.04f) return true;
		if (FMath::Abs(A.YS_DashDirection - B.YS_DashDirection) > 3.0f) return true;
		if (FMath::Abs(A.YS_DepthValue - B.YS_DepthValue) > 0.04f) return true;
		return false;
	}

	bool HasDiscreteAnimChange(const FHeroAnimSyncState& A, const FHeroAnimSyncState& B)
	{
		const bool AHasInput = (A.Flags & FlagHasInput) != 0 || !FVector(A.WorldInput).IsNearlyZero(0.05);
		const bool BHasInput = (B.Flags & FlagHasInput) != 0 || !FVector(B.WorldInput).IsNearlyZero(0.05);
		return A.bValid != B.bValid || A.Flags != B.Flags || A.PureState != B.PureState
			|| A.SC_Stage != B.SC_Stage || A.GunThirdPhase != B.GunThirdPhase
			|| A.YS_CombatPhase != B.YS_CombatPhase || A.MeshAnimClass != B.MeshAnimClass
			|| AHasInput != BHasInput;
	}
}

void UPlayerCombatComponent::OnRep_SyncedAnimState()
{
	ApplySyncedAnimState(0.f);
}

void UPlayerCombatComponent::ServerSyncAnimState_Implementation(const FHeroAnimSyncState& InState)
{
	SyncedAnimState = InState;
	SyncedAnimState.bValid = true;
}

void UPlayerCombatComponent::ServerSyncMontage_Implementation(uint8 Channel, uint16 SeqId, UObject* AnimAsset,
	FName SlotName, FName SectionName, float Position, float PlayRate, bool bStop)
{
	if (Channel > 1) return;
	if (!bStop && !IsValid(AnimAsset)) return;
	if (!bStop && !AnimAsset->IsA<UAnimMontage>() && !AnimAsset->IsA<UAnimSequenceBase>()) return;
	MulticastSyncMontage(Channel, SeqId, AnimAsset, SlotName, SectionName, Position, PlayRate, bStop);
}

void UPlayerCombatComponent::MulticastSyncMontage_Implementation(uint8 Channel, uint16 SeqId, UObject* AnimAsset,
	FName SlotName, FName SectionName, float Position, float PlayRate, bool bStop)
{
	ApplyRemoteMontage(Channel, SeqId, AnimAsset, SlotName, SectionName, Position, PlayRate, bStop);
}

void UPlayerCombatComponent::ClientSnapToRespawn_Implementation(FVector_NetQuantize Location, FRotator Rotation)
{
	auto* Character = Cast<ACharacter>(GetOwner());
	if (!Character) return;
	Character->SetActorLocationAndRotation(FVector(Location), Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	if (auto* MoveComp = Character->GetCharacterMovement())
	{
		MoveComp->StopMovementImmediately();
	}
	if (auto* PC = Cast<APlayerController>(Character->GetController()))
	{
		PC->SetControlRotation(Rotation);
	}
	PrevLocalLocation = FVector(Location);
	bHasPrevLocalLocation = true;
}

void UPlayerCombatComponent::UpdateAnimationSync(float DeltaTime)
{
	UWorld* World = GetWorld();
	auto* Character = Cast<ACharacter>(GetOwner());
	if (!World || !Character || World->GetNetMode() == NM_Standalone) return;

	USkeletalMeshComponent* Mesh = Character->GetMesh();
	USkeletalMeshComponent* SniperWeapon = FindSniperWeaponMesh(Character);
	if (Mesh)
	{
		Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}
	if (SniperWeapon)
	{
		SniperWeapon->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}

	const bool bLocal = Character->IsLocallyControlled();

	if (!bLocal)
	{
		if (!bAnimPrereqConfigured && Mesh)
		{
			Mesh->AddTickPrerequisiteComponent(this);
			if (SniperWeapon)
			{
				SniperWeapon->AddTickPrerequisiteComponent(this);
			}
			bAnimPrereqConfigured = true;
		}
		if (Character->HasAuthority())
		{
			Character->SetNetUpdateFrequency(60.f);
			Character->SetMinNetUpdateFrequency(30.f);
			if (auto* MoveComp = Character->GetCharacterMovement())
			{
				const bool bAllowClientPos = Health > 0 && World->GetTimeSeconds() >= SuppressClientAuthPosUntil;
				MoveComp->bIgnoreClientMovementErrorChecksAndCorrection = bAllowClientPos;
				MoveComp->bServerAcceptClientAuthoritativePosition = bAllowClientPos;
			}
		}
		ApplySyncedAnimState(DeltaTime);
		return;
	}

	// Owning player: sample live locomotion/hero state and active montages and replicate to remote proxies.
	if (Character->HasAuthority())
	{
		Character->SetNetUpdateFrequency(60.f);
		Character->SetMinNetUpdateFrequency(30.f);
	}

	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	auto* MoveComp = Character->GetCharacterMovement();

	FHeroAnimSyncState Local;
	Local.bValid = true;
	Local.ActorYaw = Character->GetActorRotation().Yaw;
	Local.ControlYaw = Character->GetControlRotation().Yaw;
	Local.MeshAnimClass = Mesh ? Mesh->GetAnimClass() : nullptr;

	float FwdInput = 0.f;
	float RightInput = 0.f;
	const bool bHasFwd = ReadNumericProp(Character, TEXT("ForwardInput"), FwdInput);
	const bool bHasRight = ReadNumericProp(Character, TEXT("RightInput"), RightInput);

	FVector WorldInput = FVector::ZeroVector;
	if ((bHasFwd || bHasRight) && (FMath::Abs(FwdInput) > 0.01f || FMath::Abs(RightInput) > 0.01f))
	{
		const FRotator CtrlYawRot(0.f, Local.ControlYaw, 0.f);
		const FVector CtrlFwd = FRotationMatrix(CtrlYawRot).GetUnitAxis(EAxis::X);
		const FVector CtrlRight = FRotationMatrix(CtrlYawRot).GetUnitAxis(EAxis::Y);
		WorldInput = (CtrlFwd * FwdInput + CtrlRight * RightInput).GetClampedToMaxSize(1.f);
	}
	else
	{
		const FVector LastInput = Character->GetLastMovementInputVector();
		if (!LastInput.IsNearlyZero(0.01f))
		{
			WorldInput = LastInput.GetClampedToMaxSize(1.f);
		}
	}
	Local.WorldInput = WorldInput;

	FVector Vel = Character->GetVelocity();
	const FVector CurLoc = Character->GetActorLocation();
	if (bHasPrevLocalLocation && DeltaTime > 0.001f)
	{
		const FVector DispVel = (CurLoc - PrevLocalLocation) / DeltaTime;
		if (Vel.Size2D() < 15.f && DispVel.Size2D() > 25.f && DispVel.Size2D() < 2500.f)
		{
			Vel.X = DispVel.X;
			Vel.Y = DispVel.Y;
		}
	}
	PrevLocalLocation = CurLoc;
	bHasPrevLocalLocation = true;
	Local.Velocity = Vel;

	if (!ReadNumericProp(Character, TEXT("InputVelocityDir"), Local.InputVelocityDir))
	{
		ReadNumericProp(Anim, TEXT("InputVelocityDir"), Local.InputVelocityDir);
	}
	if (!ReadByteOrEnumProp(Character, TEXT("PureState"), Local.PureState))
	{
		ReadByteOrEnumProp(Anim, TEXT("PureState"), Local.PureState);
	}

	int32 TmpInt = 0;
	if (ReadIntProp(Character, TEXT("SC_Stage"), TmpInt)) Local.SC_Stage = static_cast<uint8>(FMath::Clamp(TmpInt, 0, 255));
	if (ReadIntProp(Character, TEXT("GunThirdPhase"), TmpInt)) Local.GunThirdPhase = static_cast<uint8>(FMath::Clamp(TmpInt, 0, 255));
	if (ReadIntProp(Character, TEXT("YS_CombatPhase"), TmpInt) || ReadIntProp(Anim, TEXT("YS_CombatPhase"), TmpInt))
	{
		Local.YS_CombatPhase = static_cast<uint8>(FMath::Clamp(TmpInt, 0, 255));
	}

	if (!ReadNumericProp(Character, TEXT("YS_CrouchDepth"), Local.YS_CrouchDepth))
		ReadNumericProp(Anim, TEXT("YS_CrouchDepth"), Local.YS_CrouchDepth);
	if (!ReadNumericProp(Character, TEXT("YS_DashDirection"), Local.YS_DashDirection))
		ReadNumericProp(Anim, TEXT("YS_DashDirection"), Local.YS_DashDirection);
	if (!ReadNumericProp(Character, TEXT("YS_DepthValue"), Local.YS_DepthValue))
		ReadNumericProp(Anim, TEXT("YS_DepthValue"), Local.YS_DepthValue);

	bool bFlag = false;
	if ((ReadBoolProp(Character, TEXT("bGunMovementMode"), bFlag) || ReadBoolProp(Anim, TEXT("bGunMovementMode"), bFlag)) && bFlag)
		Local.Flags |= FlagGunMode;
	if (MoveComp && MoveComp->IsFalling())
		Local.Flags |= FlagFalling;
	bool bHasInputAnim = false;
	if (ReadBoolProp(Anim, TEXT("HasInput"), bHasInputAnim) ? bHasInputAnim : !WorldInput.IsNearlyZero(0.05f))
		Local.Flags |= FlagHasInput;
	bool bSprintingAnim = false;
	if ((ReadBoolProp(Anim, TEXT("Sprinting"), bSprintingAnim) && bSprintingAnim) || Local.PureState == 3)
		Local.Flags |= FlagSprinting;
	if ((ReadBoolProp(Character, TEXT("YS_DepthActive"), bFlag) || ReadBoolProp(Anim, TEXT("YS_DepthActive"), bFlag)) && bFlag)
		Local.Flags |= FlagYSDepthActive;
	if ((ReadBoolProp(Character, TEXT("YS_DepthFire"), bFlag) || ReadBoolProp(Anim, TEXT("YS_DepthFire"), bFlag)) && bFlag)
		Local.Flags |= FlagYSDepthFire;
	if (ReadBoolProp(Character, TEXT("YS_ScopeActive"), bFlag) && bFlag)
		Local.Flags |= FlagYSScopeActive;
	if (Character->bIsCrouched)
		Local.Flags |= FlagCrouched;

	if (const USceneComponent* GunR = FindNamedSceneComp(Character, TEXT("Gun_Right")))
	{
		if (GunR->IsVisible()) Local.Flags |= FlagGunsVisible;
	}

	const double Now = World->GetTimeSeconds();
	const bool bDiscrete = HasDiscreteAnimChange(Local, LastSentAnimState);
	const bool bChanged = IsAnimStateMateriallyDifferent(Local, LastSentAnimState);
	const bool bShouldSend = bDiscrete
		|| (bChanged && (Now - LastAnimSendTime) >= 0.033)
		|| ((Now - LastAnimSendTime) >= 0.25);

	if (bShouldSend)
	{
		LastSentAnimState = Local;
		LastAnimSendTime = Now;
		if (Character->HasAuthority())
		{
			SyncedAnimState = Local;
		}
		else
		{
			ServerSyncAnimState(Local);
		}
	}

	PollLocalMontageChannel(0, Mesh);
	if (SniperWeapon)
	{
		PollLocalMontageChannel(1, SniperWeapon);
	}
}

void UPlayerCombatComponent::ApplySyncedAnimState(float DeltaTime)
{
	auto* Character = Cast<ACharacter>(GetOwner());
	if (!Character || Character->IsLocallyControlled() || !SyncedAnimState.bValid) return;

	USkeletalMeshComponent* Mesh = Character->GetMesh();
	if (Mesh && SyncedAnimState.MeshAnimClass && SyncedAnimState.MeshAnimClass->IsChildOf(UAnimInstance::StaticClass()))
	{
		if (Mesh->GetAnimClass() != SyncedAnimState.MeshAnimClass)
		{
			Mesh->SetAnimInstanceClass(SyncedAnimState.MeshAnimClass);
		}
	}

	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (Anim && (Anim->RootMotionMode == ERootMotionMode::RootMotionFromEverything
		|| Anim->RootMotionMode == ERootMotionMode::RootMotionFromMontagesOnly))
	{
		Anim->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	}

	const FVector WorldInput(SyncedAnimState.WorldInput);
	const FRotator LocalCtrlRot(0.f, Character->GetControlRotation().Yaw, 0.f);
	const FVector LocalCtrlFwd = FRotationMatrix(LocalCtrlRot).GetUnitAxis(EAxis::X);
	const FVector LocalCtrlRight = FRotationMatrix(LocalCtrlRot).GetUnitAxis(EAxis::Y);
	const float ProxyForward = static_cast<float>(FVector::DotProduct(WorldInput, LocalCtrlFwd));
	const float ProxyRight = static_cast<float>(FVector::DotProduct(WorldInput, LocalCtrlRight));

	WriteNumericProp(Character, TEXT("ForwardInput"), ProxyForward);
	WriteNumericProp(Character, TEXT("RightInput"), ProxyRight);
	WriteNumericProp(Character, TEXT("InputVelocityDir"), SyncedAnimState.InputVelocityDir);
	WriteByteOrEnumProp(Character, TEXT("PureState"), SyncedAnimState.PureState);
	WriteIntProp(Character, TEXT("SC_Stage"), static_cast<int32>(SyncedAnimState.SC_Stage));
	WriteIntProp(Character, TEXT("GunThirdPhase"), static_cast<int32>(SyncedAnimState.GunThirdPhase));
	WriteIntProp(Character, TEXT("YS_CombatPhase"), static_cast<int32>(SyncedAnimState.YS_CombatPhase));
	WriteNumericProp(Character, TEXT("YS_CrouchDepth"), SyncedAnimState.YS_CrouchDepth);
	WriteNumericProp(Character, TEXT("YS_DashDirection"), SyncedAnimState.YS_DashDirection);
	WriteNumericProp(Character, TEXT("YS_DepthValue"), SyncedAnimState.YS_DepthValue);

	const bool bGunMode = (SyncedAnimState.Flags & FlagGunMode) != 0;
	const bool bFalling = (SyncedAnimState.Flags & FlagFalling) != 0;
	const bool bHasInput = (SyncedAnimState.Flags & FlagHasInput) != 0;
	const bool bSprinting = (SyncedAnimState.Flags & FlagSprinting) != 0;
	const bool bYSDepthActive = (SyncedAnimState.Flags & FlagYSDepthActive) != 0;
	const bool bYSDepthFire = (SyncedAnimState.Flags & FlagYSDepthFire) != 0;
	const bool bYSScopeActive = (SyncedAnimState.Flags & FlagYSScopeActive) != 0;
	const bool bCrouched = (SyncedAnimState.Flags & FlagCrouched) != 0;

	WriteBoolProp(Character, TEXT("bGunMovementMode"), bGunMode);
	WriteBoolProp(Character, TEXT("YS_DepthActive"), bYSDepthActive);
	WriteBoolProp(Character, TEXT("YS_DepthFire"), bYSDepthFire);
	WriteBoolProp(Character, TEXT("YS_ScopeActive"), bYSScopeActive);

	// Populate LastControlInputVector without leaving pending input for CharacterMovementComponent.
	Character->Internal_ConsumeMovementInputVector();
	if (!WorldInput.IsNearlyZero(0.01f))
	{
		Character->Internal_AddMovementInput(WorldInput, true);
		Character->Internal_ConsumeMovementInputVector();
	}

	if (Anim)
	{
		WriteNumericProp(Anim, TEXT("ForwardInput"), ProxyForward);
		WriteNumericProp(Anim, TEXT("RightInput"), ProxyRight);
		WriteNumericProp(Anim, TEXT("InputVelocityDir"), SyncedAnimState.InputVelocityDir);
		WriteByteOrEnumProp(Anim, TEXT("PureState"), SyncedAnimState.PureState);
		WriteIntProp(Anim, TEXT("SC_Stage"), static_cast<int32>(SyncedAnimState.SC_Stage));
		WriteIntProp(Anim, TEXT("YS_CombatPhase"), static_cast<int32>(SyncedAnimState.YS_CombatPhase));
		WriteNumericProp(Anim, TEXT("YS_CrouchDepth"), SyncedAnimState.YS_CrouchDepth);
		WriteNumericProp(Anim, TEXT("YS_DashDirection"), SyncedAnimState.YS_DashDirection);
		WriteNumericProp(Anim, TEXT("YS_DepthValue"), SyncedAnimState.YS_DepthValue);
		WriteBoolProp(Anim, TEXT("bGunMovementMode"), bGunMode);
		WriteBoolProp(Anim, TEXT("HasInput"), bHasInput);
		WriteBoolProp(Anim, TEXT("Sprinting"), bSprinting);
		WriteBoolProp(Anim, TEXT("YS_DepthActive"), bYSDepthActive);
		WriteBoolProp(Anim, TEXT("YS_DepthFire"), bYSDepthFire);
	}

	if (USceneComponent* GunL = FindNamedSceneComp(Character, TEXT("Gun_Left")))
	{
		const bool bShowGuns = (SyncedAnimState.Flags & FlagGunsVisible) != 0;
		if (GunL->IsVisible() != bShowGuns) GunL->SetVisibility(bShowGuns, true);
	}
	if (USceneComponent* GunR = FindNamedSceneComp(Character, TEXT("Gun_Right")))
	{
		const bool bShowGuns = (SyncedAnimState.Flags & FlagGunsVisible) != 0;
		if (GunR->IsVisible() != bShowGuns) GunR->SetVisibility(bShowGuns, true);
	}

	if (auto* MoveComp = Character->GetCharacterMovement(); MoveComp && Health > 0)
	{
		const FVector SyncedVel(SyncedAnimState.Velocity);
		MoveComp->Velocity = SyncedVel;
		if (!Character->HasAuthority())
		{
			if (bFalling && MoveComp->MovementMode != MOVE_Falling)
			{
				MoveComp->SetMovementMode(MOVE_Falling);
			}
			else if (!bFalling && MoveComp->MovementMode == MOVE_Falling)
			{
				MoveComp->SetMovementMode(MOVE_Walking);
			}
		}
		else if (DeltaTime > 0.f)
		{
			// On Server watching a Client hero (including RootMotion heroes), keep ActorYaw aligned smoothly.
			const FRotator Cur = Character->GetActorRotation();
			const FRotator Target(Cur.Pitch, SyncedAnimState.ActorYaw, Cur.Roll);
			Character->SetActorRotation(FMath::RInterpTo(Cur, Target, DeltaTime, 24.f));
		}
	}

	Character->SetIsCrouched(bCrouched);
}

void UPlayerCombatComponent::PollLocalMontageChannel(uint8 Channel, USkeletalMeshComponent* MeshComp)
{
	if (Channel > 1 || !MeshComp) return;
	UAnimInstance* Anim = MeshComp->GetAnimInstance();
	auto* Character = Cast<ACharacter>(GetOwner());
	if (!Anim || !Character) return;

	FAnimMontageInstance* ActiveInst = nullptr;
	for (int32 Idx = Anim->MontageInstances.Num() - 1; Idx >= 0; --Idx)
	{
		FAnimMontageInstance* Inst = Anim->MontageInstances[Idx];
		if (Inst && Inst->IsValid() && Inst->IsActive())
		{
			ActiveInst = Inst;
			break;
		}
	}

	FLocalMontageTrack& Track = LocalMontages[Channel];

	if (!ActiveInst)
	{
		if (Track.bActive)
		{
			Track.bActive = false;
			Track.InstanceId = INDEX_NONE;
			Track.AnimAsset.Reset();
			Track.SlotName = NAME_None;
			Track.SectionName = NAME_None;
			++Track.SeqId;
			if (Character->HasAuthority())
			{
				MulticastSyncMontage(Channel, Track.SeqId, nullptr, NAME_None, NAME_None, 0.f, 1.f, true);
			}
			else
			{
				ServerSyncMontage(Channel, Track.SeqId, nullptr, NAME_None, NAME_None, 0.f, 1.f, true);
			}
		}
		return;
	}

	UAnimMontage* Montage = ActiveInst->Montage;
	if (!Montage) return;

	UObject* AssetToSync = nullptr;
	FName SlotName = NAME_None;
	if (Montage->IsDynamicMontage())
	{
		if (Montage->SlotAnimTracks.Num() > 0 && Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.Num() > 0)
		{
			SlotName = Montage->SlotAnimTracks[0].SlotName;
			AssetToSync = Montage->SlotAnimTracks[0].AnimTrack.AnimSegments[0].GetAnimReference();
		}
	}
	else
	{
		AssetToSync = Montage;
	}

	if (!AssetToSync || !AssetToSync->IsSupportedForNetworking())
	{
		return;
	}

	const int32 InstId = ActiveInst->GetInstanceID();
	const FName Section = ActiveInst->GetCurrentSection();
	const float Pos = ActiveInst->GetPosition();
	const float Rate = ActiveInst->GetPlayRate();

	const bool bNewStart = !Track.bActive
		|| Track.InstanceId != InstId
		|| Track.AnimAsset.Get() != AssetToSync
		|| Track.SlotName != SlotName
		|| Track.SectionName != Section
		|| (Pos + 0.08f < Track.LastPosition)
		|| (FMath::Abs(Rate - Track.LastPlayRate) > 0.25f);

	Track.LastPosition = Pos;
	Track.LastPlayRate = Rate;

	if (!bNewStart) return;

	Track.bActive = true;
	Track.InstanceId = InstId;
	Track.AnimAsset = AssetToSync;
	Track.SlotName = SlotName;
	Track.SectionName = Section;
	++Track.SeqId;

	if (Character->HasAuthority())
	{
		MulticastSyncMontage(Channel, Track.SeqId, AssetToSync, SlotName, Section, Pos, Rate, false);
	}
	else
	{
		ServerSyncMontage(Channel, Track.SeqId, AssetToSync, SlotName, Section, Pos, Rate, false);
	}
}

void UPlayerCombatComponent::ApplyRemoteMontage(uint8 Channel, uint16 SeqId, UObject* AnimAsset, FName SlotName,
	FName SectionName, float Position, float PlayRate, bool bStop)
{
	auto* Character = Cast<ACharacter>(GetOwner());
	if (!Character || Character->IsLocallyControlled() || Channel > 1) return;

	USkeletalMeshComponent* TargetMesh = (Channel == 0) ? Character->GetMesh() : FindSniperWeaponMesh(Character);
	UAnimInstance* Anim = TargetMesh ? TargetMesh->GetAnimInstance() : nullptr;
	if (!Anim) return;

	LastAppliedMontageSeq[Channel] = SeqId;

	if (bStop)
	{
		if (UAnimMontage* Prev = RemoteSyncedMontages[Channel].Get())
		{
			Anim->Montage_Stop(0.12f, Prev);
		}
		RemoteSyncedMontages[Channel].Reset();
		return;
	}

	if (UAnimMontage* MontageAsset = Cast<UAnimMontage>(AnimAsset))
	{
		const float SafeRate = FMath::IsNearlyZero(PlayRate) ? 0.0001f : PlayRate;
		Anim->Montage_Play(MontageAsset, SafeRate, EMontagePlayReturnType::MontageLength, FMath::Max(0.f, Position), true);
		if (!SectionName.IsNone() && MontageAsset->IsValidSectionName(SectionName))
		{
			Anim->Montage_JumpToSection(SectionName, MontageAsset);
		}
		if (FMath::IsNearlyZero(PlayRate))
		{
			Anim->Montage_SetPlayRate(MontageAsset, 0.f);
			Anim->Montage_SetPosition(MontageAsset, FMath::Max(0.f, Position));
		}
		RemoteSyncedMontages[Channel] = MontageAsset;
	}
	else if (UAnimSequenceBase* SeqAsset = Cast<UAnimSequenceBase>(AnimAsset))
	{
		const FName EffectiveSlot = SlotName.IsNone() ? FName(TEXT("DefaultSlot")) : SlotName;
		const float SafeRate = FMath::IsNearlyZero(PlayRate) ? 0.0001f : PlayRate;
		UAnimMontage* Dyn = Anim->PlaySlotAnimationAsDynamicMontage(
			SeqAsset, EffectiveSlot, 0.08f, 0.15f, SafeRate, 1, -1.f, FMath::Max(0.f, Position));
		if (Dyn && FMath::IsNearlyZero(PlayRate))
		{
			Anim->Montage_SetPlayRate(Dyn, 0.f);
			Anim->Montage_SetPosition(Dyn, FMath::Max(0.f, Position));
		}
		RemoteSyncedMontages[Channel] = Dyn;
	}
}
