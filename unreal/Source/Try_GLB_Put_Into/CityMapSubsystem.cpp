#include "CityMapSubsystem.h"
#include "CityMapProjection.h"
#include "CombatMatchActor.h"
#include "GoldenHammerActor.h"
#include "WeaponForgeSubsystem.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/InputComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SLeafWidget.h"

namespace
{
using FMapPath = TArray<CityMap::Point>;
struct FCityRoadData
{
    CityMap::Bounds Bounds{};
    TArray<FMapPath> Roads, Blocks;
    bool Load()
    {
        FString Text;
        TSharedPtr<FJsonObject> Root;
        if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectContentDir() / TEXT("Map/ZZZ_roads.json")))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root) return false;
        double Version = 0;
        const TArray<TSharedPtr<FJsonValue>>* Area;
        if (!Root->TryGetNumberField(TEXT("version"), Version) || Version != 1
            || !Root->TryGetArrayField(TEXT("world_bounds"), Area) || Area->Num() != 4) return false;
        double Values[4];
        for (int32 I=0; I<4; ++I)
            if (!(*Area)[I]->TryGetNumber(Values[I]) || !FMath::IsFinite(Values[I])) return false;
        Bounds={Values[0],Values[1],Values[2],Values[3]};
        if (Bounds.MaxX <= Bounds.MinX || Bounds.MaxY <= Bounds.MinY) return false;
        auto ReadPaths = [&](const TCHAR* Name, TArray<FMapPath>& Out)
        {
            const TArray<TSharedPtr<FJsonValue>>* Paths;
            if (!Root->TryGetArrayField(Name, Paths) || Paths->Num() > 4096) return false;
            for (const auto& Value : *Paths)
            {
                const TArray<TSharedPtr<FJsonValue>>* Points;
                if (!Value->TryGetArray(Points) || Points->Num()<2 || Points->Num()>256) return false;
                FMapPath Path;
                for (const auto& Point : *Points)
                {
                    const TArray<TSharedPtr<FJsonValue>>* XY;
                    double X,Y;
                    if (!Point->TryGetArray(XY) || XY->Num()!=2 || !(*XY)[0]->TryGetNumber(X)
                        || !(*XY)[1]->TryGetNumber(Y) || !FMath::IsFinite(X) || !FMath::IsFinite(Y)) return false;
                    Path.Add({X,Y});
                }
                Out.Add(MoveTemp(Path));
            }
            return true;
        };
        return ReadPaths(TEXT("roads"), Roads) && Roads.Num()>0 && ReadPaths(TEXT("blocks"), Blocks);
    }
};
}

class SCityMapWidget final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SCityMapWidget) {} SLATE_END_ARGS()
    void Construct(const FArguments&) { SetVisibility(EVisibility::HitTestInvisible); }
    bool Load() { return Data.Load(); }
    void Update(FVector Player, double Yaw, TOptional<FVector> Hammer, bool Expanded, bool HasPlayer)
    {
        if (HasPlayer) { Centre={Player.X,Player.Y}; bHasCentre=true; }
        CameraYaw=Yaw; Objective=Hammer; bFull=Expanded; bDrawPlayer=HasPlayer;
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1,1); }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&,
        FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const override
    {
        if (!bHasCentre) return Layer;
        const FVector2D Size(G.GetLocalSize());
        const double Radius=FMath::Min(120., FMath::Max(65., (Size.Y-260.)*.5));
        const FVector2D MiniCentre(16+Radius,76+Radius);
        const FVector2D FullOrigin(40,96), FullSize(FMath::Max(1.,Size.X-80),FMath::Max(1.,Size.Y-156));
        const FLinearColor Blue(.08f,.48f,1.f), Gold(1.f,.72f,.06f);
        const auto WhiteResource=FSlateApplication::Get().GetRenderer()->GetResourceHandle(
            *FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")));
        auto AddPolygon = [&](const std::vector<CityMap::Point>& Polygon, FVector2D Origin, FColor Color,
            TArray<FSlateVertex>& Vertices, TArray<SlateIndex>& Indices)
        {
            if (Polygon.size()<3) return;
            const int32 Start=Vertices.Num();
            for (const auto P : Polygon)
                Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),
                    FVector2f(Origin+FVector2D(P.X,P.Y)),FVector2f(.5f,.5f),Color));
            for (int32 I=1; I+1<static_cast<int32>(Polygon.size()); ++I)
            {
                Indices.Add(static_cast<SlateIndex>(Start));
                Indices.Add(static_cast<SlateIndex>(Start+I));
                Indices.Add(static_cast<SlateIndex>(Start+I+1));
            }
        };
        auto Fill = [&](const TArray<FSlateVertex>& Vertices, const TArray<SlateIndex>& Indices, int32 Offset)
        {
            if (!Indices.IsEmpty()) FSlateDrawElement::MakeCustomVerts(Out,Layer+Offset,WhiteResource,
                Vertices,Indices,nullptr,0,0);
        };
        auto Line = [&](const TArray<FVector2f>& Points, FLinearColor Color, float Width, int32 Offset)
        {
            FSlateDrawElement::MakeLines(Out,Layer+Offset,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Width);
        };
        auto Circle = [&](FVector2D CentreAt, double R, FLinearColor Color, float Width, int32 Offset)
        {
            TArray<FVector2f> Points;
            for (int32 I=0; I<=48; ++I)
            {
                const double A=2*PI*I/48;
                Points.Add(FVector2f(CentreAt+FVector2D(FMath::Cos(A),FMath::Sin(A))*R));
            }
            Line(Points,Color,Width,Offset);
        };
        auto Box = [&](FVector2D At, FVector2D Extent, FLinearColor Color)
        {
            FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(FVector2f(Extent),FSlateLayoutTransform(FVector2f(At))),
                FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")),ESlateDrawEffect::None,Color);
        };
        auto Text = [&](FVector2D At, const FString& Message, int32 FontSize, FLinearColor Color)
        {
            FSlateDrawElement::MakeText(Out,Layer+6,G.ToPaintGeometry(FVector2f(1,1),FSlateLayoutTransform(FVector2f(At))),
                Message,FCoreStyle::GetDefaultFontStyle("Regular",FontSize),ESlateDrawEffect::None,Color);
        };
        if (bFull)
        {
            Box(FVector2D::ZeroVector,Size,FLinearColor(0,0,0,.65f));
            Text({40,56},TEXT("城市地图  ·  M 收起  ·  角色操作不受影响"),18,FLinearColor::White);
        }
        else
        {
            // One filled disk keeps the black background uniformly translucent.
            std::vector<CityMap::Point> Disk;
            for (int32 I=0; I<48; ++I)
                Disk.push_back({Radius*FMath::Cos(2*PI*I/48),Radius*FMath::Sin(2*PI*I/48)});
            TArray<FSlateVertex> Vertices;
            TArray<SlateIndex> Indices;
            AddPolygon(Disk,MiniCentre,FColor(0,0,0,166),Vertices,Indices);
            Fill(Vertices,Indices,0);
        }
        auto Project = [&](CityMap::Point World)
        {
            return bFull ? CityMap::ProjectFullMap(World,Data.Bounds,{FullSize.X,FullSize.Y})
                : CityMap::ProjectMiniMap(World,Centre,CameraYaw,10000,Radius-8);
        };
        auto Screen = [&](CityMap::Point P)
        {
            return (bFull ? FullOrigin : MiniCentre) + FVector2D(P.X,P.Y);
        };
        // Solid white neighbourhood blocks; the translucent black gaps are the road network.
        TArray<FSlateVertex> BlockVertices;
        TArray<SlateIndex> BlockIndices;
        for (const auto& Path : Data.Blocks)
        {
            std::vector<CityMap::Point> Polygon;
            for (const auto P : Path) Polygon.push_back(Project(P));
            if (!bFull) Polygon=CityMap::ClipPolygonToCircle(MoveTemp(Polygon),Radius-2);
            AddPolygon(Polygon,bFull ? FullOrigin : MiniCentre,FColor::White,BlockVertices,BlockIndices);
        }
        Fill(BlockVertices,BlockIndices,1);
        if (!bFull) Circle(MiniCentre,Radius,FLinearColor(.36f,.46f,.55f),2,3);
        auto MarkerPosition = [&](FVector World)
        {
            auto P=Project({World.X,World.Y});
            if (!bFull) P=CityMap::ClampMarkerToCircle(P,Radius-14);
            return Screen(P);
        };
        bool bSelfHolds=false;
        if (Objective.IsSet())
        {
            const FVector Value=Objective.GetValue();
            bSelfHolds=bDrawPlayer && FVector2D::Distance(FVector2D(Value.X,Value.Y),FVector2D(Centre.X,Centre.Y))<1.;
            Circle(MarkerPosition(Value),bSelfHolds ? 12 : 8,Gold,4,4);
        }
        if (bDrawPlayer)
        {
            const FVector2D P=Screen(Project(Centre));
            Circle(P,7,Blue,5,5);
            // The blue circle remains the player marker; a short tick clarifies camera forward.
            const double YawRad=CameraYaw*PI/180;
            const FVector2D Facing=bFull ? FVector2D(FMath::Cos(YawRad),FMath::Sin(YawRad)) : FVector2D(0,-1);
            Line({FVector2f(P+Facing*11),FVector2f(P+Facing*17)},Blue,2,5);
        }
        if (bFull)
            Text({40,Size.Y-36},TEXT("蓝圈：你自己     金圈：溯源信标 / 持有者"),14,FLinearColor(.8f,.87f,.94f));
        else
            Text({16,MiniCentre.Y+Radius+8},TEXT("M 地图   ·   金圈追踪溯源信标"),12,FLinearColor(.8f,.87f,.94f));
        return Layer+6;
    }
private:
    FCityRoadData Data;
    CityMap::Point Centre;
    double CameraYaw=0;
    TOptional<FVector> Objective;
    bool bHasCentre=false, bDrawPlayer=false, bFull=false;
};

bool UCityMapSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const auto* World=Cast<UWorld>(Outer);
    if (!World || !World->IsGameWorld() || World->GetNetMode()==NM_DedicatedServer) return false;
    // When TrioCityHUD's stylized radar atlas is available, let TrioCityHUD own the minimap/full-map without double-drawing.
    if (FPackageName::DoesPackageExist(TEXT("/Game/CityZZZ/Tex/HUD/T_CityAtlas"))) return false;
    return Super::ShouldCreateSubsystem(Outer);
}

bool UCityMapSubsystem::IsCity() const
{
    return GetWorld() && GetWorld()->GetMapName().EndsWith(TEXT("Lvl_ZZZ"));
}

TStatId UCityMapSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UCityMapSubsystem,STATGROUP_Tickables);
}

void UCityMapSubsystem::ToggleMap()
{
    if (!IsCity() || !MapWidget) return;
    const auto* Forge=GetWorld()->GetSubsystem<UWeaponForgeSubsystem>();
    if (Forge && Forge->IsPanelOpen()) return;
    if (FSlateApplication::IsInitialized())
    {
        const auto Focus=FSlateApplication::Get().GetKeyboardFocusedWidget();
        if (Focus && Focus->GetTypeAsString().Contains(TEXT("EditableText"))) return;
    }
    bExpanded=!bExpanded;
}

void UCityMapSubsystem::Tick(float)
{
    UWorld* World=GetWorld();
    if (!World || !World->HasBegunPlay() || !IsCity() || !World->GetGameViewport()) return;
    auto* PC=World->GetFirstPlayerController();
    if (!PC || !PC->IsLocalPlayerController()) return;
    if (Controller.Get()!=PC)
    {
        if (Controller.IsValid() && MapInput) Controller->PopInputComponent(MapInput);
        Controller=PC;
        MapInput=NewObject<UInputComponent>(PC);
        MapInput->Priority=15;
        MapInput->RegisterComponent();
        MapInput->BindKey(EKeys::M,IE_Pressed,this,&UCityMapSubsystem::ToggleMap).bConsumeInput=false;
        PC->PushInputComponent(MapInput);
    }
    if (!bLoadAttempted)
    {
        bLoadAttempted=true;
        MapWidget=SNew(SCityMapWidget);
        if (!MapWidget->Load())
        {
            UE_LOG(LogTemp,Warning,TEXT("[CityMap] Missing/invalid Content/Map/ZZZ_roads.json; map disabled, gameplay unaffected"));
            MapWidget.Reset();
            return;
        }
        World->GetGameViewport()->AddViewportWidgetContent(MapWidget.ToSharedRef(),70);
        UE_LOG(LogTemp,Display,TEXT("[CityMap] ZZZ road data loaded; M toggles map, local player and objective only"));
    }
    if (!MapWidget) return;
    const auto* Forge=World->GetSubsystem<UWeaponForgeSubsystem>();
    MapWidget->SetVisibility(Forge && Forge->IsPanelOpen() ? EVisibility::Collapsed : EVisibility::HitTestInvisible);
    const APawn* Pawn=PC->GetPawn();
    TOptional<FVector> HammerPosition;
    if (const auto* Match=ACombatMatchActor::Find(World))
        if (const auto* Hammer=Match->GetHammer())
            HammerPosition=Pawn && Hammer->GetHolder()==Pawn ? Pawn->GetActorLocation() : Hammer->GetMapPosition();
    const double Yaw=PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraRotation().Yaw : PC->GetControlRotation().Yaw;
    MapWidget->Update(Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector,Yaw,HammerPosition,bExpanded,IsValid(Pawn));
}

void UCityMapSubsystem::Deinitialize()
{
    if (Controller.IsValid() && MapInput) Controller->PopInputComponent(MapInput);
    if (MapWidget && GetWorld() && GetWorld()->GetGameViewport())
        GetWorld()->GetGameViewport()->RemoveViewportWidgetContent(MapWidget.ToSharedRef());
    MapWidget.Reset();
    Controller.Reset();
    Super::Deinitialize();
}
