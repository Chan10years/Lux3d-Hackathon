#include "TrioCombatHUD.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SWidget.h"
#include "UObject/UnrealType.h"

namespace
{
    const FLinearColor HeadColor(0.26f, 0.72f, 1.0f);
    const FLinearColor CritColor(1.0f, 0.29f, 0.10f);
    const FName Ids[] = {TEXT("Guns"), TEXT("Miyabi"), TEXT("Yousun")};

    void StyleText(UTextBlock& Text, int32 Size)
    {
        FSlateFontInfo Font = Text.GetFont();
        Font.Size = Size;
        Font.OutlineSettings.OutlineSize = 1;
        Text.SetFont(Font);
        Text.SetShadowOffset(FVector2D(1, 2));
        Text.SetShadowColorAndOpacity(FLinearColor(0.02f, 0.035f, 0.07f, 0.9f));
    }

    void Place(UCanvasPanel& Canvas, UWidget& Widget, FVector2D Anchor, FVector2D Position, FVector2D Size)
    {
        UCanvasPanelSlot* Slot = Canvas.AddChildToCanvas(&Widget);
        Slot->SetAnchors(FAnchors(Anchor.X, Anchor.Y));
        Slot->SetPosition(Position);
        Slot->SetSize(Size);
    }
}

void UTrioCombatHUD::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CombatOverlay"));
    WidgetTree->RootWidget = Root;
    SetVisibility(ESlateVisibility::HitTestInvisible);

    BotStatus = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BotStatus"));
    StyleText(*BotStatus, 12);
    BotStatus->SetText(NSLOCTEXT("TrioCombat", "BotsEnabled", "AI ON   [5]"));
    BotStatus->SetColorAndOpacity(FSlateColor(FLinearColor(.7f, .87f, 1.0f)));
    BotStatus->SetVisibility(ESlateVisibility::Collapsed);
    Place(*Root, *BotStatus, {0, 1}, {28, -184}, {160, 20});

    Charge = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SniperDamageMultiplier"));
    StyleText(*Charge, 17);
    Charge->SetColorAndOpacity(FSlateColor(FLinearColor(0.73f, 1.0f, 0.48f)));
    Charge->SetVisibility(ESlateVisibility::Collapsed);
    Place(*Root, *Charge, {0.5, 0.5}, {114, 84}, {150, 28});

    SweepCounter = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("GunSweepCounter"));
    StyleText(*SweepCounter, 14);
    SweepCounter->SetJustification(ETextJustify::Center);
    SweepCounter->SetVisibility(ESlateVisibility::Collapsed);
    Place(*Root, *SweepCounter, {0.5, 0.5}, {-43, 30}, {86, 21});
}

void UTrioCombatHUD::Update(const UTrioCombatWorld& Combat, APlayerController& Player, float Seconds)
{
    AActor* Pawn = Player.GetPawn();
    const FName Active = Combat.Identify(Pawn);
    const bool bCombatPawn = !Active.IsNone();
    BotStatus->SetVisibility(Combat.AreBotsEnabled() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

    const auto* Receiver = Active == Ids[0] ? Pawn->FindComponentByClass<UTrioCombatReceiver>() : nullptr;
    const int32 NewSweepShots = Receiver && !Receiver->IsDefeated() &&
        Receiver->ShowsGunSweepMeter(GetWorld()->GetTimeSeconds()) ? Receiver->GunSweepShotsFired : INDEX_NONE;
    if (SweepShots != NewSweepShots)
    {
        SweepShots = NewSweepShots;
        SweepCounter->SetVisibility(SweepShots >= 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
        if (SweepShots >= 0)
        {
            SweepCounter->SetText(FText::Format(NSLOCTEXT("TrioCombat", "GunSweepCount", "{0} / {1}"),
                FText::AsNumber(SweepShots), FText::AsNumber(UTrioCombatReceiver::MaxGunSweepShots)));
            SweepCounter->SetColorAndOpacity(FSlateColor(SweepShots == UTrioCombatReceiver::MaxGunSweepShots ?
                FLinearColor(1.0f, 0.45f, 0.27f) : FLinearColor(1.0f, 0.85f, 0.88f)));
        }
    }

    const auto* Scope = Pawn ? FindFProperty<FBoolProperty>(Pawn->GetClass(), TEXT("YS_ScopeActive")) : nullptr;
    const bool bScoped = bCombatPawn && Scope && Scope->GetPropertyValue_InContainer(Pawn);
    UpdateOverhead(Combat, Player, bScoped);
    if (bScoped != bScopeLayout)
    {
        bScopeLayout = bScoped;
        CastChecked<UCanvasPanelSlot>(BotStatus->Slot)->SetAnchors(bScoped ? FAnchors(0.09f, 0.88f) : FAnchors(0, 1));
    }
    Charge->SetVisibility(bScoped ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    if (bScoped)
    {
        const auto* Phase = FindFProperty<FIntProperty>(Pawn->GetClass(), TEXT("YS_CombatPhase"));
        const bool bFiring = Phase && Phase->GetPropertyValue_InContainer(Pawn) == 3;
        const auto* Attack = Combat.FindAttack(TEXT("Sniper"));
        if (Attack)
        {
            const double Multiplier = FMath::Lerp(Attack->Multiplier, Attack->FullChargeMultiplier,
                UTrioCombatLibrary::ScopeCharge(Pawn, bFiring));
            Charge->SetText(FText::Format(NSLOCTEXT("TrioCombat", "DamageMultiplier", "DMG {0}%"),
                FText::AsNumber(FMath::RoundToInt(Multiplier * 100))));
        }
    }

    for (int32 Index = Numbers.Num() - 1; Index >= 0; --Index)
    {
        FFloat& Number = Numbers[Index];
        Number.Age += Seconds;
        if (Number.Age >= 0.9)
        {
            Number.Text->RemoveFromParent();
            Numbers.RemoveAt(Index);
            continue;
        }
        FVector2D Screen;
        const bool bVisible = UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(&Player, Number.Location, Screen, true);
        Number.Text->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
        if (bVisible)
        {
            Number.Slot->SetPosition(Screen + FVector2D(Number.Offset, -14 - 74 * Number.Age));
            Number.Text->SetRenderOpacity(FMath::Clamp((0.9 - Number.Age) / 0.30, 0.0, 1.0));
        }
    }
}

void UTrioCombatHUD::AddDamage(FVector Location, const FTrioHitResult& Result)
{
    UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
    StyleText(*Text, Result.bCritical ? 26 : 20);
    Text->SetText(FText::AsNumber(FMath::RoundToInt(Result.Damage)));
    Text->SetColorAndOpacity(FSlateColor(Result.bCritical ? CritColor : Result.bHeadshot ? HeadColor : FLinearColor::White));
    Text->SetVisibility(ESlateVisibility::Collapsed);
    UCanvasPanelSlot* FloatSlot = Root->AddChildToCanvas(Text);
    FloatSlot->SetAutoSize(true);
    FloatSlot->SetAlignment({0.5, 0.5});
    // Spread consecutive hits around the impact point so that rapid-fire numbers stay readable.
    constexpr double LaneWidth = 48.0;
    const double LastOffset = Numbers.IsEmpty() ? 2 * LaneWidth : Numbers.Last().Offset;
    double Offset = 0;
    if (LastOffset < 0)
        Offset = -LastOffset;
    else if (LastOffset < 2 * LaneWidth)
        Offset = -LastOffset - LaneWidth;
    Numbers.Add({Text, FloatSlot, Location, 0, Offset});
}


void UTrioCombatHUD::UpdateOverhead(const UTrioCombatWorld& Combat, APlayerController& Player, bool bScoped)
{
    const FVector2D ViewSize = UWidgetLayoutLibrary::GetViewportSize(&Player) / UWidgetLayoutLibrary::GetViewportScale(&Player);
    if (ViewSize.IsNearlyZero())
        return;

    FVector Camera;
    FRotator ViewRotation;
    Player.GetPlayerViewPoint(Camera, ViewRotation);
    const double LensRadius = FMath::Min(ViewSize.X, ViewSize.Y) * 0.44;

    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Ids); ++Index)
    {
        FOverhead& Marker = Overhead[Index];
        Marker.Opacity = 0;
        const AActor* Target = Combat.FindRosterActor(Ids[Index]);
        const auto* Stats = Combat.FindStats(Ids[Index]);
        const auto* Vitals = Combat.FindVitals(Ids[Index]);
        if (!IsValid(Target) || !Stats || !Vitals || Vitals->Health <= 0 || Target->IsHidden())
            continue;

        Marker.bActive = Target == Player.GetPawn();
        if (bScoped && Marker.bActive)
            continue;

        FVector Top = Target->GetActorLocation() + FVector(0, 0, Target->GetSimpleCollisionHalfHeight());
        if (const ACharacter* Character = Cast<ACharacter>(Target))
        {
            const USkeletalMeshComponent* Mesh = Character->GetMesh();
            if (Mesh && Mesh->DoesSocketExist(TEXT("head")))
                Top = Mesh->GetSocketLocation(TEXT("head"));
            else if (Mesh && Mesh->DoesSocketExist(TEXT("Bip001-Head")))
                Top = Mesh->GetSocketLocation(TEXT("Bip001-Head"));
        }
        const FVector Anchor = Top + FVector(0, 0, 29);
        const double Distance = FVector::Distance(Camera, Anchor);
        if (Distance >= 3200 ||
            !UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(&Player, Anchor, Marker.Screen, true) ||
            Marker.Screen.X < 30 || Marker.Screen.X > ViewSize.X - 30 ||
            Marker.Screen.Y < 30 || Marker.Screen.Y > ViewSize.Y - 12 ||
            (bScoped && FVector2D::Distance(Marker.Screen, ViewSize * 0.5) >= LensRadius))
            continue;

        FCollisionQueryParams LineOfSight(SCENE_QUERY_STAT(TrioOverhead), false, Player.GetPawn());
        LineOfSight.AddIgnoredActor(Target);
        if (GetWorld()->LineTraceTestByChannel(Camera, Anchor, ECC_Visibility, LineOfSight))
            continue;

        Marker.Scale = FMath::Clamp(1.24 - Distance / 4200.0, 0.72, 1.12);
        Marker.Opacity = FMath::GetMappedRangeValueClamped(FVector2D(2250, 3200), FVector2D(1, 0), Distance);
        Marker.Shield = Stats->MaxShield > 0 ? FMath::Clamp(Vitals->Shield / Stats->MaxShield, 0.0, 1.0) : 0;
        Marker.Health = Stats->MaxHealth > 0 ? FMath::Clamp(Vitals->Health / Stats->MaxHealth, 0.0, 1.0) : 0;
    }

    if (const TSharedPtr<SWidget> Slate = GetCachedWidget())
        Slate->Invalidate(EInvalidateWidgetReason::Paint);
}

int32 UTrioCombatHUD::NativePaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
    FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const
{
    const int32 Base = Super::NativePaint(Args, Geometry, CullingRect, Elements, Layer, Style, bParentEnabled);
    const FColor Accent[] = {FColor(255, 82, 103), FColor(144, 169, 255), FColor(185, 243, 84)};
    TArray<FSlateVertex> Vertices;
    TArray<SlateIndex> Indices;
    const auto& Transform = Geometry.GetAccumulatedRenderTransform();

    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Overhead); ++Index)
    {
        const FOverhead& Marker = Overhead[Index];
        if (Marker.Opacity <= 0)
            continue;

        const FVector2f Origin = FVector2f(Marker.Screen) - FVector2f(67, 24) * Marker.Scale;
        const auto Quad = [&](FVector2f A, FVector2f B, FVector2f C, FVector2f D, FColor Tint)
        {
            const SlateIndex First = static_cast<SlateIndex>(Vertices.Num());
            Tint.A = FMath::RoundToInt(Tint.A * Marker.Opacity);
            for (const FVector2f Point : {A, B, C, D})
                Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform,
                    Origin + Point * Marker.Scale, FVector2f(0.5f, 0.5f), Tint));
            Indices.Append({First, static_cast<SlateIndex>(First + 1), static_cast<SlateIndex>(First + 2),
                First, static_cast<SlateIndex>(First + 2), static_cast<SlateIndex>(First + 3)});
        };
        const auto Slant = [&](float X, float Y, float Width, float Height, float Cut, FColor Tint)
        {
            if (Width < 0.35f)
                return;
            Cut = FMath::Min(Cut, Width * 0.38f);
            Quad({X + Cut, Y}, {X + Width, Y}, {X + Width - Cut, Y + Height}, {X, Y + Height}, Tint);
        };
        const FColor Role = Accent[Index];

        // Charcoal plate and a slim metallic edge keep the bar legible against the sky.
        Slant(1, 3, 135, 24, 8, FColor(0, 0, 0, 112));
        Slant(0, 0, 134, 24, 8, FColor(11, 14, 22, 242));
        Slant(8, 0, 123, 1.4f, 1.4f, Marker.bActive ? Role : FColor(143, 160, 187, 174));

        // Shield uses four pale segments. Health reads as a single, thicker role-colored bar.
        for (int32 Segment = 0; Segment < 4; ++Segment)
        {
            const float X = 26 + Segment * 25.5f;
            const float Fill = FMath::Clamp(Marker.Shield * 4 - Segment, 0.0f, 1.0f);
            Slant(X, 4, 24, 6, 2, FColor(55, 66, 89));
            Slant(X, 4, 24 * Fill, 6, 2, FColor(202, 228, 255));
        }
        Slant(24, 13, 100, 8, 3.6f, FColor(46, 49, 63));
        Slant(24, 13, 100 * Marker.Health, 8, 3.6f, Role);

        // Individual marks echo the weapons without adding text at this size.
        if (Index == 0)
        {
            Slant(7, 7, 7, 10, 2.6f, Role);
            Slant(14, 7, 7, 10, 2.6f, Role);
        }
        else if (Index == 1)
        {
            Quad({15, 5}, {21.5f, 11.5f}, {15, 18}, {8.5f, 11.5f}, Role);
            Quad({15, 8.5f}, {18, 11.5f}, {15, 14.5f}, {12, 11.5f}, FColor(11, 14, 22));
        }
        else
        {
            Slant(9, 10.5f, 12, 2, 0, Role);
            Slant(14, 5.5f, 2, 12, 0, Role);
            Slant(13, 9.5f, 4, 4, 0, FColor(11, 14, 22));
        }
    }

    if (SweepShots >= 0)
    {
        const FVector2f Center = FVector2f(Geometry.GetLocalSize() * 0.5);
        constexpr int32 SegmentCount = UTrioCombatReceiver::MaxGunSweepShots;
        constexpr float ArcStart = -64.0f;
        constexpr float SegmentAngle = 128.0f / SegmentCount;
        const FColor Hot(255, 122, 68);
        const FColor Filled(255, 80, 111);
        const auto DrawBand = [&](float Start, float End, float Inner, float Outer, FColor Tint)
        {
            const auto Point = [&](float Degrees, float Radius)
            {
                const float Angle = FMath::DegreesToRadians(Degrees);
                return Center + FVector2f(FMath::Sin(Angle), FMath::Cos(Angle)) * Radius;
            };
            const SlateIndex First = static_cast<SlateIndex>(Vertices.Num());
            for (const FVector2f Position : {Point(Start, Inner), Point(End, Inner), Point(End, Outer), Point(Start, Outer)})
                Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Position, FVector2f(0.5f, 0.5f), Tint));
            Indices.Append({First, static_cast<SlateIndex>(First + 1), static_cast<SlateIndex>(First + 2),
                First, static_cast<SlateIndex>(First + 2), static_cast<SlateIndex>(First + 3)});
        };

        for (int32 Slice = 0; Slice < 48; ++Slice)
        {
            const float Start = ArcStart + (128.0f / 48) * Slice;
            DrawBand(Start, Start + 128.0f / 48, 52, 69, FColor(14, 11, 19, 235));
        }
        for (int32 Segment = 0; Segment < SegmentCount; ++Segment)
        {
            const float Start = ArcStart + Segment * SegmentAngle + 1.2f;
            const float End = Start + SegmentAngle - 2.4f;
            const bool bFired = Segment < SweepShots;
            const FColor Tint = !bFired ? FColor(71, 62, 78) : SweepShots == SegmentCount ? Hot : Filled;
            for (int32 Slice = 0; Slice < 3; ++Slice)
                DrawBand(FMath::Lerp(Start, End, Slice / 3.0f), FMath::Lerp(Start, End, (Slice + 1) / 3.0f), 56, 65, Tint);
        }
    }

    if (Vertices.IsEmpty())
        return Base;

    const FSlateBrush* Brush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
    const FSlateResourceHandle Resource = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Brush);
    FSlateDrawElement::MakeCustomVerts(Elements, Base + 1, Resource, Vertices, Indices, nullptr, 0, 0);
    return Base + 1;
}
