#pragma once

#include "Blueprint/UserWidget.h"
#include "TrioCombat.h"
#include "TrioCombatHUD.generated.h"

class UCanvasPanel;
class UCanvasPanelSlot;
class UTextBlock;

UCLASS()
class UTrioCombatHUD : public UUserWidget
{
    GENERATED_BODY()
public:
    void Update(const UTrioCombatWorld& Combat, APlayerController& Player, float Seconds);
    void AddDamage(FVector Location, const FTrioHitResult& Result);

protected:
    virtual void NativeOnInitialized() override;
    virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
        FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const override;

private:
    struct FFloat
    {
        UTextBlock* Text = nullptr;
        UCanvasPanelSlot* Slot = nullptr;
        FVector Location;
        double Age = 0;
        double Offset = 0;
    };
    struct FOverhead
    {
        FVector2D Screen = FVector2D::ZeroVector;
        float Shield = 0;
        float Health = 0;
        float Scale = 1;
        float Opacity = 0;
        bool bActive = false;
    };
    void UpdateOverhead(const UTrioCombatWorld& Combat, APlayerController& Player, bool bScoped);

    int32 SweepShots = INDEX_NONE;
    FOverhead Overhead[3];
    bool bScopeLayout = false;
    TArray<FFloat> Numbers;
    UPROPERTY() TObjectPtr<UCanvasPanel> Root;
    UPROPERTY() TObjectPtr<UTextBlock> Charge;
    UPROPERTY() TObjectPtr<UTextBlock> BotStatus;
    UPROPERTY() TObjectPtr<UTextBlock> SweepCounter;
};
