#pragma once

#include "Blueprint/UserWidget.h"
#include "Styling/SlateBrush.h"
#include "TrioCityHUD.generated.h"

class FTrioCityInput;
class UTexture2D;

UCLASS()
class UTrioCityHUD : public UUserWidget
{
    GENERATED_BODY()

public:
    UTrioCityHUD(const FObjectInitializer& Initializer);
    static bool Supports(const UWorld& World);
    void Update(const APlayerController& Player);

protected:
    virtual void NativeOnInitialized() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
        FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const override;

private:
    friend class FTrioCityInput;

    enum class EPanel : uint8 { None, Atlas };

    bool HasGameFocus() const;
    bool HandleKey(const FKeyEvent& Event);
    void SetPanel(EPanel Next);

    // Full map navigation (UV space of the atlas texture).
    FVector2f ToLocal(const FPointerEvent& Event) const;
    void ResetAtlasView();
    void ClampAtlasView();
    void ZoomAtlas(float Steps, FVector2f Pivot);
    void PanAtlas(FVector2f DeltaUV);
    void PointerDown(const FPointerEvent& Event);
    bool PointerMove(const FPointerEvent& Event);

    UPROPERTY() TObjectPtr<UTexture2D> AtlasTexture;
    UPROPERTY() TObjectPtr<UTexture2D> ForgeTexture;
    FSlateBrush AtlasBrush;
    FSlateBrush ForgeBrush;
    TSharedPtr<FTrioCityInput> Input;
    FVector2f PlayerUV = FVector2f(0.5f, 0.5f);
    FVector2f BeaconUV = FVector2f(0.5f, 0.5f);
    float PlayerHeading = 0.f;
    float CameraHeading = 0.f;
    EPanel Panel = EPanel::None;
    bool bHasPawn = false;
    bool bHasBeacon = false;
    bool bOwnsPause = false;
    bool bRestoreDebugDisplay = false;
    bool bRestoreCursor = false;

    float AtlasZoom = 1.f;
    FVector2f AtlasCenter = FVector2f(0.5f, 0.5f);
    double PanelOpened = 0.0;
    bool bDragging = false;
    FVector2f DragLast = FVector2f::ZeroVector;
    // Where the full map was last painted, in widget-local units (input hit testing).
    mutable FVector2f AtlasRectOrigin = FVector2f::ZeroVector;
    mutable float AtlasRectSize = 0.f;
};
