#include "WeaponForgeSubsystem.h"
#include "WeaponGripState.h"
#include "WeaponSyncComponent.h"
#include "PlayerCombatComponent.h"
#include "glTFRuntimeAssetActor.h"
#include "glTFRuntimeFunctionLibrary.h"
#include "Misc/App.h"

#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#include "glTFRuntimeParser.h"

namespace
{
	struct FGripProfile
	{
		float Along;
		FVector Forward;
		FVector Offset;
	};
	// Forward is in character space. Each model has its own source frame.
	const FGripProfile Profiles[] = {
		{.14f, FVector(.92, 0, .4), FVector(3, 0, 1)},
		{.26f, FVector(1, 0, .08), FVector(3, 0, 0)},
		{.13f, FVector(.7, 0, .7), FVector(4, 0, 0)},
		{.40f, FVector(.9, 0, .25), FVector(4, 0, 0)},
		{.16f, FVector(.5, 0, .86), FVector(3, 0, 1)},
		{.18f, FVector(.3, 0, .95), FVector(3, 0, 0)},
		{.20f, FVector(.5, 0, .86), FVector(3, 0, 1)},
		{.32f, FVector(1, 0, .02), FVector(1, 0, 0)},
		{.50f, FVector(.05, 0, 1), FVector(8, 0, 0)},
		{.18f, FVector(1, 0, 0), FVector(3, 0, 1)},
	};

	// Convert authored glTF metadata with exactly the same basis as the loader.
	bool ReadGripMetadata(const TArray<uint8>& Bytes, FVector& Grip, FVector& Axis, FVector& Normal)
	{
		if (Bytes.Num() < 20) return false;
		uint32 Length = 0;
		FMemory::Memcpy(&Length, Bytes.GetData() + 12, sizeof(Length));
		if (Length == 0 || Length > uint32(Bytes.Num() - 20)) return false;
		FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 20), Length);
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Json) || !Json) return false;
		const TSharedPtr<FJsonObject>* Extras = nullptr;
		const TSharedPtr<FJsonObject>* Metadata = nullptr;
		if (!Json->TryGetObjectField(TEXT("extras"), Extras) || !(*Extras)->TryGetObjectField(TEXT("weapon_grip"), Metadata)) return false;
		auto Vector = [&](const TCHAR* Key, FVector& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
			if (!(*Metadata)->TryGetArrayField(Key, Data) || Data->Num() != 3) return false;
			double X, Y, Z;
			if (!(*Data)[0]->TryGetNumber(X) || !(*Data)[1]->TryGetNumber(Y) || !(*Data)[2]->TryGetNumber(Z)
				|| !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z)) return false;
			Out = FVector(X, Y, Z);
			return true;
		};
		if (!Vector(TEXT("point"), Grip) || !Vector(TEXT("axis"), Axis) || !Vector(TEXT("normal"), Normal)) return false;
		const FglTFRuntimeConfig Config;
		Grip = Config.GetMatrix().TransformPosition(Grip * Config.SceneScale);
		Axis = Config.GetMatrix().TransformVector(Axis).GetSafeNormal();
		Normal = Config.GetMatrix().TransformVector(Normal).GetSafeNormal();
		return !Axis.IsNearlyZero() && !Normal.IsNearlyZero() && FMath::Abs(FVector::DotProduct(Axis, Normal)) < .95;
	}
}

bool FWeaponGripState::PrepareGrip(AActor* Visual, const TArray<uint8>& Bytes, float RequestedLengthCm, bool bReverse, int32 Preset)
{
	TInlineComponentArray<UStaticMeshComponent*> Parts(Visual);
	TArray<FVector> Vertices;
	FBox Bounds(ForceInit);
	for (UStaticMeshComponent* Part : Parts)
	{
		if (!Part->GetStaticMesh()) continue;
		const FStaticMeshRenderData* Data = Part->GetStaticMesh()->GetRenderData();
		if (!Data || Data->LODResources.IsEmpty()) continue;
		const auto& Buffer = Data->LODResources[0].VertexBuffers.PositionVertexBuffer;
		if (!Buffer.GetNumVertices()) continue;
		const uint32 Step = FMath::Max(1u, Buffer.GetNumVertices() / 12000);
		for (uint32 I = 0; I < Buffer.GetNumVertices(); I += Step)
		{
			const FVector Point = Part->GetComponentTransform().TransformPosition(FVector(Buffer.VertexPosition(I)));
			if (Point.ContainsNaN()) continue;
			Vertices.Add(Point);
			Bounds += Point;
		}
	}
	if (!Bounds.IsValid || Vertices.Num() < 3) return false;
	ModelGrip = FVector::ZeroVector;
	ModelAxis = FVector::UpVector;
	ModelNormal = FVector::ForwardVector;

	if (!ReadGripMetadata(Bytes, ModelGrip, ModelAxis, ModelNormal)
		|| !Bounds.ExpandBy(Bounds.GetSize().Size() * .05 + 1).IsInsideOrOn(ModelGrip))
	{
		// Recover a rigid model frame from geometry, including models exported at an angle.
		const FVector Center = Bounds.GetCenter();
		const FVector Extent = Bounds.GetSize();
		int32 Longest = Extent.Y > Extent.X ? 1 : 0;
		if (Extent.Z > Extent[Longest]) Longest = 2;
		ModelAxis = FVector::ZeroVector;
		ModelAxis[Longest] = 1;
		auto Multiply = [&Vertices, &Center](const FVector& Axis)
		{
			FVector Value = FVector::ZeroVector;
			for (const FVector& V : Vertices) Value += (V - Center) * FVector::DotProduct(V - Center, Axis);
			return Value;
		};
		for (int32 I = 0; I < 14; ++I) ModelAxis = Multiply(ModelAxis).GetSafeNormal();
		if (ModelAxis.IsNearlyZero()) return false;
		if (ModelAxis[Longest] < 0) ModelAxis *= -1;
		FVector Wide = FMath::Abs(ModelAxis.Z) < .8 ? FVector::UpVector : FVector::ForwardVector;
		Wide = FVector::VectorPlaneProject(Wide, ModelAxis).GetSafeNormal();
		for (int32 I = 0; I < 14; ++I) Wide = FVector::VectorPlaneProject(Multiply(Wide), ModelAxis).GetSafeNormal();
		if (Wide.IsNearlyZero()) ModelAxis.FindBestAxisVectors(Wide, ModelNormal);
		ModelNormal = FVector::CrossProduct(ModelAxis, Wide).GetSafeNormal();
		// Fix the normal sign so repeated loads cannot turn a blade edge arbitrarily.
		int32 NormalAxis = ModelNormal.GetAbs().X > ModelNormal.GetAbs().Y ? 0 : 1;
		if (FMath::Abs(ModelNormal.Z) > FMath::Abs(ModelNormal[NormalAxis])) NormalAxis = 2;
		if (ModelNormal[NormalAxis] < 0) ModelNormal *= -1;

		float Low = FLT_MAX, High = -FLT_MAX;
		for (const FVector& V : Vertices)
		{
			const float Along = FVector::DotProduct(V - Center, ModelAxis);
			Low = FMath::Min(Low, Along);
			High = FMath::Max(High, Along);
		}
		const float Span = High - Low;
		if (Span <= UE_SMALL_NUMBER) return false;
		FBox LowEnd(ForceInit), HighEnd(ForceInit);
		for (const FVector& V : Vertices)
		{
			const float At = (FVector::DotProduct(V - Center, ModelAxis) - Low) / Span;
			const FVector P(FVector::DotProduct(V - Center, Wide), FVector::DotProduct(V - Center, ModelNormal), 0);
			if (At >= .08f && At <= .30f) LowEnd += P;
			if (At >= .70f && At <= .92f) HighEnd += P;
		}
		if (LowEnd.IsValid && HighEnd.IsValid && HighEnd.GetSize().SizeSquared() < LowEnd.GetSize().SizeSquared() * .65)
		{
			ModelAxis *= -1;
			const float OldLow = Low;
			Low = -High;
			High = -OldLow;
		}
		const float Along = Preset >= 0 && Preset < UE_ARRAY_COUNT(Profiles) ? Profiles[Preset].Along : .15f;
		const float GripAt = Low + (High - Low) * Along;
		FVector GripSum = FVector::ZeroVector;
		int32 Count = 0;
		for (const FVector& V : Vertices)
		{
			const float At = FVector::DotProduct(V - Center, ModelAxis);
			if (FMath::Abs(At - GripAt) <= Span * .10)
			{
				GripSum += V - ModelAxis * (At - GripAt);
				++Count;
			}
		}
		ModelGrip = Count ? GripSum / Count : Center + ModelAxis * GripAt;
	}

	float Low = FLT_MAX, High = -FLT_MAX;
	for (const FVector& V : Vertices)
	{
		const float At = FVector::DotProduct(V, ModelAxis);
		Low = FMath::Min(Low, At);
		High = FMath::Max(High, At);
	}
	if (High - Low <= UE_SMALL_NUMBER) return false;
	ModelScale = RequestedLengthCm / (High - Low);
	ModelStrikeReachCm = FMath::Clamp(High - static_cast<float>(FVector::DotProduct(ModelGrip, ModelAxis)), 0.f, High - Low) * ModelScale;
	const FVector Depth = FVector::VectorPlaneProject(ModelNormal, ModelAxis).GetSafeNormal();
	const FVector Width = FVector::CrossProduct(ModelAxis, Depth).GetSafeNormal();
	constexpr int32 NumSections = 5;
	FBox Sections[NumSections] = {FBox(ForceInit), FBox(ForceInit), FBox(ForceInit), FBox(ForceInit), FBox(ForceInit)};
	for (const FVector& Vertex : Vertices)
	{
		const float Along = FVector::DotProduct(Vertex, ModelAxis);
		const int32 Index = FMath::Clamp(FMath::FloorToInt((Along - Low) * NumSections / (High - Low)), 0, NumSections - 1);
		Sections[Index] += FVector(Along, FVector::DotProduct(Vertex, Width), FVector::DotProduct(Vertex, Depth));
	}
	ModelContacts.Reset();
	for (const FBox& Section : Sections)
	{
		if (!Section.IsValid) continue;
		const FVector Center = Section.GetCenter();
		const FVector Offset = Width * Center.Y + Depth * Center.Z;
		const float Radius = FMath::Max(2.f, static_cast<float>(FVector2D(Section.GetExtent().Y, Section.GetExtent().Z).Size()) * ModelScale);
		const FVector Start = ModelAxis * Section.Min.X + Offset;
		if (!ModelContacts.IsEmpty() && FVector::DistSquared(ModelContacts.Last().End, Start) * FMath::Square(ModelScale) > 4)
		{
			const FContactSpan Previous = ModelContacts.Last();
			ModelContacts.Add({Previous.End, Start, FMath::Min(Previous.Radius, Radius)});
		}
		ModelContacts.Add({Start, ModelAxis * Section.Max.X + Offset, Radius});
	}
	if (bReverse) ModelAxis *= -1;
	EquippedGripPreset = Preset;
	GripVisual = Visual;
	GripRoll = 0;
	GripShift = 0;
	return true;
}

bool FWeaponGripState::TraceWeaponBarrier(const ACharacter* Holder, const FVector& Start, const FVector& End,
	float Radius, FHitResult& Hit) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ForgeWeaponBarrier), false, Holder);
	if (GripVisual.IsValid()) Params.AddIgnoredActor(GripVisual.Get());
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	Objects.AddObjectTypesToQuery(ECC_Pawn);
	TArray<FHitResult> Hits;
	GetWorld()->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, Objects, FCollisionShape::MakeSphere(Radius), Params);
	bool bFound = false;
	for (const FHitResult& Candidate : Hits)
	{
		const UPrimitiveComponent* Component = Candidate.GetComponent();
		if (!Component) continue;
		const AActor* Actor = Candidate.GetActor();
		const bool bShield = Component->ComponentHasTag(TEXT("ForgeShield")) || (Actor && Actor->ActorHasTag(TEXT("ForgeShield")));
		if (!bShield && Component->GetCollisionObjectType() != ECC_Pawn
			&& Component->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block) continue;
		if (!bFound || Candidate.Time < Hit.Time)
		{
			Hit = Candidate;
			bFound = true;
		}
	}
	return bFound;
}

bool FWeaponGripState::WeaponPoseClear(const ACharacter* Holder, const FTransform& Pose, const FVector& Grip) const
{
	for (const FContactSpan& Span : ModelContacts)
	{
		const FVector Start = Pose.TransformPosition(Span.Start);
		const FVector End = Pose.TransformPosition(Span.End);
		FHitResult Contact;
		if (TraceWeaponBarrier(Holder, Start, End, Span.Radius, Contact)) return false;
		const UCapsuleComponent* Body = Holder->GetCapsuleComponent();
		if (!Body || EquippedGripPreset == 7) continue; // The gauntlet is worn over the hand.
		const float BodyRadius = Body->GetScaledCapsuleRadius();
		const FVector Center = Body->GetComponentLocation();
		const FVector Height = Body->GetUpVector() * FMath::Max(0.f, Body->GetScaledCapsuleHalfHeight() - BodyRadius);
		for (const FVector& Point : {Start, (Start + End) * .5, End})
		{
			if (FVector::DistSquared(Point, Grip) <= FMath::Square(22.f + Span.Radius)) continue;
			if (FMath::PointDistToSegmentSquared(Point, Center - Height, Center + Height)
				< FMath::Square(BodyRadius + Span.Radius)) return false;
		}
	}
	return true;
}

void FWeaponGripState::PoseWeapon(ACharacter* Character, AActor* Visual, float Pitch, float Yaw, float Extension)
{
	if (!Character || !Visual || GripVisual.Get() != Visual) return;
	if (const auto* Combat = Character->FindComponentByClass<UPlayerCombatComponent>(); Combat && Combat->GetHealth() <= 0)
	{
		Visual->SetActorHiddenInGame(true);
		return;
	}
	const FGripProfile Profile = EquippedGripPreset >= 0 && EquippedGripPreset < UE_ARRAY_COUNT(Profiles)
		? Profiles[EquippedGripPreset] : FGripProfile{.15f, FVector(.8, 0, .6), FVector(3, 0, 0)};
	const FQuat CharacterRotation = FRotator(0, Character->GetActorRotation().Yaw, 0).Quaternion();
	const FQuat Sweep = FRotator(Pitch, Yaw, 0).Quaternion();
	const FVector Direction = CharacterRotation.RotateVector(Sweep.RotateVector(Profile.Forward.GetSafeNormal()));
	FVector Up = CharacterRotation.RotateVector(Sweep.RotateVector(EquippedGripPreset == 8 ? FVector::ForwardVector : FVector::UpVector));
	if (FMath::Abs(FVector::DotProduct(Direction, Up)) > .98) Up = CharacterRotation.RotateVector(FVector::RightVector);
	Up = FQuat(Direction, FMath::DegreesToRadians(GripRoll)).RotateVector(Up);
	const FQuat TargetFrame = FRotationMatrix::MakeFromXZ(Direction, Up).ToQuat();
	const FQuat SourceFrame = FRotationMatrix::MakeFromXZ(ModelAxis, ModelNormal).ToQuat();
	const FQuat Rotation = TargetFrame * SourceFrame.Inverse();
	const FVector Grip = Character->GetMesh()->GetSocketLocation(TEXT("hand_r"))
		+ CharacterRotation.RotateVector(Profile.Offset + FVector(FMath::Clamp(Extension, -4.f, 4.f), 0, 0));
	const FVector AdjustedGrip = ModelGrip + ModelAxis * (GripShift / ModelScale);
	const auto AtHand = [&](const FQuat& Orientation, const FVector& Anchor)
	{
		return FTransform(Orientation, Anchor - Orientation.RotateVector(AdjustedGrip * ModelScale), FVector(ModelScale));
	};
	FTransform SafePose = AtHand(Rotation, Grip);
	bool bClear = WeaponPoseClear(Character, SafePose, Grip);
	const FQuat Previous = Visual->GetActorQuat();
	const double Turn = FMath::RadiansToDegrees(Previous.AngularDistance(Rotation));
	const bool bEquipped = Visual->ActorHasTag(TEXT("GeneratedWeapon"));
	if (bEquipped && (!bClear || Turn > 10))
	{
		const FTransform Reanchored = AtHand(Previous, Grip);
		if (WeaponPoseClear(Character, Reanchored, Grip))
		{
			SafePose = Reanchored;
			bClear = true;
			const int32 Steps = FMath::Clamp(FMath::CeilToInt(Turn / 10), 1, 8);
			for (int32 Step = 1; Step <= Steps; ++Step)
			{
				const FTransform Pose = AtHand(FQuat::Slerp(Previous, Rotation, float(Step) / Steps), Grip);
				if (!WeaponPoseClear(Character, Pose, Grip)) break;
				SafePose = Pose;
			}
		}
	}
	if (!bClear)
	{
		const FVector Right = CharacterRotation.RotateVector(FVector::RightVector);
		for (const float Degrees : {-28.f, 28.f, -55.f, 55.f, -80.f, 80.f})
		{
			SafePose = AtHand(FQuat(Right, FMath::DegreesToRadians(Degrees)) * Rotation, Grip);
			if (WeaponPoseClear(Character, SafePose, Grip)) { bClear = true; break; }
		}
	}
	if (!bClear)
	{
		const FVector Right = CharacterRotation.GetRightVector();
		const FVector Forward = CharacterRotation.GetForwardVector();
		const FVector Outward = FVector::DotProduct(Grip - Character->GetActorLocation(), Right) < 0 ? -Right : Right;
		const FQuat Guard = FRotationMatrix::MakeFromXZ((FVector::UpVector + .30 * Outward).GetSafeNormal(), Forward).ToQuat()
			* SourceFrame.Inverse();
		for (const FVector& Anchor : {Grip, Grip + Outward * 5.f, Grip + Outward * 10.f, Grip + Forward * 8.f})
		{
			SafePose = AtHand(Guard, Anchor);
			if (WeaponPoseClear(Character, SafePose, Anchor)) { bClear = true; break; }
		}
	}
	// A fully enclosed hand can have no clear pose. Keep it visible and anchored.
	if (Visual->IsHidden()) Visual->SetActorHiddenInGame(false);
	Visual->SetActorTransform(bClear ? SafePose : AtHand(bEquipped ? Previous : Rotation, Grip));
	TInlineComponentArray<USceneComponent*> Markers(Visual);
	for (USceneComponent* Marker : Markers)
		if (Marker->ComponentHasTag(TEXT("ForgeGripAnchor"))) Marker->SetRelativeLocation(AdjustedGrip);
}


bool FWeaponGripState::Equip(ACharacter* Character, const TArray<uint8>& Bytes, float LengthCm, bool bReverse, int32 Preset)
{
	if (!FApp::CanEverRender() || !IsValid(Character) || Bytes.Num() < 20 || Bytes.Num() > 32*1024*1024 || FMemory::Memcmp(Bytes.GetData(), "glTF", 4) != 0 || !FMath::IsFinite(LengthCm) || LengthCm < 25 || LengthCm > 220) return false;
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	if (!Mesh || !Mesh->DoesSocketExist(TEXT("hand_r"))) return false;
	FglTFRuntimeConfig Loader; Loader.bAllowExternalFiles = false;
	auto* Asset = UglTFRuntimeFunctionLibrary::glTFLoadAssetFromData(Bytes, Loader);
	if (!Asset) return false;
	auto* Visual = Character->GetWorld()->SpawnActorDeferred<AglTFRuntimeAssetActor>(AglTFRuntimeAssetActor::StaticClass(), FTransform::Identity, Character, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Visual) return false;
	Visual->Asset = Asset; Visual->bAllowCameras = false; Visual->bAllowLights = false; Visual->bAllowNodeAnimations = false; Visual->bAllowSkeletalAnimations = false; Visual->StaticMeshConfig.bAllowCPUAccess = true; Visual->SetActorEnableCollision(false);
	Visual->FinishSpawning(FTransform::Identity);
	FWeaponGripState Next; Next.World = Character->GetWorld();
	if (!Next.PrepareGrip(Visual, Bytes, LengthCm, bReverse, Preset) || !Visual->AttachToComponent(Mesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, TEXT("hand_r"))) { Visual->Destroy(); return false; }
	auto* Marker = NewObject<USceneComponent>(Visual, TEXT("WeaponGripMarker"));
	Marker->ComponentTags.Add(TEXT("ForgeGripAnchor")); Marker->RegisterComponent();
	Marker->AttachToComponent(Visual->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform); Marker->SetRelativeLocation(Next.ModelGrip);
	Next.PoseWeapon(Character, Visual);
	TArray<AActor*> Attached; Character->GetAttachedActors(Attached);
	for (AActor* Old : Attached) if (Old != Visual && Old->ActorHasTag(TEXT("GeneratedWeapon"))) Old->Destroy();
	Visual->Tags.Add(TEXT("GeneratedWeapon"));
	*this = MoveTemp(Next);
	return true;
}

void FWeaponGripState::Reset()
{
	if (GripVisual.IsValid()) GripVisual->Destroy();
	*this = FWeaponGripState();
}

void UWeaponForgeSubsystem::PoseWeapon(ACharacter* Character, AActor* Visual, float Pitch, float Yaw, float Extension)
{
 if (Character)
  if (auto* Combat = Character->FindComponentByClass<UPlayerCombatComponent>())
  {
   float AttackPitch = 0, AttackYaw = 0, AttackExtension = 0;
   Combat->GetAttackPose(AttackPitch, AttackYaw, AttackExtension);
   Pitch += AttackPitch; Yaw += AttackYaw; Extension += AttackExtension;
  }
 GripState.PoseWeapon(Character, Visual, Pitch, Yaw, Extension);
}
bool UWeaponForgeSubsystem::TraceWeaponBarrier(const ACharacter* Holder, const FVector& Start, const FVector& End, float Radius, FHitResult& Hit) const { return GripState.TraceWeaponBarrier(Holder, Start, End, Radius, Hit); }
void UWeaponForgeSubsystem::RotateGrip()
{
	GripState.GripRoll = FMath::Fmod(GripState.GripRoll + 90, 360);
	if (EquippedCharacter.IsValid() && GripState.GripVisual.IsValid()) PoseWeapon(EquippedCharacter.Get(), GripState.GripVisual.Get());
	if (EquippedCharacter.IsValid()) if (auto* Sync = EquippedCharacter->FindComponentByClass<UWeaponSyncComponent>()) Sync->PublishGrip(GripState.GripRoll, GripState.GripShift);
}
void UWeaponForgeSubsystem::AdjustGripForward()
{
	GripState.GripShift = FMath::Clamp(GripState.GripShift + 2, -6.f, 6.f);
	if (EquippedCharacter.IsValid() && GripState.GripVisual.IsValid()) PoseWeapon(EquippedCharacter.Get(), GripState.GripVisual.Get());
	if (EquippedCharacter.IsValid()) if (auto* Sync = EquippedCharacter->FindComponentByClass<UWeaponSyncComponent>()) Sync->PublishGrip(GripState.GripRoll, GripState.GripShift);
}
void UWeaponForgeSubsystem::AdjustGripBack()
{
	GripState.GripShift = FMath::Clamp(GripState.GripShift - 2, -6.f, 6.f);
	if (EquippedCharacter.IsValid() && GripState.GripVisual.IsValid()) PoseWeapon(EquippedCharacter.Get(), GripState.GripVisual.Get());
	if (EquippedCharacter.IsValid()) if (auto* Sync = EquippedCharacter->FindComponentByClass<UWeaponSyncComponent>()) Sync->PublishGrip(GripState.GripRoll, GripState.GripShift);
}

bool UWeaponForgeSubsystem::WeaponPoseClear(const ACharacter* Holder, const FTransform& Pose, const FVector& Grip) const { return GripState.WeaponPoseClear(Holder, Pose, Grip); }
