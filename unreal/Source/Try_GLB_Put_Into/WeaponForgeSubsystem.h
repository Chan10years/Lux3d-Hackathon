#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "WeaponGripState.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeaponForgeSubsystem.generated.h"

class ACharacter;
class UWeaponSyncComponent;
class APlayerController;
class FJsonObject;
class SForgeCanvas;
class SWidget;
class UInputComponent;
class IInputProcessor;
struct FKeyEvent;

/** One forge session per game world. Leaving the world discards all callbacks. */
UCLASS()
class TRY_GLB_PUT_INTO_API UWeaponForgeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual void Deinitialize() override;

	/** Visual equipment only. A multiplayer host should validate the descriptor separately. */
	UFUNCTION(BlueprintCallable, Category="Weapon Forge")
	bool EquipGlb(ACharacter* Character, const TArray<uint8>& Bytes, float LengthCm, bool bReverseGrip, int32 Preset = -1, bool bSample = false);
	bool EquipInitialGun();
	bool IsPanelOpen() const { return Panel.IsValid(); }

private:
	enum class EStep : uint8 { Idle, Submit, Poll, Resume, Descriptor, Download, Equip };
	void TogglePanel();
	void ClosePanel();
	void Submit();
	void SubmitPreview();
	void SubmitSketch(bool bPreview);
	void Retry();
	void ContinueStep(EStep Step);
	bool RetryConnection(EStep Step);
	void ImportSample();
	void FlipGrip();
	void Request(EStep Step, const FString& Path, const TCHAR* Verb = TEXT("GET"));
	void Receive(EStep Step, const FHttpResponsePtr& Response, bool bConnected);
	void RecoverSubmission();
	void TryEquip();
	void Fail(EStep RetryAt, const FText& Message);
	void CreateForgeEntry();
	bool RouteForgeKey(const FKeyEvent& Event);
	void PoseWeapon(ACharacter* Character, AActor* Visual, float Pitch = 0, float Yaw = 0, float Extension = 0);
	void RotateGrip();
	void AdjustGripForward();
	void AdjustGripBack();
	bool PrepareGrip(AActor* Visual, const TArray<uint8>& Bytes, float LengthCm, bool bReverse, int32 Preset);
	bool TraceWeaponBarrier(const ACharacter* Holder, const FVector& Start, const FVector& End, float Radius, FHitResult& Hit) const;
	bool WeaponPoseClear(const ACharacter* Holder, const FTransform& Pose, const FVector& Grip) const;
	void SelectTrialSword();
	void SelectTrialSpear();
	void SelectTrialLongsword();
	void CycleTrial();
	void SelectTrial(int32 Preset, bool bShortSample = false);
	void ToggleTrialShield();
	void ActivateTrial(ACharacter* Character, int32 Preset, float Reach, const FString& Trait);
	void StartTrialAttack();
	void ResetTrialTargets();
	void UpdateTrial(float DeltaTime);
	void ClearTrial();
	void ResolveTrialHit();
	void ApplyTrialDamage(int32 TargetIndex, int32 Damage, const FVector& Direction, bool bPrimary);
	void UpdateTrialProjectiles(float DeltaTime);
	int32 FindTrialPreset(const FString& Id, const FString& Trait, double LengthM, double ReachM) const;

	struct FTrialTarget
	{
		FVector Position;
		int32 Health = 100;
		float Flash = 0;
		float BurnTime = 0;
		int32 BurnTicks = 0;
		float ChillTime = 0;
		float PatrolOffset = 0;
		float PatrolDirection = 1;
	};
	TArray<FTrialTarget> TrialTargets;
	TWeakObjectPtr<AActor> TrialShield;
	struct FTrialProjectile
	{
		FVector Position;
		FVector Direction;
		float Distance;
		float Speed;
		int32 Damage;
	};
	TArray<FTrialProjectile> TrialProjectiles;
	float TrialTestDelay = -1;
	float ForgePanelTestTime = -1;
	float TrialStrikeTime = 0;
	float TrialFinishTime = 0;
	float TrialInterval = 0;
	int32 TrialDamage = 0;
	bool bTrialHitResolved = false;
	TWeakObjectPtr<ACharacter> TrialCharacter;
	TWeakObjectPtr<AActor> TrialVisual;
	FVector TrialForward = FVector::ForwardVector;
	FVector TrialPatrolAxis = FVector::RightVector;
	float TrialOriginalSpeed = 0;
	float TrialAttackTime = -1;
	int32 TrialPreset = INDEX_NONE;
	int32 TrialTestPhase = 0;
	bool bTrialSwordPassed = false;
	bool bTrialRestoreOrientation = false;
	int32 CatalogTestIndex = INDEX_NONE;
	float TrialReachCm = 0;
	float TrialTraitCooldown = 0;
	FString TrialTrait;

	TWeakObjectPtr<APlayerController> Controller;
	UPROPERTY(Transient)
	TObjectPtr<UInputComponent> ForgeInput;
	TSharedPtr<SWidget> Panel;
	TSharedPtr<SWidget> ForgeEntry;
	TSharedPtr<IInputProcessor> ForgeKeys;
	TSharedPtr<SForgeCanvas> Canvas;
	FHttpRequestPtr Pending;
	EStep Stage = EStep::Idle;
	EStep RetryStage = EStep::Idle;
	FText Status;
	FString ServiceUrl = TEXT("http://127.0.0.1:47833");
	FString ActiveServiceUrl;
	FString Prompt = TEXT("A stylized forged weapon matching the drawing");
	FString TaskId;
	FString ModelPath;
	FString EquippedModelUrl;
	TArray<uint8> SubmitBody;
	TArray<uint8> ReadyBytes;
	TArray<uint8> EquippedBytes;
	FString ContentType;
	double NextPoll = 0;
	double Deadline = 0;
	double CooldownUntil = 0;
	double NextSubmitReplay = 0;
	int32 SubmitReplays = 0;
	int32 ConsecutiveConnectionFailures = 0;
	bool bDeferredRetry = false;
	bool bLocatingSubmission = false;
	float LengthCm = 110;
	float ReadyLengthCm = 110;
	float EquippedLengthCm = 110;
	int32 EquippedPreset = INDEX_NONE;
	FWeaponGripState GripState;
	bool bRestoredMoveInput = false;
	bool bRestoredLookInput = false;
	bool bOriginalCursor = false;
	bool bReverseGrip = false;
	bool bEquippedReverseGrip = false;
	bool bReadyIsSample = false;
	bool bEquippedIsSample = false;
	TWeakObjectPtr<UWeaponSyncComponent> LastPublishedComponent;
	bool bPendingPreview = false;
	int32 ReadyCombatPreset = INDEX_NONE;
	float ReadyReachCm = 0;
	FString ReadyTrait;
	FText ReadySummary;
	TWeakObjectPtr<ACharacter> EquippedCharacter;
};
