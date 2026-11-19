#include "WeaponForgeSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/FileManager.h"
#include "UnrealClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	enum class EAttack : uint8 { Sweep, Strike, Slam, Shot };
	struct FAttackProfile
	{
		const TCHAR* Name;
		float MinReach, MaxReach;
		float FastInterval, SlowInterval, WindupFraction;
		int32 LowDamage, HighDamage;
	};

	constexpr FAttackProfile AttackProfiles[] = {
		{TEXT("横扫"), 85, 195, .42f, .74f, .36f, 19, 26},
		{TEXT("直击"), 65, 255, .32f, .76f, .36f, 14, 24},
		{TEXT("重砸"), 105, 165, .63f, 1.02f, .45f, 29, 40},
		{TEXT("远射"), 300, 1200, .72f, 1.30f, .45f, 19, 31},
	};
	constexpr float SweepHalfAngle = 62.f;
	constexpr float StrikeHalfWidth = 29.f;
	constexpr float MeleeBodyReach = 40.f;
	const FVector ShieldHalfExtent(7, 36, 55);

	const FAttackProfile& AttackProfile(EAttack Attack)
	{
		return AttackProfiles[static_cast<uint8>(Attack)];
	}

	struct FAttackStats
	{
		float Interval;
		int32 Damage;
	};

	FAttackStats AttackStats(const FAttackProfile& Profile, float Reach)
	{
		const float Alpha = FMath::Clamp((Reach - Profile.MinReach) / (Profile.MaxReach - Profile.MinReach), 0.f, 1.f);
		return {FMath::Lerp(Profile.FastInterval, Profile.SlowInterval, Alpha),
			FMath::RoundToInt(FMath::Lerp(float(Profile.LowDamage), float(Profile.HighDamage), Alpha))};
	}

	struct FTrialPreset
	{
		const TCHAR* Id;
		const TCHAR* Title;
		const TCHAR* Trait;
		const TCHAR* TraitName;
		EAttack Attack;
		float MinLength, Length, MinReach, Reach, Speed;
	};

	const FTrialPreset Presets[] = {
		{TEXT("short_sword"), TEXT("短剑"), TEXT("fire"), TEXT("灼烧"), EAttack::Sweep, 65,90,85,115,500},
		{TEXT("spear"), TEXT("长矛"), TEXT("push"), TEXT("重击退"), EAttack::Strike, 135,205,175,255,390},
		{TEXT("longsword"), TEXT("单手剑"), TEXT("precision"), TEXT("尖端精准"), EAttack::Sweep, 85,125,105,155,460},
		{TEXT("staff"), TEXT("长棍"), TEXT("frost"), TEXT("寒霜"), EAttack::Sweep, 110,165,135,195,420},
		{TEXT("axe"), TEXT("宽斧"), TEXT("push"), TEXT("重击退"), EAttack::Sweep, 100,145,125,185,380},
		{TEXT("hammer"), TEXT("重锤"), TEXT("shock"), TEXT("蓄响"), EAttack::Slam, 85,130,105,165,350},
		{TEXT("hook"), TEXT("弯钩"), TEXT("pull"), TEXT("短钩"), EAttack::Sweep, 90,135,115,170,400},
		{TEXT("gauntlet"), TEXT("拳套"), TEXT("quick"), TEXT("贴身快攻"), EAttack::Strike, 30,45,65,90,510},
		{TEXT("bow"), TEXT("弓"), TEXT("shatter"), TEXT("碎裂箭"), EAttack::Shot, 85,135,600,1200,410},
		{TEXT("hand_cannon"), TEXT("手炮"), TEXT("fire"), TEXT("烈焰弹"), EAttack::Shot, 50,90,300,700,360},
	};
	constexpr int32 TrialExamplePresets[] = {0, 1, 5, 8};
	const FName VisualTag(TEXT("GeneratedWeapon"));

	FVector Aim(const APlayerController& PC) { return FRotator(0, PC.GetControlRotation().Yaw, 0).Vector(); }
	FColor Tint(const FTrialPreset& Spec)
	{
		return Spec.Attack == EAttack::Shot || Spec.Attack == EAttack::Strike ? FColor(255, 170, 55) : FColor(71, 228, 255);
	}
	void Notice(int32 Key, const FString& Text, FColor Color, float Seconds)
	{
		if (!FParse::Param(FCommandLine::Get(), TEXT("WeaponTrialSelfTest"))) return;
		if (GEngine) GEngine->AddOnScreenDebugMessage(Key, Seconds, Color, Text);
	}

	AActor* SpawnTrialBarrier(UWorld* World, const FVector& Position, const FRotator& Rotation,
		const FVector& Extent, ECollisionChannel ObjectType, bool bShield)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor) return nullptr;
		if (bShield) Actor->Tags.Add(TEXT("ForgeShield"));
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Actor->SetRootComponent(Box);
		Actor->AddInstanceComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionObjectType(ObjectType);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Box->SetCollisionResponseToAllChannels(ECR_Ignore);
		Box->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Box->RegisterComponent();
		Actor->SetActorLocationAndRotation(Position, Rotation);
		return Actor;
	}
}

int32 UWeaponForgeSubsystem::FindTrialPreset(const FString& Id, const FString& Trait, double LengthM, double ReachM) const
{
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Presets); ++Index)
	{
		const FTrialPreset& Spec = Presets[Index];
		if (Id == Spec.Id && Trait == Spec.Trait && LengthM >= Spec.MinLength/100.0 && LengthM <= Spec.Length/100.0
			&& ReachM >= Spec.MinReach/100.0 && ReachM <= Spec.Reach/100.0) return Index;
	}
	return INDEX_NONE;
}

void UWeaponForgeSubsystem::CycleTrial()
{
	const int32 Next = TrialPreset == INDEX_NONE ? 0 : (static_cast<int32>(Presets[TrialPreset].Attack) + 1) % UE_ARRAY_COUNT(TrialExamplePresets);
	SelectTrial(TrialExamplePresets[Next]);
}

void UWeaponForgeSubsystem::SelectTrialSword() { SelectTrial(0); }
void UWeaponForgeSubsystem::SelectTrialSpear() { SelectTrial(1); }
void UWeaponForgeSubsystem::SelectTrialLongsword() { SelectTrial(2); }

void UWeaponForgeSubsystem::SelectTrial(int32 Preset, bool bShortSample)
{
	if (Panel || !Controller.IsValid() || Preset < 0 || Preset >= UE_ARRAY_COUNT(Presets)) return;
	ACharacter* Character = Cast<ACharacter>(Controller->GetPawn());
	if (!Character || !Character->GetCharacterMovement()) return;
	const FTrialPreset& Spec = Presets[Preset];
	const float Length = bShortSample ? Spec.MinLength : Spec.Length;
	const FString File = FString::Printf(TEXT("trial-%s.glb"), Preset == 0 ? TEXT("sword") : Spec.Id);
	TArray<uint8> Model;
	if (!FFileHelper::LoadFileToArray(Model, *(FPaths::ProjectContentDir() / TEXT("WeaponSamples") / File))
		|| !EquipGlb(Character, Model, Length, false, Preset, true))
	{
		Notice(6122, TEXT("试验武器加载失败"), FColor::Red, 4);
		return;
	}
	EquippedBytes = Model;
	EquippedLengthCm = Length;
	EquippedPreset = Preset;
	bReverseGrip = bEquippedReverseGrip = false;
	EquippedCharacter = Character;
	ActivateTrial(Character, Preset, bShortSample ? Spec.MinReach : Spec.Reach, Spec.Trait);
}

void UWeaponForgeSubsystem::ActivateTrial(ACharacter* Character, int32 Preset, float Reach, const FString& Trait)
{
	ClearTrial();
	if (GetWorld()->GetNetMode() != NM_Standalone) return;
	const FTrialPreset& Spec = Presets[Preset];
	TrialCharacter = Character;
	TrialPreset = Preset;
	TrialReachCm = Spec.Attack == EAttack::Shot ? Reach : FMath::Clamp(GripState.ModelStrikeReachCm + MeleeBodyReach, Spec.MinReach, Spec.Reach);
	const FAttackStats Stats = AttackStats(AttackProfile(Spec.Attack), TrialReachCm);
	TrialInterval = Stats.Interval;
	TrialDamage = Stats.Damage;
	TrialTrait = Trait;
	TrialTraitCooldown = 0;
	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	TrialOriginalSpeed = Movement->MaxWalkSpeed;
	Movement->MaxWalkSpeed = Spec.Speed;
	TArray<AActor*> Attached;
	Character->GetAttachedActors(Attached);
	for (AActor* Actor : Attached)
	{
		if (!Actor->ActorHasTag(VisualTag)) continue;
		TrialVisual = Actor;
		break;
	}
	ResetTrialTargets();
}

void UWeaponForgeSubsystem::ClearTrial()
{
	if (TrialShield.IsValid()) TrialShield->Destroy();
	TrialShield.Reset();
	if (ACharacter* Character = TrialCharacter.Get())
	{
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			Movement->MaxWalkSpeed = TrialOriginalSpeed;
			if (TrialAttackTime >= 0) Movement->bOrientRotationToMovement = bTrialRestoreOrientation;
		}
	}
	if (TrialVisual.IsValid() && TrialCharacter.IsValid()) PoseWeapon(TrialCharacter.Get(), TrialVisual.Get());
	TrialVisual.Reset();
	TrialCharacter.Reset();
	TrialProjectiles.Reset();
	TrialTargets.Reset();
	TrialAttackTime = -1;
	TrialPreset = INDEX_NONE;
}

void UWeaponForgeSubsystem::ResetTrialTargets()
{
	if (!TrialCharacter.IsValid() || !Controller.IsValid()) return;
	if (TrialShield.IsValid()) TrialShield->Destroy();
	TrialShield.Reset();
	if (TrialAttackTime >= 0) TrialCharacter->GetCharacterMovement()->bOrientRotationToMovement = bTrialRestoreOrientation;
	TrialAttackTime = -1;
	TrialProjectiles.Reset();
	TrialTraitCooldown = 0;
	TrialCharacter->GetCharacterMovement()->MaxWalkSpeed = Presets[TrialPreset].Speed;
	if (TrialVisual.IsValid()) PoseWeapon(TrialCharacter.Get(), TrialVisual.Get());
	const FVector Forward = Aim(*Controller.Get());
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Forward);
	TrialPatrolAxis = Right;
	const FVector Origin = TrialCharacter->GetActorLocation();
	const EAttack Attack = Presets[TrialPreset].Attack;
	const float Main = TrialReachCm * (Attack == EAttack::Slam ? .60f : Attack == EAttack::Shot ? .48f : .75f);
	const float Side = TrialReachCm * (Attack == EAttack::Sweep ? .52f : .7f);
	const bool bCluster = Attack == EAttack::Slam || Attack == EAttack::Shot;
	const float ClusterOffset = Attack == EAttack::Slam ? TrialReachCm * .25f : 60.f;
	const FVector SidePosition = bCluster ? Origin + Forward * Main + Right * ClusterOffset : Origin + Forward * (Main * .56f) + Right * Side;
	TrialTargets = {
		{Origin + Forward * Main},
		{Origin + Forward * (TrialReachCm * 1.15f)},
		{SidePosition},
	};
	Notice(6122, TEXT("靶子已重置"), FColor::White, 2);
}

void UWeaponForgeSubsystem::ToggleTrialShield()
{
	if (Panel || !TrialCharacter.IsValid() || !Controller.IsValid()) return;
	if (TrialShield.IsValid())
	{
		TrialShield->Destroy();
		TrialShield.Reset();
		return;
	}
	if (TrialTargets.IsEmpty()) return;
	const FVector Origin = TrialCharacter->GetActorLocation();
	const FVector ShieldPosition = FMath::Lerp(Origin, TrialTargets[0].Position, .60);
	const FRotator Rotation = (TrialTargets[0].Position - Origin).Rotation();
	TrialShield = SpawnTrialBarrier(GetWorld(), ShieldPosition, Rotation, ShieldHalfExtent, ECC_WorldDynamic, true);
}

void UWeaponForgeSubsystem::StartTrialAttack()
{
	if (Panel || TrialPreset == INDEX_NONE || TrialAttackTime >= 0 || !TrialCharacter.IsValid() || !Controller.IsValid()) return;
	ACharacter* Character = TrialCharacter.Get();
	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	const FAttackProfile& Profile = AttackProfile(Presets[TrialPreset].Attack);
	TrialForward = Aim(*Controller.Get());
	Character->SetActorRotation(TrialForward.Rotation());
	bTrialRestoreOrientation = Movement->bOrientRotationToMovement;
	Movement->bOrientRotationToMovement = false;
	TrialAttackTime = 0;
	bTrialHitResolved = false;
	TrialStrikeTime = TrialInterval * Profile.WindupFraction;
	TrialFinishTime = TrialInterval;
}

void UWeaponForgeSubsystem::ApplyTrialDamage(int32 TargetIndex, int32 Damage, const FVector& Direction, bool bPrimary)
{
	FTrialTarget& Target = TrialTargets[TargetIndex];
	if (Target.Health <= 0) return;
	const FTrialPreset& Spec = Presets[TrialPreset];
	if (TrialTraitCooldown <= 0 && bPrimary && !TrialTrait.IsEmpty())
	{
		if (TrialTrait == TEXT("fire")) { Target.BurnTime = 1; Target.BurnTicks = 3; }
		else if (TrialTrait == TEXT("push")) Target.Position += Direction * 45;
		else if (TrialTrait == TEXT("pull")) Target.Position -= Direction * 45;
		else if (TrialTrait == TEXT("precision")) Damage += 4;
		else if (TrialTrait == TEXT("quick")) TrialFinishTime = FMath::Max(TrialStrikeTime + .06f, TrialFinishTime - .04f);
		else if (TrialTrait == TEXT("frost")) Target.ChillTime = 1;
		else if (TrialTrait == TEXT("shock") || TrialTrait == TEXT("shatter"))
		{
			const FVector Center = Target.Position;
			DrawDebugSphere(GetWorld(), Center, 95, 12, Tint(Spec), false, .45f, 0, 3);
			for (int32 I = 0; I < TrialTargets.Num(); ++I)
			{
				if (I == TargetIndex || TrialTargets[I].Health <= 0) continue;
				const FVector Delta = TrialTargets[I].Position - Center;
				if (Delta.SizeSquared() <= 95 * 95) TrialTargets[I].Position += Delta.GetSafeNormal() * 22;
			}
		}
		TrialTraitCooldown = TrialTrait == TEXT("fire") || TrialTrait == TEXT("frost") ? 4.f : 2.5f;
	}
	Target.Health = FMath::Max(0, Target.Health - Damage);
	Target.Flash = .22f;
	DrawDebugSphere(GetWorld(), Target.Position, 28, 8, Tint(Spec), false, .22f, 0, 2);
	Notice(6123, FString::Printf(TEXT("命中  -%d"), Damage), Tint(Spec), 1);
}

void UWeaponForgeSubsystem::ResolveTrialHit()
{
	const FTrialPreset& Spec = Presets[TrialPreset];
	const FVector Origin = TrialCharacter->GetActorLocation();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, TrialForward);
	const FColor Color = Tint(Spec);
	if (Spec.Attack == EAttack::Shot)
	{
		TrialProjectiles.Add({Origin, TrialForward, TrialReachCm, 2200, TrialDamage});
		return;
	}
	const FVector Center = Origin + TrialForward * (.6f * TrialReachCm);
	if (Spec.Attack == EAttack::Slam) DrawDebugSphere(GetWorld(), Center, .4f * TrialReachCm, 16, Color, false, .35f, 0, 2);
	else if (Spec.Attack == EAttack::Strike)
	{
		for (const float Sign : {-1.f, 1.f})
		{
			const FVector Edge = Right * Sign * StrikeHalfWidth;
			DrawDebugLine(GetWorld(), Origin + Edge, Origin + Edge + TrialForward * TrialReachCm, Color, false, .28f, 0, 3);
		}
	}
	else
	{
		FVector Last = Origin + TrialForward.RotateAngleAxis(-SweepHalfAngle, FVector::UpVector) * TrialReachCm;
		for (int32 I = 1; I <= 12; ++I)
		{
			const FVector Next = Origin + TrialForward.RotateAngleAxis(-SweepHalfAngle + I * SweepHalfAngle / 6, FVector::UpVector) * TrialReachCm;
			DrawDebugLine(GetWorld(), Last, Next, Color, false, .24f, 0, 3);
			Last = Next;
		}
	}
	for (int32 I = 0; I < TrialTargets.Num(); ++I)
	{
		const FTrialTarget& Target = TrialTargets[I];
		FVector Delta = Target.Position - Origin;
		if (Target.Health <= 0 || FMath::Abs(Delta.Z) > 110) continue;
		Delta.Z = 0;
		const float Along = FVector::DotProduct(Delta, TrialForward);
		const float Side = FMath::Abs(FVector::DotProduct(Delta, Right));
		const float Dist = Delta.Size();
		int32 Damage = 0;
		bool bPrimary = true;
		if (Spec.Attack == EAttack::Strike && Along >= 25 && Along <= TrialReachCm && Side <= StrikeHalfWidth)
		{
			Damage = TrialDamage;
		}
		else if (Spec.Attack == EAttack::Slam)
		{
			const float Radial = FVector::Dist2D(Center, Target.Position);
			bPrimary = Radial <= TrialReachCm * .18f;
			if (Radial <= TrialReachCm * .4f) Damage = bPrimary ? TrialDamage : FMath::RoundToInt(TrialDamage * .4f);
		}
		else if (Spec.Attack == EAttack::Sweep && Dist <= TrialReachCm
			&& FVector::DotProduct(Delta.GetSafeNormal(), TrialForward) >= FMath::Cos(FMath::DegreesToRadians(SweepHalfAngle)))
		{
			Damage = TrialDamage;
			if (TrialTrait == TEXT("precision") || TrialTrait == TEXT("pull")) bPrimary = Dist >= TrialReachCm * .70f;
		}
		if (Damage <= 0) continue;
		FHitResult Barrier;
		if (TraceWeaponBarrier(TrialCharacter.Get(), Origin, Target.Position, 5, Barrier))
		{
			DrawDebugSphere(GetWorld(), Barrier.Location, 10, 8, FColor::Yellow, false, .25f);
			Notice(6123, TEXT("攻击被挡住"), FColor::Yellow, 1);
			continue;
		}
		ApplyTrialDamage(I, Damage, TrialForward, bPrimary);
	}
}

void UWeaponForgeSubsystem::UpdateTrialProjectiles(float DeltaTime)
{
	for (int32 I = TrialProjectiles.Num() - 1; I >= 0; --I)
	{
		FTrialProjectile& Shot = TrialProjectiles[I];
		const float Travel = FMath::Min(Shot.Distance, Shot.Speed * DeltaTime);
		const FVector IntendedEnd = Shot.Position + Shot.Direction * Travel;
		FHitResult Barrier;
		const bool bBlocked = TraceWeaponBarrier(TrialCharacter.Get(), Shot.Position, IntendedEnd, 4, Barrier);
		const FVector End = bBlocked ? Barrier.Location : IntendedEnd;
		int32 Hit = INDEX_NONE;
		float Nearest = bBlocked ? Travel * Barrier.Time : Travel + 1;
		for (int32 J = 0; J < TrialTargets.Num(); ++J)
		{
			const FTrialTarget& Target = TrialTargets[J];
			const float Along = FVector::DotProduct(Target.Position - Shot.Position, Shot.Direction);
			if (Target.Health > 0 && Along >= 0 && Along < Nearest
				&& FMath::PointDistToSegmentSquared(Target.Position, Shot.Position, End) <= 26 * 26)
			{
				Hit = J;
				Nearest = Along;
			}
		}
		DrawDebugLine(GetWorld(), Shot.Position, Hit == INDEX_NONE ? End : TrialTargets[Hit].Position,
			Tint(Presets[TrialPreset]), false, .12f, 0, 2);
		if (Hit != INDEX_NONE) ApplyTrialDamage(Hit, Shot.Damage, Shot.Direction, true);
		else if (bBlocked)
		{
			DrawDebugSphere(GetWorld(), End, 10, 8, FColor::Yellow, false, .25f);
			Notice(6123, TEXT("投射物被挡住"), FColor::Yellow, 1);
		}
		Shot.Position = End;
		Shot.Distance -= Travel;
		if (Hit != INDEX_NONE || bBlocked || Shot.Distance <= UE_SMALL_NUMBER) TrialProjectiles.RemoveAtSwap(I);
	}
}

void UWeaponForgeSubsystem::UpdateTrial(float DeltaTime)
{
#if !UE_BUILD_SHIPPING
	if (TrialTestPhase == 0 && FParse::Param(FCommandLine::Get(), TEXT("WeaponTrialSelfTest"))
		&& Controller.IsValid() && Cast<ACharacter>(Controller->GetPawn())
		&& Cast<ACharacter>(Controller->GetPawn())->GetCharacterMovement()->IsMovingOnGround())
	{
		TrialTestPhase = 1;
		SelectTrial(2, true);
		const float ShortReach = TrialReachCm, ShortInterval = TrialInterval;
		const int32 ShortDamage = TrialDamage;
		SelectTrial(2);
		const bool bRange = ShortReach < TrialReachCm && ShortInterval < TrialInterval
			&& ShortDamage < TrialDamage && ShortDamage / ShortInterval > TrialDamage / TrialInterval;
		UE_LOG(LogTemp, Display, TEXT("WeaponTrial reach scaling %s short=%.1fcm/%.2fs/%d long=%.1fcm/%.2fs/%d"),
			bRange ? TEXT("PASS") : TEXT("FAIL"), ShortReach, ShortInterval, ShortDamage, TrialReachCm, TrialInterval, TrialDamage);
		SelectTrial(0);
		ToggleTrialShield();
		ACharacter* Holder = TrialCharacter.Get();
		const FVector Origin = Holder->GetActorLocation();
		TrialForward = Aim(*Controller.Get());
		FHitResult Contact;
		const bool bShieldQuery = TrialShield.IsValid() && TraceWeaponBarrier(Holder, Origin, TrialTargets[0].Position, 5, Contact)
			&& Contact.GetActor() == TrialShield.Get();
		ResolveTrialHit();
		const bool bShieldBlock = bShieldQuery && TrialTargets[0].Health == 100;
		ToggleTrialShield();
		ResetTrialTargets();
		// An obstacle at the model's far end must never be left inside the visible weapon.
		bool bBodyClear = false;
		if (AActor* Visual = TrialVisual.Get(); Visual && !Visual->IsHidden() && !GripState.ModelContacts.IsEmpty())
		{
			const FTransform Before = Visual->GetActorTransform();
			const FVector Grip = Before.TransformPosition(GripState.ModelGrip);
			const FWeaponGripState::FContactSpan& Tip = GripState.ModelContacts.Last();
			const FVector TipPoint = Before.TransformPosition(Tip.End);
			AActor* Body = SpawnTrialBarrier(GetWorld(), TipPoint, FRotator::ZeroRotator, FVector(14), ECC_Pawn, false);
			const bool bDetected = Body && !WeaponPoseClear(Holder, Before, Grip);
			bBodyClear = bDetected;
			for (int32 Frame = 0; Frame < 12; ++Frame)
			{
				PoseWeapon(Holder, Visual, 0, Frame % 2 ? -3.f : 3.f);
				const FTransform Pose = Visual->GetActorTransform();
				bBodyClear &= !Visual->IsHidden() && WeaponPoseClear(Holder, Pose, Pose.TransformPosition(GripState.ModelGrip));
			}
			if (Body) Body->Destroy();
			PoseWeapon(Holder, Visual);
		}
		SelectTrial(8);
		TrialForward = Aim(*Controller.Get());
		const FVector WallCenter = FMath::Lerp(TrialCharacter->GetActorLocation(), TrialTargets[0].Position, .65);
		AActor* Wall = SpawnTrialBarrier(GetWorld(), WallCenter, TrialForward.Rotation(), FVector(8, 44, 70), ECC_WorldStatic, false);
		ResolveTrialHit();
		UpdateTrialProjectiles(TrialReachCm / 2200.f);
		const bool bWallBlock = Wall && TrialTargets[0].Health == 100 && TrialProjectiles.IsEmpty();
		if (Wall) Wall->Destroy();
		UE_LOG(LogTemp, Display, TEXT("WeaponTrial collision %s shield=%d body=%d wall=%d"),
			bShieldBlock && bBodyClear && bWallBlock ? TEXT("PASS") : TEXT("FAIL"), bShieldBlock, bBodyClear, bWallBlock);
		bTrialSwordPassed = bRange && bShieldBlock && bBodyClear && bWallBlock;
		SelectTrial(0);
		StartTrialAttack();
	}
#endif
	if (TrialPreset == INDEX_NONE)
	{
		if (!Panel && EquippedBytes.IsEmpty()) Notice(6120, TEXT("1 短剑  |  2 长矛  |  3 单手剑  |  F2 制作台  |  Tab 切换四种动作"), FColor::White, .2f);
		return;
	}
	if (!TrialCharacter.IsValid() || TrialCharacter.Get() != Controller->GetPawn())
	{
		if (Controller->GetPawn() && EquippedBytes.IsEmpty()) SelectTrial(TrialPreset);
		return;
	}
	const FTrialPreset& Spec = Presets[TrialPreset];
	if (!Panel)
		Notice(6120, FString::Printf(TEXT("%s · %s   范围 %.2f 米   间隔 %.2f 秒   伤害 %d   |   左键攻击   H 放盾   R 重摆"),
			Spec.Title, AttackProfile(Spec.Attack).Name, TrialReachCm / 100, TrialInterval, TrialDamage), Tint(Spec), .2f);
	TrialTraitCooldown = FMath::Max(0.f, TrialTraitCooldown - DeltaTime);

	if (TrialAttackTime >= 0)
	{
		TrialAttackTime += DeltaTime;
		if (!bTrialHitResolved && TrialAttackTime >= TrialStrikeTime)
		{
			bTrialHitResolved = true;
			ResolveTrialHit();
		}
		if (TrialVisual.IsValid())
		{
			const float T = TrialAttackTime;
			const float In = FMath::Clamp(T / TrialStrikeTime, 0.f, 1.f);
			const float Out = FMath::Clamp((TrialFinishTime - T) / (TrialFinishTime - TrialStrikeTime), 0.f, 1.f);
			float Pitch = 0, Yaw = 0;
			float Extension = 0;
			if (Spec.Attack == EAttack::Slam)
				Pitch = T < TrialStrikeTime ? 100 * FMath::Sin(PI * In) - 110 * FMath::Pow(In, 4) : -110 * Out;
			else if (Spec.Attack == EAttack::Sweep)
				Yaw = T < TrialStrikeTime ? FMath::Lerp(-SweepHalfAngle, 0.f, In) : SweepHalfAngle * FMath::Sin(PI * Out);
			else if (Spec.Attack == EAttack::Shot)
				Extension = T < TrialStrikeTime ? -12 * FMath::Sin(PI * In) : -22 * Out;
			else Extension = T < TrialStrikeTime ? -20 * FMath::Sin(PI * In) + 58 * FMath::Pow(In, 4) : 58 * Out;
			PoseWeapon(TrialCharacter.Get(), TrialVisual.Get(), Pitch, Yaw, Extension);
		}
		if (TrialAttackTime >= TrialFinishTime)
		{
			TrialAttackTime = -1;
			TrialCharacter->GetCharacterMovement()->bOrientRotationToMovement = bTrialRestoreOrientation;
			if (TrialVisual.IsValid() && TrialCharacter.IsValid()) PoseWeapon(TrialCharacter.Get(), TrialVisual.Get());
		}
	}
	if (TrialAttackTime < 0 && TrialVisual.IsValid()) PoseWeapon(TrialCharacter.Get(), TrialVisual.Get());
	UpdateTrialProjectiles(DeltaTime);
	if (TrialShield.IsValid())
		DrawDebugBox(GetWorld(), TrialShield->GetActorLocation(), ShieldHalfExtent, TrialShield->GetActorQuat(), FColor::Yellow, false, -1, 0, 2);
	for (int32 Index = 0; Index < TrialTargets.Num(); ++Index)
	{
		FTrialTarget& Target = TrialTargets[Index];
		Target.Flash = FMath::Max(0.f, Target.Flash - DeltaTime);
		Target.ChillTime = FMath::Max(0.f, Target.ChillTime - DeltaTime);
		if (Index == 2 && Target.Health > 0)
		{
			const float Previous = Target.PatrolOffset;
			const float Speed = Target.ChillTime > 0 ? 24.6f : 30.f;
			Target.PatrolOffset = FMath::Clamp(Previous + Target.PatrolDirection * Speed * DeltaTime, -24.f, 24.f);
			if (FMath::Abs(Target.PatrolOffset) >= 24.f) Target.PatrolDirection = -Target.PatrolDirection;
			Target.Position += TrialPatrolAxis * (Target.PatrolOffset - Previous);
		}
		if (Target.BurnTicks > 0 && Target.Health > 0)
		{
			Target.BurnTime -= DeltaTime;
			if (Target.BurnTime <= 0)
			{
				Target.BurnTime += 1;
				--Target.BurnTicks;
				Target.Health = FMath::Max(0, Target.Health - 2);
				Notice(6123, TEXT("灼烧  -2"), FColor(255, 96, 35), 1);
			}
		}
		const FColor Color = Target.Flash > 0 ? Tint(Spec) : Target.Health == 0 ? FColor::Silver
			: Target.BurnTicks > 0 ? FColor(255, 96, 35) : Target.ChillTime > 0 ? FColor::Cyan : FColor::Green;
		DrawDebugCapsule(GetWorld(), Target.Position, 45, 21, FQuat::Identity, Color, false, -1, 0, 2);
	}
	if (!Panel && TrialTargets.Num() == 3)
		Notice(6121, FString::Printf(TEXT("靶子生命    近 %d   远 %d   侧 %d   |   %s"), TrialTargets[0].Health, TrialTargets[1].Health,
			TrialTargets[2].Health, Spec.TraitName), FColor::White, .2f);

#if !UE_BUILD_SHIPPING
	if (TrialTestPhase > 0 && TrialTestPhase <= UE_ARRAY_COUNT(Presets) && TrialAttackTime < 0 && TrialProjectiles.IsEmpty())
	{
		if (TrialTestDelay == -1)
		{
		// A nearby body can deliberately deflect the pose. Check the roll axis
		// with the targets moved aside, then restore the combat result.
		const TArray<FTrialTarget> HitTargets = MoveTemp(TrialTargets);
		TrialTargets.Reset();
		if (TrialVisual.IsValid()) PoseWeapon(TrialCharacter.Get(), TrialVisual.Get());
		const FVector OriginalAxis = TrialVisual.IsValid() ? TrialVisual->GetActorQuat().RotateVector(GripState.ModelAxis) : FVector::ZeroVector;
		RotateGrip();
		const bool bRollValid = TrialVisual.IsValid() && FVector::DotProduct(OriginalAxis, TrialVisual->GetActorQuat().RotateVector(GripState.ModelAxis)) > .999;
		for (int32 Turn = 0; Turn < 3; ++Turn) RotateGrip();
		TrialTargets = HitTargets;
		bool bTraitValid = TrialTraitCooldown > 0;
		if (TrialTrait == TEXT("fire")) bTraitValid &= TrialTargets[0].BurnTicks > 0;
		if (TrialTrait == TEXT("frost")) bTraitValid &= TrialTargets[0].ChillTime > 0;
		if (TrialTrait == TEXT("quick")) bTraitValid &= TrialFinishTime < TrialInterval;
		if (TrialTrait == TEXT("precision")) bTraitValid &= TrialTargets[0].Health == 100 - TrialDamage - 4;
		if (TrialTrait == TEXT("push") || TrialTrait == TEXT("pull"))
		{
			const float Along = FVector::DotProduct(TrialTargets[0].Position - TrialCharacter->GetActorLocation(), TrialForward);
			bTraitValid &= FMath::Abs(Along - TrialReachCm * .75f) > 40;
		}
		const bool bPass = bRollValid && bTraitValid && TrialPreset == TrialTestPhase - 1 && TrialTargets.Num() == 3
			&& TrialTargets[0].Health < 100 && TrialTargets[1].Health == 100
			&& TrialCharacter->GetCharacterMovement()->MaxWalkSpeed == Spec.Speed
			&& TrialVisual.IsValid()
			&& FVector::Dist(TrialVisual->GetActorTransform().TransformPosition(GripState.ModelGrip), TrialCharacter->GetMesh()->GetSocketLocation(TEXT("hand_r"))) < 20;
		bTrialSwordPassed &= bPass;
		UE_LOG(LogTemp, Display, TEXT("WeaponTrial %s %s center=%d trait=%d roll=%d"), Spec.Id, bPass ? TEXT("PASS") : TEXT("FAIL"),
			TrialTargets.IsEmpty() ? 100 : TrialTargets[0].Health, bTraitValid, bRollValid);
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("WeaponCatalogChecks");
		IFileManager::Get().MakeDirectory(*Directory, true);
		Controller->SetControlRotation(FRotator(-12, TrialCharacter->GetActorRotation().Yaw - 40, 0));
		TrialTestDelay = .6f;
		return;
		}
		if (TrialTestDelay > 0)
		{
			TrialTestDelay -= DeltaTime;
			if (TrialTestDelay > 0) return;
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("WeaponCatalogChecks") / FString(Spec.Id), true, false);
			TrialTestDelay = -2;
			return;
		}
		TrialTestDelay = -1;
		if (TrialTestPhase == UE_ARRAY_COUNT(Presets))
		{
			UE_LOG(LogTemp, Display, TEXT("WeaponTrial catalog %s"), bTrialSwordPassed ? TEXT("PASS") : TEXT("FAIL"));
			++TrialTestPhase;
			FPlatformMisc::RequestExit(false);
		}
		else
		{
			Controller->SetControlRotation(FRotator(0, TrialCharacter->GetActorRotation().Yaw, 0));
			SelectTrial(TrialTestPhase++);
			StartTrialAttack();
		}
	}
#endif
}
