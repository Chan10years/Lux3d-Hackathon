#include "GlbValidationSubsystem.h"

#include "Components/MeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogGLBValidation, Log, All);

namespace
{
	constexpr double WatchdogSeconds = 65.0;
	const FName HandSocket(TEXT("hand_r"));
	TSharedPtr<FJsonObject> VectorJson(const FVector& Value)
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Value.X);
		Object->SetNumberField(TEXT("y"), Value.Y);
		Object->SetNumberField(TEXT("z"), Value.Z);
		return Object;
	}
}

bool UGlbValidationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return FParse::Param(FCommandLine::Get(), TEXT("GLBSelfTest"))
		&& World && World->WorldType == EWorldType::Game
		&& Super::ShouldCreateSubsystem(Outer);
}

void UGlbValidationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bEnabled = FParse::Param(FCommandLine::Get(), TEXT("GLBSelfTest"));
	if (!bEnabled) return;
	StartedAt = FPlatformTime::Seconds();
	FParse::Value(FCommandLine::Get(), TEXT("GLBRun="), RunId);
	if (RunId.IsEmpty()) RunId = TEXT("default");
	// Treat the run name as one directory component, never as a supplied path.
	RunId = FPaths::MakeValidFileName(RunId);
	if (RunId.IsEmpty() || RunId == TEXT(".") || RunId == TEXT("..")) RunId = TEXT("default");
	OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("RuntimeGLB_Verification"), RunId));
	bOutputDirectoryReady = IFileManager::Get().MakeDirectory(*OutputDirectory, true);
	IFileManager::Get().Delete(*FPaths::Combine(OutputDirectory, TEXT("report.json")));
	FApp::SetVolumeMultiplier(0.0f);
	PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(
		this, &UGlbValidationSubsystem::AfterActorTick);
	UE_LOG(LogGLBValidation, Display, TEXT("GLB self-test started: %s"), *OutputDirectory);
}

void UGlbValidationSubsystem::Deinitialize()
{
	FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
	bEnabled = false;
	Super::Deinitialize();
}

bool UGlbValidationSubsystem::IsTickable() const
{
	return bEnabled && !bFinished && !IsTemplate();
}

TStatId UGlbValidationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGlbValidationSubsystem, STATGROUP_Tickables);
}

void UGlbValidationSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// Wall-clock watchdog also covers a world that has not begun play.
	if (bEnabled && !bFinished && FPlatformTime::Seconds() - StartedAt >= WatchdogSeconds)
	{
		Finish(TEXT("Watchdog: objects unavailable or verification did not finish"));
	}
}

void UGlbValidationSubsystem::AfterActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (!bEnabled || bFinished || World != GetWorld() || !World->HasBegunPlay()) return;
	const double Now = FPlatformTime::Seconds();
	if (Now - StartedAt >= WatchdogSeconds)
	{
		Finish(TEXT("Watchdog: verification did not finish"));
		return;
	}
	if (!bOutputDirectoryReady)
	{
		Finish(TEXT("Could not create report directory"));
		return;
	}
	if (!bTracking)
	{
		LastActorCount = 0;
		AActor* Candidate = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			// No compile-time dependency on glTFRuntime; match its actual native class.
			if (It->GetClass()->GetPathName() == TEXT("/Script/glTFRuntime.glTFRuntimeAssetActor"))
			{
				++LastActorCount;
				Candidate = *It;
			}
		}
		ACharacter* Player = UGameplayStatics::GetPlayerCharacter(World, 0);
		if (LastActorCount != 1 || !Candidate || !Candidate->GetRootComponent()
			|| !Player || !Player->GetMesh() || !Player->GetCharacterMovement()
			|| !Player->GetCharacterMovement()->IsMovingOnGround()) return;
		Weapon = Candidate;
		Character = Player;
		StartLocation = Player->GetActorLocation();
		ReferenceRelative = Candidate->GetRootComponent()->GetRelativeTransform();
		TrackingAt = Now;
		bTracking = true;
	}
	ACharacter* Player = Character.Get();
	AActor* Actor = Weapon.Get();
	if (!Player || !Actor || !Actor->GetRootComponent() || !Player->GetMesh())
	{
		Finish(TEXT("Tracked character or GLB actor disappeared"));
		return;
	}
	MotionTime += DeltaSeconds;
	const double Elapsed = MotionTime;
	USceneComponent* Root = Actor->GetRootComponent();
	USkeletalMeshComponent* Mesh = Player->GetMesh();
	const bool bAttached = Root->GetAttachParent() == Mesh
		&& Root->GetAttachSocketName() == HandSocket && Mesh->DoesSocketExist(HandSocket);
	bAttachmentAlwaysValid &= bAttached;
	// PostActorTick runs after actor/component tick groups and skeletal pose updates.
	// Compare world geometry with the initial root-relative offset composed with the current hand pose.
	const FTransform Expected = ReferenceRelative * Mesh->GetSocketTransform(HandSocket, RTS_World);
	const FTransform Actual = Root->GetComponentTransform();
	MaxPositionErrorCm = FMath::Max(MaxPositionErrorCm,
		FVector::Distance(Expected.GetLocation(), Actual.GetLocation()));
	MaxRotationErrorDeg = FMath::Max(MaxRotationErrorDeg,
		FMath::RadiansToDegrees(Expected.GetRotation().AngularDistance(Actual.GetRotation())));
	MaxScaleError = FMath::Max(MaxScaleError,
		(Expected.GetScale3D() - Actual.GetScale3D()).GetAbsMax());
	TInlineComponentArray<USceneComponent*> SceneComponents(Actor);
	for (const USceneComponent* Component : SceneComponents)
	{
		if (Component->ComponentHasTag(TEXT("ForgeGripAnchor")))
		{
			bGripAnchorTracking = true;
			MaxGripDistanceCm = FMath::Max(MaxGripDistanceCm, FVector::Dist(Component->GetComponentLocation(), Mesh->GetSocketLocation(HandSocket)));
		}
	}
	++Samples;
	const FVector Location = Player->GetActorLocation();
	MaxDisplacementCm = FMath::Max(MaxDisplacementCm, FVector::Dist2D(Location, StartLocation));
	MaxRiseCm = FMath::Max(MaxRiseCm, Location.Z - StartLocation.Z);
	bObservedFalling |= Player->GetCharacterMovement()->IsFalling();
	// Inputs added here are consumed by character movement on the following frame.
	if (Elapsed >= 1.0 && Elapsed <= 3.0) Player->AddMovementInput(Player->GetActorForwardVector(), 1.0f);
	if (!bJumpRequested && Elapsed >= 1.8)
	{
		Player->Jump();
		bJumpRequested = true;
		JumpAt = Elapsed;
	}
	else if (bJumpRequested && Elapsed - JumpAt >= 0.2) Player->StopJumping();
	static const double ScreenshotTimes[] = {0.8, 2.2, 3.8};
	static const TCHAR* ScreenshotNames[] = {TEXT("idle.png"), TEXT("jump.png"), TEXT("moved.png")};
	// Only one request can be pending in the engine. Never overwrite a pending capture.
	if (NextScreenshot < 3 && Elapsed >= ScreenshotTimes[NextScreenshot] && !FScreenshotRequest::IsScreenshotRequested())
	{
		const FString Filename = FPaths::Combine(OutputDirectory, ScreenshotNames[NextScreenshot]);
		IFileManager::Get().Delete(*Filename); // Exclude stale captures from a previous run with this ID.
		FScreenshotRequest::RequestScreenshot(Filename, false, false);
		RequestedScreenshots.Add(Filename);
		++NextScreenshot;
	}
	if (Elapsed >= 5.5) Finish(FString());
}

void UGlbValidationSubsystem::Finish(const FString& Failure)
{
	if (bFinished) return;
	bFinished = true;
	if (Character.IsValid()) Character->StopJumping();
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("run"), RunId);
	Report->SetStringField(TEXT("failure"), Failure);
	Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Report->SetStringField(TEXT("command_line"), FCommandLine::Get());
	Report->SetStringField(TEXT("world"), GetWorld() ? GetWorld()->GetPathName() : TEXT(""));
	Report->SetNumberField(TEXT("elapsed_wall_seconds"), FPlatformTime::Seconds() - StartedAt);
	Report->SetNumberField(TEXT("tracking_wall_seconds"), bTracking ? FPlatformTime::Seconds() - TrackingAt : 0.0);
	Report->SetNumberField(TEXT("initial_glb_actor_count"), LastActorCount);
	Report->SetNumberField(TEXT("samples"), Samples);
	Report->SetObjectField(TEXT("start_character_location_cm"), VectorJson(StartLocation));
	Report->SetObjectField(TEXT("end_character_location_cm"), VectorJson(Character.IsValid() ? Character->GetActorLocation() : StartLocation));
	Report->SetStringField(TEXT("initial_root_relative_transform"), ReferenceRelative.ToString());
	Report->SetBoolField(TEXT("attachment_valid_every_sample"), bTracking && bAttachmentAlwaysValid);
	Report->SetNumberField(TEXT("max_hand_position_error_cm"), MaxPositionErrorCm);
	Report->SetNumberField(TEXT("max_hand_rotation_error_degrees"), MaxRotationErrorDeg);
	Report->SetNumberField(TEXT("max_hand_scale_error"), MaxScaleError);
	Report->SetNumberField(TEXT("position_tolerance_cm"), 1.0);
	Report->SetNumberField(TEXT("rotation_tolerance_degrees"), 0.5);
	Report->SetNumberField(TEXT("scale_tolerance"), 0.01);
	Report->SetNumberField(TEXT("max_horizontal_displacement_cm"), MaxDisplacementCm);
	Report->SetBoolField(TEXT("jump_requested"), bJumpRequested);
	Report->SetBoolField(TEXT("observed_is_falling"), bObservedFalling);
	Report->SetNumberField(TEXT("max_character_rise_cm"), MaxRiseCm);
	TArray<TSharedPtr<FJsonValue>> Meshes;
	int32 MeshCount = 0;
	int32 MeshAssetCount = 0;
	int32 MaterialCount = 0;
	if (Weapon.IsValid())
	{
		Report->SetStringField(TEXT("weapon_actor"), Weapon->GetPathName());
		USceneComponent* Root = Weapon->GetRootComponent();
		Report->SetStringField(TEXT("attach_parent"), Root && Root->GetAttachParent() ? Root->GetAttachParent()->GetPathName() : TEXT(""));
		Report->SetStringField(TEXT("attach_socket"), Root ? Root->GetAttachSocketName().ToString() : TEXT(""));
		TInlineComponentArray<UMeshComponent*> Components;
		Weapon->GetComponents(Components);
		for (UMeshComponent* Component : Components)
		{
			++MeshCount;
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("component"), Component->GetPathName());
			Item->SetStringField(TEXT("class"), Component->GetClass()->GetName());
			Item->SetBoolField(TEXT("visible"), Component->IsVisible());
			UObject* MeshAsset = nullptr;
			if (const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Component))
			{
				MeshAsset = Static->GetStaticMesh();
			}
			else if (const USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Component))
			{
				MeshAsset = Skeletal->GetSkeletalMeshAsset();
			}
			if (MeshAsset) ++MeshAssetCount;
			Item->SetStringField(TEXT("mesh_asset"), MeshAsset ? MeshAsset->GetPathName() : TEXT("NULL"));
			TArray<TSharedPtr<FJsonValue>> Materials;
			for (int32 Index = 0; Index < Component->GetNumMaterials(); ++Index)
			{
				UMaterialInterface* Material = Component->GetMaterial(Index);
				Materials.Add(MakeShared<FJsonValueString>(Material ? Material->GetPathName() : TEXT("NULL")));
				if (Material) ++MaterialCount;
			}
			Item->SetArrayField(TEXT("materials"), Materials);
			Meshes.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	Report->SetNumberField(TEXT("mesh_component_count"), MeshCount);
	Report->SetNumberField(TEXT("non_null_mesh_asset_count"), MeshAssetCount);
	Report->SetNumberField(TEXT("non_null_material_count"), MaterialCount);
	Report->SetArrayField(TEXT("meshes"), Meshes);
	TArray<TSharedPtr<FJsonValue>> Screenshots;
	bool bScreenshotsExist = RequestedScreenshots.Num() == 3;
	for (const FString& Filename : RequestedScreenshots)
	{
		auto Item = MakeShared<FJsonObject>();
		const int64 Bytes = IFileManager::Get().FileSize(*Filename);
		Item->SetStringField(TEXT("file"), Filename);
		Item->SetNumberField(TEXT("bytes"), static_cast<double>(Bytes));
		Item->SetBoolField(TEXT("exists"), Bytes > 0);
		bScreenshotsExist &= Bytes > 0;
		Screenshots.Add(MakeShared<FJsonValueObject>(Item));
	}
	Report->SetArrayField(TEXT("screenshots"), Screenshots);
	Report->SetBoolField(TEXT("uses_grip_anchor"), bGripAnchorTracking);
	Report->SetNumberField(TEXT("max_grip_distance_cm"), MaxGripDistanceCm);
	Report->SetNumberField(TEXT("grip_distance_limit_cm"), 20);
	const bool bPoseValid = bGripAnchorTracking ? MaxGripDistanceCm <= 20
		: MaxPositionErrorCm <= 1.0 && MaxRotationErrorDeg <= 0.5 && MaxScaleError <= 0.01;
	const bool bPassed = Failure.IsEmpty() && bTracking && Samples > 1 && bAttachmentAlwaysValid
		&& bPoseValid
		&& MaxDisplacementCm > 10.0 && bJumpRequested && bObservedFalling && MaxRiseCm > 10.0
		&& MeshAssetCount > 0 && MaterialCount > 0 && bScreenshotsExist;
	Report->SetBoolField(TEXT("passed"), bPassed);
	Report->SetStringField(TEXT("measurement_note"), TEXT("Game-time motion; sampled OnWorldPostActorTick. Grip-marker equipment is checked against hand distance and attachment; legacy fixed-relative errors are informational for it. Inspect PNGs for visual alignment."));
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	const bool bSerialized = FJsonSerializer::Serialize(Report, Writer);
	const FString ReportPath = FPaths::Combine(OutputDirectory, TEXT("report.json"));
	const bool bSaved = bSerialized && FFileHelper::SaveStringToFile(Json, *ReportPath);
	UE_LOG(LogGLBValidation, Display, TEXT("GLB self-test passed=%s report_saved=%s attachment=%s displacement_cm=%.3f rise_cm=%.3f max_error_cm=%.6f report=%s failure=%s"),
		bPassed ? TEXT("true") : TEXT("false"), bSaved ? TEXT("true") : TEXT("false"),
		bAttachmentAlwaysValid && bTracking ? TEXT("true") : TEXT("false"),
		MaxDisplacementCm, MaxRiseCm, MaxPositionErrorCm, *ReportPath, *Failure);
	FPlatformMisc::RequestExit(false);
}
