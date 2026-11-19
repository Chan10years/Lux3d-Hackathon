#include "TrioCityHUD.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CanvasPanel.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Input/Events.h"
#include "Kismet/GameplayStatics.h"
#include "Layout/Clipping.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/SViewport.h"

#define LOCTEXT_NAMESPACE "TrioCityHUD"

namespace
{
    // World square covered by T_CityAtlas (UE cm, X north, Y east): the city plus the construction site to the south.
    // Baked by TrioAtlasBake (TrioCombatEditor); keep the atlas and HUD projection settings in sync.
    constexpr float MapCenterX = -16100.f;
    constexpr float MapCenterY = 0.f;
    constexpr float MapSpan = 122880.f;
    // World distance from the minimap centre to its rim.
    constexpr float LocalRange = 6200.f;
    constexpr float MaxAtlasZoom = 6.f;

    const FLinearColor Lime = FLinearColor::FromSRGBColor(FColor(214, 250, 106));
    const FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(13, 17, 16));
    const FLinearColor Plate = FLinearColor::FromSRGBColor(FColor(23, 28, 27));
    const FLinearColor Border = FLinearColor::FromSRGBColor(FColor(76, 86, 81));
    const FLinearColor Paper = FLinearColor::FromSRGBColor(FColor(241, 245, 237));
    const FLinearColor Muted = FLinearColor::FromSRGBColor(FColor(161, 174, 166));
    const FLinearColor Shadow(0.f, 0.f, 0.f, .36f);

    FVector2f Polar(float Angle, float Radius)
    {
        return FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius;
    }

    // Screen direction of a heading (radians, 0 = up, clockwise positive).
    FVector2f Facing(float Heading)
    {
        return FVector2f(FMath::Sin(Heading), -FMath::Cos(Heading));
    }

    struct FDrawHUD
    {
        const FGeometry& Geometry;
        FSlateWindowElementList& Elements;
        int32 Layer;
        FVector2f Origin = FVector2f::ZeroVector;
        float Scale = 1.f;

        FPaintGeometry At(FVector2f Position, FVector2f Size) const
        {
            return Geometry.ToPaintGeometry(Size * Scale, FSlateLayoutTransform(Origin + Position * Scale));
        }

        void Box(FVector2f Position, FVector2f Size, FLinearColor Color, float Radius = 0.f)
        {
            if (Radius > 0.f)
            {
                const FSlateRoundedBoxBrush Brush(FLinearColor::White, Radius * Scale);
                FSlateDrawElement::MakeBox(Elements, ++Layer, At(Position, Size), &Brush, ESlateDrawEffect::None, Color);
                return;
            }
            FSlateDrawElement::MakeBox(Elements, ++Layer, At(Position, Size), FCoreStyle::Get().GetBrush("WhiteBrush"),
                ESlateDrawEffect::None, Color);
        }

        void Disc(FVector2f Center, float Radius, FLinearColor Color)
        {
            Box(Center - FVector2f(Radius, Radius), {Radius * 2, Radius * 2}, Color, Radius);
        }

        void Image(FVector2f Position, FVector2f Size, const FSlateBrush& Brush)
        {
            if (Brush.GetResourceObject())
                FSlateDrawElement::MakeBox(Elements, ++Layer, At(Position, Size), &Brush);
        }

        FSlateFontInfo Font(int32 Size, FName Face) const
        {
            return FCoreStyle::GetDefaultFontStyle(Face, FMath::Max(1, FMath::RoundToInt(Size * Scale)));
        }

        void Text(FVector2f Position, const FText& Label, int32 Size, FLinearColor Color = Paper, FName Face = "Regular")
        {
            FSlateDrawElement::MakeText(Elements, ++Layer, At(Position, {1, 1}), Label, Font(Size, Face), ESlateDrawEffect::None, Color);
        }

        void CenteredText(FVector2f Center, const FText& Label, int32 Size, FLinearColor Color, FName Face)
        {
            const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
            const FVector2f Extent = FVector2f(Measure->Measure(Label, Font(Size, Face))) / Scale;
            Text(Center - Extent * .5f, Label, Size, Color, Face);
        }

        void Line(TArray<FVector2f> Points, FLinearColor Color, float Thickness = 1.f)
        {
            for (FVector2f& Point : Points)
                Point *= Scale;
            FSlateDrawElement::MakeLines(Elements, ++Layer, At({0, 0}, {1, 1}), Points, ESlateDrawEffect::None,
                Color, true, Thickness * Scale);
        }

        void Arc(FVector2f Center, float Radius, float Start, float End, FLinearColor Color, float Thickness)
        {
            const int32 Steps = FMath::Max(2, FMath::CeilToInt((End - Start) / 4.f));
            TArray<FVector2f> Points;
            Points.Reserve(Steps + 1);
            for (int32 Step = 0; Step <= Steps; ++Step)
                Points.Emplace(Center + Polar(FMath::DegreesToRadians(FMath::Lerp(Start, End, float(Step) / Steps)), Radius));
            Line(MoveTemp(Points), Color, Thickness);
        }

        // Convex polygon, fan-triangulated from its first point, with per-vertex colours and optional texture.
        void Mesh(const TArray<FVector2f>& Points, TFunctionRef<FLinearColor(int32)> ColorAt, const FSlateBrush* Brush = nullptr,
            TFunctionRef<FVector2f(int32)> UVAt = [](int32) { return FVector2f::ZeroVector; })
        {
            const FSlateResourceHandle Handle = Brush ? FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush)
                : FSlateResourceHandle();
            if (Brush && !Handle.IsValid())
                return;
            TArray<FSlateVertex> Vertices;
            TArray<SlateIndex> Indices;
            Vertices.Reserve(Points.Num());
            Indices.Reserve((Points.Num() - 2) * 3);
            for (int32 Index = 0; Index < Points.Num(); ++Index)
            {
                Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Geometry.GetAccumulatedRenderTransform(),
                    Origin + Points[Index] * Scale, UVAt(Index), ColorAt(Index).ToFColorSRGB()));
            }
            for (int32 Index = 2; Index < Points.Num(); ++Index)
            {
                Indices.Add(0);
                Indices.Add(Index - 1);
                Indices.Add(Index);
            }
            FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, Handle, Vertices, Indices, nullptr, 0, 0);
        }

        void Shape(const TArray<FVector2f>& Points, FLinearColor Color)
        {
            Mesh(Points, [&](int32) { return Color; });
        }

        // Quad from edge A0-A1 (colour A) to edge B1-B0 (colour B): soft gradients.
        void Gradient(FVector2f A0, FVector2f A1, FVector2f B1, FVector2f B0, FLinearColor A, FLinearColor B)
        {
            Mesh({A0, A1, B1, B0}, [&](int32 Index) { return Index < 2 ? A : B; });
        }

        // Plate with two cut corners (top-left, bottom-right): the HUD's hint shape.
        void Chamfer(FVector2f Min, FVector2f Max, float Cut, FLinearColor Color)
        {
            Shape({{Min.X + Cut, Min.Y}, {Max.X, Min.Y}, {Max.X, Max.Y - Cut}, {Max.X - Cut, Max.Y}, {Min.X, Max.Y}, {Min.X, Min.Y + Cut}}, Color);
        }

        // Lime key hint shared by the minimap and the forge entry.
        void KeyBadge(FVector2f Center, const FText& Name, float Width)
        {
            const FVector2f Half(Width * .5f, 10.f);
            Chamfer(Center - Half + FVector2f(0, 3), Center + Half + FVector2f(0, 3), 5.f, Shadow);
            Chamfer(Center - Half - FVector2f(2, 2), Center + Half + FVector2f(2, 2), 6.f, Ink);
            Chamfer(Center - Half, Center + Half, 5.f, Lime);
            CenteredText(Center + FVector2f(0, .5f), Name, 12, Ink, "BoldCondensed");
        }
    };

    void PlayerArrow(FDrawHUD& Draw, FVector2f Center, float Heading, float Size)
    {
        const float Sine = FMath::Sin(Heading);
        const float Cosine = FMath::Cos(Heading);
        TArray<FVector2f> Points;
        for (const FVector2f Point : {FVector2f(0, .30f), FVector2f(-.72f, .66f), FVector2f(0, -1), FVector2f(.72f, .66f)})
            Points.Add(Center + FVector2f(Point.X * Cosine - Point.Y * Sine, Point.X * Sine + Point.Y * Cosine) * Size);
        // Non-convex chevron: draw it as two convex halves.
        Draw.Shape({Points[0], Points[1], Points[2]}, Lime);
        Draw.Shape({Points[0], Points[2], Points[3]}, Lime);
        const FVector2f Start = Points[0];
        Points.Add(Start);
        Draw.Line(MoveTemp(Points), Ink, 1.6f);
    }

    // Soft wedge showing where the camera looks.
    void ViewCone(FDrawHUD& Draw, FVector2f Center, float Heading, float Radius, float Alpha)
    {
        constexpr int32 Steps = 14;
        constexpr float Spread = .62f;
        TArray<FVector2f> Points;
        Points.Add(Center);
        for (int32 Step = 0; Step <= Steps; ++Step)
            Points.Add(Center + Facing(Heading + FMath::Lerp(-Spread, Spread, float(Step) / Steps)) * Radius);
        Draw.Mesh(Points, [&](int32 Index) { return Lime.CopyWithNewOpacity(Index == 0 ? Alpha : 0.f); });
    }

    void Marker(FDrawHUD& Draw, FVector2f Center, float Heading, float ViewHeading, float Size, float ConeRadius)
    {
        ViewCone(Draw, Center, ViewHeading, ConeRadius, .34f);
        Draw.Disc(Center, Size * 1.45f, Ink.CopyWithNewOpacity(.55f));
        PlayerArrow(Draw, Center, Heading, Size);
    }

    void BeaconMarker(FDrawHUD& Draw, FVector2f Center, float Size)
    {
        const FLinearColor BeaconCyan(0.18f, 0.92f, 1.0f, 1.0f);
        const FLinearColor BeaconGold(1.0f, 0.86f, 0.25f, 1.0f);
        const float Pulse = FMath::Fmod(float(FPlatformTime::Seconds()), 1.4f) / 1.4f;
        Draw.Arc(Center, Size * (1.1f + Pulse * 1.1f), 0, 360, BeaconCyan.CopyWithNewOpacity(0.85f * (1.f - Pulse)), 1.8f);
        const TArray<FVector2f> Outer = {
            Center + FVector2f(0, -Size),
            Center + FVector2f(Size * 0.82f, 0),
            Center + FVector2f(0, Size),
            Center + FVector2f(-Size * 0.82f, 0)
        };
        Draw.Shape(Outer, Ink);
        const float InnerS = Size * 0.72f;
        const TArray<FVector2f> Inner = {
            Center + FVector2f(0, -InnerS),
            Center + FVector2f(InnerS * 0.82f, 0),
            Center + FVector2f(0, InnerS),
            Center + FVector2f(-InnerS * 0.82f, 0)
        };
        Draw.Shape(Inner, BeaconCyan);
        Draw.Disc(Center, Size * 0.26f, BeaconGold);
    }

    void LocalMap(FDrawHUD& Draw, const FSlateBrush& Brush, FVector2f UV, float Heading, float View, bool bHasPawn,
        FVector2f BeaconUV, bool bHasBeacon)
    {
        const FVector2f Center(120, 120);
        constexpr float Radius = 80.f;
        constexpr float Bezel = 9.f;

        // Soft drop shadow, bezel, map.
        for (int32 Ring = 0; Ring < 4; ++Ring)
            Draw.Disc(Center + FVector2f(0, 4), Radius + Bezel + 3 + Ring * 2.5f, FLinearColor(0, 0, 0, .085f));
        Draw.Disc(Center, Radius + Bezel, Ink.CopyWithNewOpacity(.94f));
        Draw.Disc(Center, Radius, Plate);
        if (Brush.GetResourceObject())
        {
            // Map turns with the camera: screen-up is the view direction.
            constexpr int32 Segments = 96;
            const float Cosine = FMath::Cos(View);
            const float Sine = FMath::Sin(View);
            const float UVPerPixel = LocalRange / (Radius * MapSpan);
            TArray<FVector2f> Points;
            Points.Reserve(Segments + 2);
            Points.Add(Center);
            for (int32 Step = 0; Step <= Segments; ++Step)
                Points.Add(Center + Polar(Step * (2.f * PI / Segments), Radius));
            Draw.Mesh(Points, [](int32) { return FLinearColor::White; }, &Brush, [&](int32 Index)
            {
                const FVector2f Offset = Points[Index] - Center;
                return UV + FVector2f(Cosine * Offset.X - Sine * Offset.Y, Sine * Offset.X + Cosine * Offset.Y) * UVPerPixel;
            });
        }
        // Inner shade so the map sits under the glass.
        for (int32 Step = 0; Step < 7; ++Step)
            Draw.Arc(Center, Radius - .5f - Step * 1.5f, 0, 360, Ink.CopyWithNewOpacity(.42f * FMath::Square(1.f - Step / 7.f)), 1.8f);
        Draw.Arc(Center, Radius + .6f, 0, 360, Border, 1.2f);
        Draw.Arc(Center, Radius + Bezel - .6f, 0, 360, Border.CopyWithNewOpacity(.55f), 1.f);

        // Compass ticks on the bezel turn with the map; the north mark is a lime pip.
        for (int32 Tick = 0; Tick < 24; ++Tick)
        {
            if (Tick == 0)
                continue;
            const FVector2f Direction = Facing(Tick * (PI / 12.f) - View);
            const bool bCardinal = Tick % 6 == 0;
            Draw.Line({Center + Direction * (Radius + 2.5f), Center + Direction * (Radius + (bCardinal ? 6.5f : 4.5f))},
                Muted.CopyWithNewOpacity(bCardinal ? .8f : .38f), bCardinal ? 1.6f : 1.1f);
        }
        {
            const FVector2f North = Facing(-View);
            const FVector2f Side(-North.Y, North.X);
            const FVector2f Tip = Center + North * (Radius + 1.5f);
            const FVector2f Base = Center + North * (Radius + Bezel + 2.5f);
            Draw.Shape({Tip, Base + Side * 5.2f, Base - Side * 5.2f}, Ink);
            Draw.Shape({Tip + North * 1.6f, Base + Side * 3.6f - North * .6f, Base - Side * 3.6f - North * .6f}, Lime);
        }
        // Fixed lime accent on the upper-left of the bezel.
        Draw.Arc(Center, Radius + Bezel + 4.5f, 198, 246, Lime, 2.4f);
        Draw.Disc(Center + Polar(FMath::DegreesToRadians(254.f), Radius + Bezel + 4.5f), 1.7f, Lime);

        if (bHasBeacon)
        {
            const float UVPerPixel = LocalRange / (Radius * MapSpan);
            const FVector2f DeltaUV = BeaconUV - UV;
            const float Cosine = FMath::Cos(View);
            const float Sine = FMath::Sin(View);
            // Inverse of screen->UV rotation: rotate DeltaUV by -View
            FVector2f OffsetPx = FVector2f(Cosine * DeltaUV.X + Sine * DeltaUV.Y, -Sine * DeltaUV.X + Cosine * DeltaUV.Y) / UVPerPixel;
            const float DistPx = OffsetPx.Size();
            if (DistPx > Radius - 9.f && DistPx > KINDA_SMALL_NUMBER)
            {
                OffsetPx = OffsetPx * ((Radius - 9.f) / DistPx);
            }
            BeaconMarker(Draw, Center + OffsetPx, 7.5f);
        }

        if (bHasPawn)
            Marker(Draw, Center, Heading - View, 0.f, 9.5f, Radius * .78f);

        Draw.KeyBadge(Center + Polar(FMath::DegreesToRadians(45.f), Radius + Bezel + 3.f), LOCTEXT("Tab", "TAB/M"), 52);
    }

    void FullMap(FDrawHUD& Draw, FVector2f Screen, const FSlateBrush& Brush, FVector2f ViewMin, float ViewSpan,
        FVector2f UV, float Heading, float View, bool bHasPawn, FVector2f BeaconUV, bool bHasBeacon, float Intro, FVector2f& OutOrigin, float& OutSize)
    {
        const float Full = FMath::Max(120.f, FMath::Min(Screen.X, Screen.Y) - 2.f * FMath::Clamp(Screen.Y * .045f, 24.f, 64.f));
        const float Size = FMath::RoundToFloat(Full * FMath::Lerp(.965f, 1.f, Intro));
        const FVector2f Min = ((Screen - FVector2f(Size, Size)) * .5f).RoundToVector();
        const FVector2f Max = Min + FVector2f(Size, Size);
        OutOrigin = Min;
        OutSize = Size;

        for (int32 Ring = 0; Ring < 4; ++Ring)
        {
            const float Grow = 4.f + Ring * 5.f;
            Draw.Box(Min - FVector2f(Grow, Grow - 8), FVector2f(Size, Size) + FVector2f(Grow, Grow) * 2.f, FLinearColor(0, 0, 0, .12f), Grow + 4);
        }
        Draw.Box(Min - FVector2f(1, 1), FVector2f(Size + 2, Size + 2), Border);
        Draw.Box(Min, FVector2f(Size, Size), Ink);
        // Everything on the map (image, shade, player marker and its view cone) is clipped to the frame.
        Draw.Elements.PushClip(FSlateClippingZone(Draw.At(Min, FVector2f(Size, Size))));
        if (Brush.GetResourceObject())
        {
            const TArray<FVector2f> Corners = {Min, {Max.X, Min.Y}, Max, {Min.X, Max.Y}};
            Draw.Mesh(Corners, [&](int32) { return FLinearColor(1.f, 1.f, 1.f, Intro); }, &Brush,
                [&](int32 Index) { return ViewMin + (Corners[Index] - Min) / Size * ViewSpan; });
        }

        // Edge shade.
        const float Fade = FMath::Min(42.f, Size * .06f);
        const FLinearColor Edge = Ink.CopyWithNewOpacity(.6f);
        const FLinearColor Clear = Ink.CopyWithNewOpacity(0.f);
        Draw.Gradient(Min, {Max.X, Min.Y}, {Max.X, Min.Y + Fade}, {Min.X, Min.Y + Fade}, Edge, Clear);
        Draw.Gradient({Min.X, Max.Y}, Max, {Max.X, Max.Y - Fade}, {Min.X, Max.Y - Fade}, Edge, Clear);
        Draw.Gradient(Min, {Min.X, Max.Y}, {Min.X + Fade, Max.Y}, {Min.X + Fade, Min.Y}, Edge, Clear);
        Draw.Gradient({Max.X, Min.Y}, Max, {Max.X - Fade, Max.Y}, {Max.X - Fade, Min.Y}, Edge, Clear);

        if (bHasBeacon)
        {
            const FVector2f BeaconAt = Min + (BeaconUV - ViewMin) / ViewSpan * Size;
            if (BeaconAt.X > Min.X - 40 && BeaconAt.X < Max.X + 40 && BeaconAt.Y > Min.Y - 40 && BeaconAt.Y < Max.Y + 40)
            {
                BeaconMarker(Draw, BeaconAt, 11.f);
            }
        }
        if (bHasPawn)
        {
            const FVector2f At = Min + (UV - ViewMin) / ViewSpan * Size;
            if (At.X > Min.X - 40 && At.X < Max.X + 40 && At.Y > Min.Y - 40 && At.Y < Max.Y + 40)
            {
                const float Pulse = FMath::Fmod(float(FPlatformTime::Seconds()), 1.8f) / 1.8f;
                Draw.Arc(At, 10.f + Pulse * 26.f, 0, 360, Lime.CopyWithNewOpacity(.75f * Intro * FMath::Square(1.f - Pulse)), 2.f);
                Marker(Draw, At, Heading, View, 11.f, 74.f);
            }
        }
        Draw.Elements.PopClip();

        // Corner brackets.
        constexpr float Arm = 26.f;
        const float Gap = 7.f + 18.f * (1.f - Intro);
        for (const FVector2f Sign : {FVector2f(-1, -1), FVector2f(1, -1), FVector2f(1, 1), FVector2f(-1, 1)})
        {
            const FVector2f Corner((Sign.X < 0 ? Min.X : Max.X) + Sign.X * Gap, (Sign.Y < 0 ? Min.Y : Max.Y) + Sign.Y * Gap);
            Draw.Line({Corner - FVector2f(Sign.X * Arm, 0), Corner, Corner - FVector2f(0, Sign.Y * Arm)}, Lime.CopyWithNewOpacity(Intro), 3.f);
        }

        // North pip, top-right inside the frame.
        const FVector2f Pip(Max.X - 30, Min.Y + 32);
        Draw.Disc(Pip, 15.f, Ink.CopyWithNewOpacity(.82f));
        Draw.Arc(Pip, 15.f, 0, 360, Border, 1.f);
        Draw.Shape({Pip + FVector2f(0, -10), Pip + FVector2f(6, 6), Pip + FVector2f(0, 2.5f)}, Lime);
        Draw.Shape({Pip + FVector2f(0, -10), Pip + FVector2f(0, 2.5f), Pip + FVector2f(-6, 6)}, Lime.CopyWithNewOpacity(.55f));

        // Zoom gauge: a slim lime fill along the right edge.
        const float Zoom = 1.f / ViewSpan;
        if (Zoom > 1.01f)
        {
            const float Track = Size * .28f;
            const FVector2f Base(Max.X + 16, Max.Y - Track);
            Draw.Box(Base, {3, Track}, Border.CopyWithNewOpacity(.7f), 1.5f);
            const float Fill = Track * (Zoom - 1.f) / (MaxAtlasZoom - 1.f);
            Draw.Box({Base.X, Max.Y - Fill}, {3, Fill}, Lime, 1.5f);
        }
    }
}

class FTrioCityInput final : public IInputProcessor
{
public:
    explicit FTrioCityInput(UTrioCityHUD& HUD) : Owner(&HUD) {}

    virtual void Tick(float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
    {
        UTrioCityHUD* HUD = Owner.Get();
        return HUD && HUD->HasGameFocus() && HUD->HandleKey(Event);
    }

    virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
    {
        if (!IsModal())
            return false;
        Owner->PointerDown(Event);
        return true;
    }

    virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
    {
        UTrioCityHUD* HUD = Owner.Get();
        const bool bWasDragging = HUD && HUD->bDragging;
        if (HUD)
            HUD->bDragging = false;
        return bWasDragging || IsModal();
    }

    virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
    {
        UTrioCityHUD* HUD = Owner.Get();
        return HUD && IsModal() && HUD->PointerMove(Event);
    }

    virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
    {
        return IsModal();
    }

    virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& Event,
        const FPointerEvent* GestureEvent) override
    {
        if (!IsModal())
            return false;
        if (Owner->Panel == UTrioCityHUD::EPanel::Atlas && Event.GetWheelDelta() != 0.f)
            Owner->ZoomAtlas(Event.GetWheelDelta(), Owner->ToLocal(Event));
        return true;
    }

private:
    bool IsModal() const
    {
        const UTrioCityHUD* HUD = Owner.Get();
        return HUD && HUD->Panel != UTrioCityHUD::EPanel::None && HUD->HasGameFocus();
    }

    TWeakObjectPtr<UTrioCityHUD> Owner;
};

UTrioCityHUD::UTrioCityHUD(const FObjectInitializer& Initializer) : Super(Initializer)
{
    static ConstructorHelpers::FObjectFinder<UTexture2D> Atlas(TEXT("/Game/CityZZZ/Tex/HUD/T_CityAtlas.T_CityAtlas"));
    static ConstructorHelpers::FObjectFinder<UTexture2D> Forge(TEXT("/Game/CityZZZ/Tex/HUD/T_ForgeSigil.T_ForgeSigil"));
    AtlasTexture = Atlas.Object;
    ForgeTexture = Forge.Object;
}

bool UTrioCityHUD::Supports(const UWorld& World)
{
    return World.IsGameWorld() && UGameplayStatics::GetCurrentLevelName(&World) == TEXT("Lvl_ZZZ");
}

void UTrioCityHUD::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    WidgetTree->RootWidget = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CityOverlay"));
    SetVisibility(ESlateVisibility::HitTestInvisible);
    ForceVolatile(true);
    AtlasBrush.DrawAs = ESlateBrushDrawType::Image;
    AtlasBrush.SetResourceObject(AtlasTexture);
    ForgeBrush.DrawAs = ESlateBrushDrawType::Image;
    ForgeBrush.SetResourceObject(ForgeTexture);
}

void UTrioCityHUD::NativeConstruct()
{
    Super::NativeConstruct();
    if (FSlateApplication::IsInitialized() && !Input.IsValid())
    {
        Input = MakeShared<FTrioCityInput>(*this);
        FSlateApplication::Get().RegisterInputPreProcessor(Input, EInputPreProcessorType::PreEditor);
        bRestoreDebugDisplay = GEngine && GEngine->bEnableOnScreenDebugMessagesDisplay;
        if (bRestoreDebugDisplay)
            GEngine->bEnableOnScreenDebugMessagesDisplay = false;
    }
}

void UTrioCityHUD::NativeDestruct()
{
    if (Input.IsValid() && FSlateApplication::IsInitialized())
        FSlateApplication::Get().UnregisterInputPreProcessor(Input);
    Input.Reset();
    if (GEngine && bRestoreDebugDisplay)
        GEngine->bEnableOnScreenDebugMessagesDisplay = true;
    bRestoreDebugDisplay = false;
    SetPanel(EPanel::None);
    Super::NativeDestruct();
}

void UTrioCityHUD::Update(const APlayerController& Player)
{
    bHasBeacon = false;
    if (UWorld* World = Player.GetWorld())
    {
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->ActorHasTag(TEXT("BeaconObjective")))
            {
                const FVector BLoc = It->GetActorLocation();
                BeaconUV = FVector2f((BLoc.Y - MapCenterY) / MapSpan + .5f, (MapCenterX - BLoc.X) / MapSpan + .5f);
                bHasBeacon = true;
                break;
            }
        }
    }
    const APawn* Pawn = Player.GetPawn();
    bHasPawn = Pawn != nullptr;
    if (!Pawn)
        return;
    const FVector Location = Pawn->GetActorLocation();
    PlayerUV = FVector2f((Location.Y - MapCenterY) / MapSpan + .5f, (MapCenterX - Location.X) / MapSpan + .5f);
    PlayerHeading = FMath::DegreesToRadians(float(Pawn->GetActorRotation().Yaw));
    const FRotator View = Player.PlayerCameraManager ? Player.PlayerCameraManager->GetCameraRotation() : Player.GetControlRotation();
    CameraHeading = FMath::DegreesToRadians(float(View.Yaw));
}

bool UTrioCityHUD::HasGameFocus() const
{
    const APlayerController* Player = GetOwningPlayer();
    const ULocalPlayer* LocalPlayer = Player ? Player->GetLocalPlayer() : nullptr;
    const UGameViewportClient* Viewport = LocalPlayer ? LocalPlayer->ViewportClient : nullptr;
    const TSharedPtr<SViewport> Widget = Viewport ? Viewport->GetGameViewportWidget() : nullptr;
    return Widget && (Widget->HasKeyboardFocus() || Widget->HasFocusedDescendants());
}

bool UTrioCityHUD::HandleKey(const FKeyEvent& Event)
{
    const FKey Key = Event.GetKey();
    const bool bModal = Panel != EPanel::None;
    if (Key == EKeys::Escape && bModal && !Event.IsRepeat())
    {
        SetPanel(EPanel::None);
        return true;
    }
    if (!Event.IsAltDown() && !Event.IsCommandDown() && !Event.IsShiftDown())
    {
        if ((Key == EKeys::Tab && !Event.IsControlDown()) || (Key == EKeys::M && !Event.IsControlDown()))
        {
            if (FSlateApplication::IsInitialized())
            {
                const auto Focus = FSlateApplication::Get().GetKeyboardFocusedWidget();
                if (Focus && Focus->GetTypeAsString().Contains(TEXT("EditableText"))) return false;
            }
            if (!Event.IsRepeat())
            {
                SetPanel(Panel == EPanel::Atlas ? EPanel::None : EPanel::Atlas);
            }
            return true;
        }
    }
    if (Panel == EPanel::Atlas)
    {
        const float Step = .09f / AtlasZoom;
        if (Key == EKeys::W || Key == EKeys::Up)
            PanAtlas({0, -Step});
        else if (Key == EKeys::S || Key == EKeys::Down)
            PanAtlas({0, Step});
        else if (Key == EKeys::A || Key == EKeys::Left)
            PanAtlas({-Step, 0});
        else if (Key == EKeys::D || Key == EKeys::Right)
            PanAtlas({Step, 0});
        else if (Key == EKeys::Equals || Key == EKeys::Add || Key == EKeys::E)
            ZoomAtlas(1.f, AtlasRectOrigin + FVector2f(AtlasRectSize, AtlasRectSize) * .5f);
        else if (Key == EKeys::Hyphen || Key == EKeys::Subtract || Key == EKeys::Q)
            ZoomAtlas(-1.f, AtlasRectOrigin + FVector2f(AtlasRectSize, AtlasRectSize) * .5f);
    }
    return bModal;
}

void UTrioCityHUD::SetPanel(EPanel Next)
{
    APlayerController* Player = GetOwningPlayer();
    if (Player && Next != Panel && (Next == EPanel::None || Panel == EPanel::None))
    {
        if (Next == EPanel::None)
        {
            if (const UWorld* World = Player->GetWorld(); bOwnsPause && World && !World->bIsTearingDown)
                Player->SetPause(false);
            Player->SetIgnoreMoveInput(false);
            Player->SetIgnoreLookInput(false);
            bOwnsPause = false;
        }
        else
        {
            Player->FlushPressedKeys();
            Player->SetIgnoreMoveInput(true);
            Player->SetIgnoreLookInput(true);
            bOwnsPause = Player->GetNetMode() == NM_Standalone && !UGameplayStatics::IsGamePaused(Player) && Player->SetPause(true);
        }
    }
    // The full map is mouse-driven (wheel zoom, drag pan): show the cursor only while it is open.
    if (Player && (Next == EPanel::Atlas) != (Panel == EPanel::Atlas))
    {
        if (Next == EPanel::Atlas)
        {
            bRestoreCursor = Player->bShowMouseCursor;
            Player->bShowMouseCursor = true;
            ResetAtlasView();
            PanelOpened = FPlatformTime::Seconds();
        }
        else
        {
            Player->bShowMouseCursor = bRestoreCursor;
            bDragging = false;
        }
    }
    Panel = Next;
}

FVector2f UTrioCityHUD::ToLocal(const FPointerEvent& Event) const
{
    const FVector2f Local = GetCachedGeometry().AbsoluteToLocal(Event.GetScreenSpacePosition());
    return Local;
}

void UTrioCityHUD::ResetAtlasView()
{
    AtlasZoom = 1.f;
    AtlasCenter = FVector2f(.5f, .5f);
    bDragging = false;
}

void UTrioCityHUD::ClampAtlasView()
{
    AtlasZoom = FMath::Clamp(AtlasZoom, 1.f, MaxAtlasZoom);
    const float Half = .5f / AtlasZoom;
    AtlasCenter.X = FMath::Clamp(AtlasCenter.X, Half, 1.f - Half);
    AtlasCenter.Y = FMath::Clamp(AtlasCenter.Y, Half, 1.f - Half);
}

void UTrioCityHUD::ZoomAtlas(float Steps, FVector2f Pivot)
{
    if (AtlasRectSize <= 0.f)
        return;
    // Keep the map point under the pivot fixed while zooming.
    const FVector2f Local = (Pivot - AtlasRectOrigin) / AtlasRectSize;
    const FVector2f Clamped(FMath::Clamp(Local.X, 0.f, 1.f), FMath::Clamp(Local.Y, 0.f, 1.f));
    const FVector2f Before = AtlasCenter + (Clamped - FVector2f(.5f, .5f)) / AtlasZoom;
    AtlasZoom = FMath::Clamp(AtlasZoom * FMath::Pow(1.25f, Steps), 1.f, MaxAtlasZoom);
    AtlasCenter = Before - (Clamped - FVector2f(.5f, .5f)) / AtlasZoom;
    ClampAtlasView();
}

void UTrioCityHUD::PanAtlas(FVector2f DeltaUV)
{
    AtlasCenter += DeltaUV;
    ClampAtlasView();
}

void UTrioCityHUD::PointerDown(const FPointerEvent& Event)
{
    if (Panel != EPanel::Atlas || Event.GetEffectingButton() != EKeys::LeftMouseButton)
        return;
    bDragging = true;
    DragLast = ToLocal(Event);
}

bool UTrioCityHUD::PointerMove(const FPointerEvent& Event)
{
    if (!bDragging || AtlasRectSize <= 0.f)
        return false;
    const FVector2f Now = ToLocal(Event);
    PanAtlas(-(Now - DragLast) / (AtlasRectSize * AtlasZoom));
    DragLast = Now;
    return true;
}

int32 UTrioCityHUD::NativePaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
    FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const
{
    FDrawHUD Draw{Geometry, Elements, Super::NativePaint(Args, Geometry, CullingRect, Elements, Layer, Style, bParentEnabled)};
    const FVector2f Size(Geometry.GetLocalSize());
    if (Panel == EPanel::None)
    {
        LocalMap(Draw, AtlasBrush, PlayerUV, PlayerHeading, CameraHeading, bHasPawn, BeaconUV, bHasBeacon);
    }
    else
    {
        // Short open: dim and map fade in, the frame settles and the brackets close in (real time: the game is paused).
        const float Intro = FMath::InterpEaseOut(0.f, 1.f, FMath::Clamp(float(FPlatformTime::Seconds() - PanelOpened) / .22f, 0.f, 1.f), 3.f);
        Draw.Box({0, 0}, Size, FLinearColor(0.f, .006f, .004f, .80f * Intro));
        const float Span = 1.f / AtlasZoom;
        FullMap(Draw, Size, AtlasBrush, AtlasCenter - FVector2f(Span, Span) * .5f, Span, PlayerUV, PlayerHeading, CameraHeading,
            bHasPawn, BeaconUV, bHasBeacon, Intro, AtlasRectOrigin, AtlasRectSize);
    }
    return Draw.Layer;
}

#undef LOCTEXT_NAMESPACE
