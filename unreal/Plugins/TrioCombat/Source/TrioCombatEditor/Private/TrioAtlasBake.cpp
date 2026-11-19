#include "TrioAtlasBake.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // World square covered by T_CityAtlas (UE cm, X north, Y east). Keep in sync with TrioCityHUD.cpp.
    constexpr double CenterX = -16100.0;
    constexpr double CenterY = 0.0;
    constexpr double Span = 122880.0;
    // 4 x 4 orthographic tiles of 2048 px: 15 cm per pixel, downsampled to the 4096 px texture offline.
    constexpr int32 Tile = 2048;
    constexpr int32 Tiles = 4;
    constexpr double TileWorld = Span / Tiles;
    constexpr double CameraZ = 60000.0;
    // Captures per tile before reading back, so Nanite pages and texture mips stream in.
    constexpr int32 WarmFrames = 8;

    enum ELayer : int32 { Height, Base, Lit, LayerCount };

    struct FBakeState
    {
        TStrongObjectPtr<USceneCaptureComponent2D> Captures[LayerCount];
        TStrongObjectPtr<UTextureRenderTarget2D> Targets[LayerCount];
        TWeakObjectPtr<UWorld> World;
        int32 TileIndex = 0;
        int32 Frame = 0;
        double Started = 0.0;
    };

    TUniquePtr<FBakeState> Bake;
    FTSTicker::FDelegateHandle TickHandle;
    double NextPoll = 0.0;

    FString OutputDir() { return FPaths::ProjectSavedDir() / TEXT("ZZZ/atlas"); }
    FString FlagPath() { return FPaths::ProjectSavedDir() / TEXT("ZZZ/atlas_bake.flag"); }

    USceneCaptureComponent2D* MakeCapture(UWorld& World, ESceneCaptureSource Source, EPixelFormat Format,
        TStrongObjectPtr<UTextureRenderTarget2D>& OutTarget)
    {
        UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
        Target->ClearColor = FLinearColor::Black;
        Target->InitCustomFormat(Tile, Tile, Format, true);
        Target->UpdateResourceImmediate(true);
        OutTarget.Reset(Target);

        USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage());
        Capture->ProjectionType = ECameraProjectionMode::Orthographic;
        Capture->OrthoWidth = TileWorld;
        Capture->bAutoCalculateOrthoPlanes = false;
        Capture->bUpdateOrthoPlanes = false;
        Capture->CaptureSource = Source;
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->bAlwaysPersistRenderingState = true;
        Capture->TextureTarget = Target;
        // A map wants plain albedo and form: no haze, no screen-space or temporal effects that would seam between tiles,
        // and no virtual shadow maps (first-frame VSM work is what hangs this GPU).
        for (const TCHAR* Flag : {TEXT("Fog"), TEXT("VolumetricFog"), TEXT("Cloud"), TEXT("Atmosphere"), TEXT("Particles"),
                 TEXT("MotionBlur"), TEXT("Bloom"), TEXT("DepthOfField"), TEXT("AntiAliasing"), TEXT("TemporalAA"),
                 TEXT("DynamicShadows"), TEXT("ContactShadows"), TEXT("AmbientOcclusion"), TEXT("DistanceFieldAO"),
                 TEXT("ScreenSpaceReflections"), TEXT("ScreenSpaceAO"), TEXT("LumenGlobalIllumination"), TEXT("LumenReflections"),
                 TEXT("GlobalIllumination"), TEXT("EyeAdaptation"), TEXT("LensFlares"), TEXT("Vignette"), TEXT("Grain"),
                 TEXT("VolumetricLightmap"), TEXT("LightShafts")})
        {
            const int32 Index = FEngineShowFlags::FindIndexByName(Flag);
            if (Index != INDEX_NONE)
                Capture->ShowFlags.SetSingleFlag(Index, false);
        }
        Capture->RegisterComponentWithWorld(&World);
        return Capture;
    }

    void Finish(const TCHAR* Result)
    {
        if (!Bake)
            return;
        for (TStrongObjectPtr<USceneCaptureComponent2D>& Capture : Bake->Captures)
            if (Capture && Capture->IsRegistered())
                Capture->UnregisterComponent();
        UE_LOG(LogTemp, Display, TEXT("TrioAtlasBake %s after %.1f s"), Result, FPlatformTime::Seconds() - Bake->Started);
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"result\":\"%s\",\"tiles\":%d,\"tile\":%d,\"centerX\":%.1f,\"centerY\":%.1f,\"span\":%.1f,\"cameraZ\":%.1f}"),
            Result, Tiles, Tile, CenterX, CenterY, Span, CameraZ), *(OutputDir() / TEXT("done.json")));
        Bake.Reset();
    }

    bool SaveTile(int32 TileX, int32 TileY)
    {
        const FString Stem = OutputDir() / FString::Printf(TEXT("%d_%d"), TileX, TileY);

        TArray<FLinearColor> Depth;
        if (!Bake->Targets[Height]->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Depth) || Depth.Num() != Tile * Tile)
            return false;
        TArray<float> Heights;
        Heights.SetNumUninitialized(Depth.Num());
        for (int32 Index = 0; Index < Depth.Num(); ++Index)
            Heights[Index] = float(CameraZ - Depth[Index].R);
        Depth.Empty();
        if (!FFileHelper::SaveArrayToFile(TArrayView<const uint8>(reinterpret_cast<const uint8*>(Heights.GetData()), Heights.Num() * sizeof(float)),
                *(Stem + TEXT("_height.f32"))))
            return false;

        TArray<FFloat16Color> Albedo;
        if (!Bake->Targets[Base]->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Albedo) || Albedo.Num() != Tile * Tile)
            return false;
        TArray<FColor> AlbedoSRGB;
        AlbedoSRGB.SetNumUninitialized(Albedo.Num());
        for (int32 Index = 0; Index < Albedo.Num(); ++Index)
            AlbedoSRGB[Index] = FLinearColor(Albedo[Index]).ToFColorSRGB();
        if (!FImageUtils::SaveImageByExtension(*(Stem + TEXT("_base.png")), FImageView(AlbedoSRGB.GetData(), Tile, Tile)))
            return false;

        TArray<FFloat16Color> Lit;
        if (!Bake->Targets[ELayer::Lit]->GameThread_GetRenderTargetResource()->ReadFloat16Pixels(Lit) || Lit.Num() != Tile * Tile)
            return false;
        return FImageUtils::SaveImageByExtension(*(Stem + TEXT("_lit.exr")), FImageView(Lit.GetData(), Tile, Tile));
    }

    void Step()
    {
        UWorld* World = Bake->World.Get();
        if (!World || (GEditor && GEditor->PlayWorld))
        {
            Finish(TEXT("aborted"));
            return;
        }
        const int32 TileX = Bake->TileIndex % Tiles;
        const int32 TileY = Bake->TileIndex / Tiles;
        // Pitch -90 / yaw 0 looks straight down with +Y to the right and +X up: the texture's east and north.
        const FVector Location(CenterX + Span * .5 - (TileY + .5) * TileWorld, CenterY - Span * .5 + (TileX + .5) * TileWorld, CameraZ);
        for (TStrongObjectPtr<USceneCaptureComponent2D>& Capture : Bake->Captures)
        {
            Capture->SetWorldLocationAndRotation(Location, FRotator(-90.0, 0.0, 0.0));
            Capture->CaptureScene();
        }
        if (++Bake->Frame < WarmFrames)
            return;
        if (!SaveTile(TileX, TileY))
        {
            Finish(TEXT("failed"));
            return;
        }
        UE_LOG(LogTemp, Display, TEXT("TrioAtlasBake tile %d/%d"), Bake->TileIndex + 1, Tiles * Tiles);
        Bake->Frame = 0;
        if (++Bake->TileIndex == Tiles * Tiles)
            Finish(TEXT("ok"));
    }

    void Begin(UWorld& World)
    {
        IFileManager::Get().Delete(*FlagPath());
        IFileManager::Get().DeleteDirectory(*OutputDir(), false, true);
        IFileManager::Get().MakeDirectory(*OutputDir(), true);
        Bake = MakeUnique<FBakeState>();
        Bake->World = &World;
        Bake->Started = FPlatformTime::Seconds();
        Bake->Captures[Height].Reset(MakeCapture(World, SCS_SceneDepth, PF_A32B32G32R32F, Bake->Targets[Height]));
        Bake->Captures[Base].Reset(MakeCapture(World, SCS_BaseColor, PF_FloatRGBA, Bake->Targets[Base]));
        Bake->Captures[Lit].Reset(MakeCapture(World, SCS_SceneColorHDRNoAlpha, PF_FloatRGBA, Bake->Targets[Lit]));
        UE_LOG(LogTemp, Display, TEXT("TrioAtlasBake started in %s"), *World.GetName());
    }

    bool Tick(float)
    {
        if (Bake)
        {
            Step();
            return true;
        }
        const double Now = FPlatformTime::Seconds();
        if (Now < NextPoll)
            return true;
        NextPoll = Now + 1.0;
        if (!GEditor || GEditor->PlayWorld || !IFileManager::Get().FileExists(*FlagPath()))
            return true;
        if (UWorld* World = GEditor->GetEditorWorldContext().World())
            Begin(*World);
        return true;
    }
}

void TrioAtlasBake::Startup()
{
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
}

void TrioAtlasBake::Shutdown()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    Bake.Reset();
}
