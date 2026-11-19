#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CityMapSubsystem.generated.h"

class APlayerController;
class UInputComponent;
class SCityMapWidget;

/** Local presentation only: no world edits, network requests or input-mode changes. */
UCLASS()
class TRY_GLB_PUT_INTO_API UCityMapSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual void Deinitialize() override;
    bool IsExpanded() const { return bExpanded && MapWidget.IsValid(); }
private:
    void ToggleMap();
    bool IsCity() const;
    bool bLoadAttempted = false;
    bool bExpanded = false;
    TWeakObjectPtr<APlayerController> Controller;
    UPROPERTY(Transient) TObjectPtr<UInputComponent> MapInput;
    TSharedPtr<SCityMapWidget> MapWidget;
};
