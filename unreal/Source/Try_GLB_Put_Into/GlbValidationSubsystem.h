#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GlbValidationSubsystem.generated.h"

class ACharacter;
class AActor;

/** Opt-in packaged-game acceptance probe. Never loads or attaches a GLB. */
UCLASS()
class TRY_GLB_PUT_INTO_API UGlbValidationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

private:
	void AfterActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void Finish(const FString& Failure);
	bool bEnabled = false;
	bool bFinished = false;
	bool bTracking = false;
	bool bJumpRequested = false;
	bool bObservedFalling = false;
	bool bAttachmentAlwaysValid = true;
	bool bGripAnchorTracking = false;
	double MaxGripDistanceCm = 0;
	bool bOutputDirectoryReady = false;
	double StartedAt = 0;
	double TrackingAt = 0;
	double MotionTime = 0;
	double JumpAt = 0;
	double MaxPositionErrorCm = 0;
	double MaxRotationErrorDeg = 0;
	double MaxScaleError = 0;
	double MaxRiseCm = 0;
	double MaxDisplacementCm = 0;
	int32 Samples = 0;
	int32 LastActorCount = 0;
	int32 NextScreenshot = 0;
	FVector StartLocation = FVector::ZeroVector;
	FTransform ReferenceRelative = FTransform::Identity;
	TWeakObjectPtr<AActor> Weapon;
	TWeakObjectPtr<ACharacter> Character;
	FString OutputDirectory;
	FString RunId;
	TArray<FString> RequestedScreenshots;
	FDelegateHandle PostActorTickHandle;
};
