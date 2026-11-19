#include "WeaponForgeSubsystem.h"
#include "RoomCodeSubsystem.h"
#include "WeaponSyncComponent.h"
#include "PlayerCombatComponent.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "UnrealClient.h"

#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "ImageUtils.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "glTFRuntimeAssetActor.h"
#include "glTFRuntimeFunctionLibrary.h"

namespace
{
	constexpr int32 CanvasSize = 320;
	constexpr int32 MaxGlbBytes = 32 * 1024 * 1024;
	constexpr double JobTimeout = 1800;
	constexpr double ForgeCooldown = 180;
	const FName WeaponTag(TEXT("GeneratedWeapon"));
	const FName HandSocket(TEXT("hand_r"));
	struct FForgeSwatch
	{
		const TCHAR* Name;
		FColor Color;
	};
	const FForgeSwatch ForgeSwatches[] = {
		{TEXT("黑色"), FColor::Black}, {TEXT("红色"), FColor(221, 56, 54)},
		{TEXT("橙色"), FColor(223, 110, 35)}, {TEXT("金黄"), FColor(201, 137, 16)},
		{TEXT("绿色"), FColor(28, 167, 88)}, {TEXT("青色"), FColor(19, 155, 173)},
		{TEXT("蓝色"), FColor(50, 109, 226)}, {TEXT("紫色"), FColor(134, 78, 204)},
		{TEXT("粉色"), FColor(216, 75, 143)}, {TEXT("棕色"), FColor(137, 77, 42)},
	};

	FText Label(const TCHAR* Text) { return FText::FromString(Text); }
	double Now() { return FPlatformTime::Seconds(); }

	bool IsSafeLocalService(const FString& Url)
	{
		return (Url.StartsWith(TEXT("http://")) || Url.StartsWith(TEXT("https://")))
			&& Url.Len() <= 512 && !Url.Contains(TEXT(" ")) && !Url.Contains(TEXT("?")) && !Url.Contains(TEXT("#"));
	}

	void AppendUtf8(TArray<uint8>& Bytes, const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	bool IsGlb(const TArray<uint8>& Bytes)
	{
		return Bytes.Num() >= 20 && Bytes.Num() <= MaxGlbBytes
			&& FMemory::Memcmp(Bytes.GetData(), "glTF", 4) == 0;
	}
}

class SForgeCanvas final : public SLeafWidget
{
	struct FStroke
	{
		FColor Color;
		TArray<FVector2D> Points;
	};

public:
	SLATE_BEGIN_ARGS(SForgeCanvas) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&) {}
	void Clear() { Strokes.Reset(); Invalidate(EInvalidateWidgetReason::Paint); }
	bool HasDrawing() const { return !Strokes.IsEmpty(); }
	FColor GetBrushColor() const { return BrushColor; }
	void SetBrushColor(FColor Color) { BrushColor = Color; }
	void Undo()
	{
		if (!HasDrawing()) return;
		Strokes.Pop();
		Invalidate(EInvalidateWidgetReason::Paint);
	}

#if !UE_BUILD_SHIPPING
	void SeedTestSketch(bool bWithColors = false)
	{
		if (bWithColors) SetBrushColor(ForgeSwatches[1].Color);
		BeginStroke().Points = {{160, 42}, {185, 80}, {182, 196}, {138, 196}, {135, 80}, {160, 42}};
		if (bWithColors) SetBrushColor(ForgeSwatches[3].Color);
		BeginStroke().Points = {{110, 202}, {210, 202}};
		if (bWithColors) SetBrushColor(ForgeSwatches[6].Color);
		BeginStroke().Points = {{160, 205}, {160, 272}};
		if (bWithColors)
		{
			BeginStroke().Points = {{26, 26}, {45, 45}};
			Undo();
		}
		Invalidate(EInvalidateWidgetReason::Paint);
	}
#endif

	TArray64<uint8> EncodePng() const
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor::White, CanvasSize * CanvasSize);
		auto Dot = [&Pixels](const FVector2D& Point, FColor Color)
		{
			const int32 X = FMath::RoundToInt(Point.X);
			const int32 Y = FMath::RoundToInt(Point.Y);
			for (int32 Dy = -4; Dy <= 4; ++Dy)
			{
				for (int32 Dx = -4; Dx <= 4; ++Dx)
				{
					if (Dx * Dx + Dy * Dy <= 16 && X + Dx >= 0 && X + Dx < CanvasSize
						&& Y + Dy >= 0 && Y + Dy < CanvasSize)
					{
						Pixels[(Y + Dy) * CanvasSize + X + Dx] = Color;
					}
				}
			}
		};
		for (const FStroke& Stroke : Strokes)
		{
			const TArray<FVector2D>& Points = Stroke.Points;
			Dot(Points[0], Stroke.Color);
			for (int32 I = 1; I < Points.Num(); ++I)
			{
				const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(Points[I - 1], Points[I])));
				for (int32 J = 1; J <= Steps; ++J)
					Dot(FMath::Lerp(Points[I - 1], Points[I], static_cast<double>(J) / Steps), Stroke.Color);
			}
		}
		TArray64<uint8> Result;
		FImageUtils::PNGCompressImageArray(CanvasSize, CanvasSize, MakeArrayView64(Pixels), Result);
		return Result;
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(CanvasSize); }

	virtual FReply OnMouseButtonDown(const FGeometry& G, const FPointerEvent& E) override
	{
		if (E.GetEffectingButton() != EKeys::LeftMouseButton || Strokes.Num() >= 256) return FReply::Unhandled();
		BeginStroke();
		AddPoint(G, E);
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}

	virtual FReply OnMouseMove(const FGeometry& G, const FPointerEvent& E) override
	{
		if (!HasMouseCapture() || !E.IsMouseButtonDown(EKeys::LeftMouseButton) || Strokes.IsEmpty())
			return FReply::Unhandled();
		AddPoint(G, E);
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent& E) override
	{
		return E.GetEffectingButton() == EKeys::LeftMouseButton && HasMouseCapture()
			? FReply::Handled().ReleaseMouseCapture() : FReply::Unhandled();
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&,
		FSlateWindowElementList& Out, int32 Layer, const FWidgetStyle&, bool) const override
	{
		const FVector2D Scale = FVector2D(G.GetLocalSize()) / CanvasSize;
		for (const FStroke& Stroke : Strokes)
		{
			TArray<FVector2f> Points;
			Points.Reserve(FMath::Max(2, Stroke.Points.Num()));
			for (const FVector2D& P : Stroke.Points) Points.Emplace(FVector2f(P * Scale));
			if (Points.Num() == 1) Points.Add(Points[0] + FVector2f(0.1f, 0));
			FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), Points,
				ESlateDrawEffect::None, FLinearColor::FromSRGBColor(Stroke.Color), true, 7.0f);
		}
		return Layer;
	}

private:
	FStroke& BeginStroke()
	{
		FStroke& Stroke = Strokes.AddDefaulted_GetRef();
		Stroke.Color = BrushColor;
		return Stroke;
	}

	void AddPoint(const FGeometry& G, const FPointerEvent& E)
	{
		const FVector2D P = FVector2D(G.AbsoluteToLocal(E.GetScreenSpacePosition())) / FVector2D(G.GetLocalSize()) * CanvasSize;
		TArray<FVector2D>& Stroke = Strokes.Last().Points;
		if (Stroke.Num() >= 2048) return;
		Stroke.Emplace(FMath::Clamp(P.X, 0.0, double(CanvasSize - 1)), FMath::Clamp(P.Y, 0.0, double(CanvasSize - 1)));
		Invalidate(EInvalidateWidgetReason::Paint);
	}
	TArray<FStroke> Strokes;
	FColor BrushColor = FColor::Black;
};

bool UWeaponForgeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World || !World->IsGameWorld()) return false;
	if (World->GetMapName().EndsWith(TEXT("Lvl_Front"))) return false;
	return Super::ShouldCreateSubsystem(Outer);
}

TStatId UWeaponForgeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWeaponForgeSubsystem, STATGROUP_Tickables);
}

void UWeaponForgeSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || !World->HasBegunPlay()) return;
	if (World->GetNetMode() != NM_Client)
	{
		for (TActorIterator<ACharacter> It(World); It; ++It)
		{
			ACharacter* Player = *It;
			if (!Player->IsPlayerControlled() || Player->FindComponentByClass<UWeaponSyncComponent>()) continue;
			auto* Sync = NewObject<UWeaponSyncComponent>(Player, TEXT("WeaponEquipment"));
			Player->AddInstanceComponent(Sync); Sync->RegisterComponent();
		}
	}

	if (!Controller.IsValid())
	{
		APlayerController* Local = World->GetFirstPlayerController();
		if (!Local || !Local->IsLocalPlayerController() || !World->GetGameViewport()) return;
		Controller = Local;
		ForgeInput = NewObject<UInputComponent>(Local);
		ForgeInput->RegisterComponent();
		FString SavedService;
		if (FFileHelper::LoadFileToString(SavedService, *(FPaths::ProjectDir() / TEXT(".local/forge-service.json"))))
		{
			TSharedPtr<FJsonObject> ServiceConfig;
			FString SavedUrl;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SavedService), ServiceConfig) && ServiceConfig
				&& ServiceConfig->TryGetStringField(TEXT("api_url"), SavedUrl) && IsSafeLocalService(SavedUrl)) ServiceUrl = SavedUrl;
		}
		FString LaunchService;
		if (FParse::Value(FCommandLine::Get(), TEXT("ForgeServiceUrl="), LaunchService) && IsSafeLocalService(LaunchService)) ServiceUrl = LaunchService;
		if (GetWorld()->GetGameInstance())
		{
			if (URoomCodeSubsystem* Room = GetWorld()->GetGameInstance()->GetSubsystem<URoomCodeSubsystem>())
				if (IsSafeLocalService(Room->GetForgeServiceUrl())) ServiceUrl = Room->GetForgeServiceUrl();
		}
		if (GEngine)
		{
			GEngine->bEnableOnScreenDebugMessages = false;
			GEngine->bEnableOnScreenDebugMessagesDisplay = false;
			GEngine->ClearOnScreenDebugMessages();
		}
		CreateForgeEntry();
		if (FParse::Param(FCommandLine::Get(), TEXT("WeaponTrialSelfTest")))
		{
			ForgeInput->BindKey(EKeys::One, IE_Pressed, this, &UWeaponForgeSubsystem::SelectTrialSword);
			ForgeInput->BindKey(EKeys::Two, IE_Pressed, this, &UWeaponForgeSubsystem::SelectTrialSpear);
			ForgeInput->BindKey(EKeys::Three, IE_Pressed, this, &UWeaponForgeSubsystem::SelectTrialLongsword);
			ForgeInput->BindKey(EKeys::Tab, IE_Pressed, this, &UWeaponForgeSubsystem::CycleTrial);
			ForgeInput->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &UWeaponForgeSubsystem::StartTrialAttack);
			ForgeInput->BindKey(EKeys::R, IE_Pressed, this, &UWeaponForgeSubsystem::ResetTrialTargets);
			ForgeInput->BindKey(EKeys::H, IE_Pressed, this, &UWeaponForgeSubsystem::ToggleTrialShield);
		}
		Local->PushInputComponent(ForgeInput);
		UE_LOG(LogTemp, Display, TEXT("Weapon Forge ready. Press F2 to open. Service=%s"), *ServiceUrl);
		if (FParse::Param(FCommandLine::Get(), TEXT("ForgeEquipSample"))) ImportSample();
		FString ExistingWeaponId;
		if (FParse::Value(FCommandLine::Get(), TEXT("ForgeWeaponId="), ExistingWeaponId))
		{
			FGuid ParsedWeaponId;
			if (FGuid::ParseExact(ExistingWeaponId, EGuidFormats::Digits, ParsedWeaponId))
			{
				TaskId = ParsedWeaponId.ToString(EGuidFormats::Digits).ToLower();
				ActiveServiceUrl = ServiceUrl;
				bPendingPreview = false;
				bReadyIsSample = false;
				UE_LOG(LogTemp, Display, TEXT("Weapon Forge loading completed weapon %s"), *TaskId);
				Request(EStep::Descriptor, TEXT("/api/v1/weapons/") + TaskId);
			}
			else Status = Label(TEXT("无效武器编号，请从最新武器入口启动。"));
		}

#if !UE_BUILD_SHIPPING
		// Opt-in real-service checks use the exact callbacks bound to the forge UI.
		// They have no effect on a normal play session and never select a mock.
		FString LiveResumeId;
		if (TaskId.IsEmpty() && FParse::Value(FCommandLine::Get(), TEXT("ForgeLiveResume="), LiveResumeId))
		{
			FGuid ParsedTaskId;
			if (FGuid::ParseExact(LiveResumeId, EGuidFormats::Digits, ParsedTaskId) && IsSafeLocalService(ServiceUrl))
			{
				TaskId = ParsedTaskId.ToString(EGuidFormats::Digits).ToLower();
				ActiveServiceUrl = ServiceUrl;
				bPendingPreview = false;
				bReadyIsSample = false;
				RetryStage = EStep::Resume;
				UE_LOG(LogTemp, Display, TEXT("Weapon Forge live check: continuing task %s"), *TaskId);
				Retry();
			}
		}
		else if (TaskId.IsEmpty() && FParse::Param(FCommandLine::Get(), TEXT("ForgeLiveSubmit")))
		{
			if (!Canvas) SAssignNew(Canvas, SForgeCanvas);
			Canvas->SeedTestSketch();
			Submit();
		}
		int32 CatalogPort = 0;
		if (FParse::Value(FCommandLine::Get(), TEXT("ForgeCatalogPort="), CatalogPort)
			&& CatalogPort >= 1 && CatalogPort <= 65535)
		{
			ServiceUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), CatalogPort);
			SAssignNew(Canvas, SForgeCanvas);
			Canvas->SeedTestSketch();
			CatalogTestIndex = 0;
		}
		int32 FixturePort = 0;
		if (FParse::Param(FCommandLine::Get(), TEXT("GLBSelfTest"))
			&& FParse::Value(FCommandLine::Get(), TEXT("ForgeFixturePort="), FixturePort)
			&& FixturePort >= 1 && FixturePort <= 65535)
		{
			ServiceUrl = FString::Printf(TEXT("http://127.0.0.1:%d"), FixturePort);
			SAssignNew(Canvas, SForgeCanvas);
			Canvas->SeedTestSketch(FParse::Param(FCommandLine::Get(), TEXT("ForgeColorSelfTest")));
			SubmitSketch(FParse::Param(FCommandLine::Get(), TEXT("ForgePreviewSelfTest")));
		}
#endif
	}
#if !UE_BUILD_SHIPPING
	if (CatalogTestIndex >= 0 && CatalogTestIndex < 10 && Stage == EStep::Idle && RetryStage == EStep::Idle
		&& Controller->GetPawn() && TaskId.IsEmpty())
	{
		CooldownUntil = 0;
		SubmitSketch(true);
	}
#endif

	if (Stage == EStep::Poll && !Pending && Now() >= NextPoll)
	{
		if (Now() >= Deadline)
		{
			if (bLocatingSubmission) Fail(EStep::Submit, Label(TEXT("暂未找到提交回执。草图和原编号已保留，点继续任务可用同一编号重发。")));
			else Fail(EStep::Poll, Label(TEXT("等待超时。可继续查询原任务。")));
		}
		else Request(EStep::Poll, TEXT("/api/v1/tasks/") + TaskId);
	}
	else if (bDeferredRetry && !Pending && Now() >= NextPoll)
	{
		bDeferredRetry = false;
		ContinueStep(Stage);
	}
	if (ForgePanelTestTime >= 0)
	{
		ForgePanelTestTime += DeltaTime;
		if (ForgePanelTestTime >= 1 && ForgePanelTestTime < 100)
		{
			const FString Directory = FPaths::ProjectSavedDir() / TEXT("WeaponCatalogChecks");
			IFileManager::Get().MakeDirectory(*Directory, true);
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("crafting-panel"), true, false);
			ForgePanelTestTime = 100;
		}
		else if (ForgePanelTestTime >= 100.5f) FPlatformMisc::RequestExit(false);
	}
	if (Stage == EStep::Equip) TryEquip();
	if (!EquippedBytes.IsEmpty() && Controller.IsValid())
	{
		if (ACharacter* Player = Cast<ACharacter>(Controller->GetPawn()))
			if (auto* Sync = Player->FindComponentByClass<UWeaponSyncComponent>())
				if (LastPublishedComponent.Get() != Sync)
				{ Sync->PublishEquipment(EquippedBytes, EquippedLengthCm, bEquippedReverseGrip, EquippedPreset, bEquippedIsSample, bEquippedIsSample ? FString() : EquippedModelUrl); LastPublishedComponent = Sync; }
	}

	UpdateTrial(DeltaTime);
	if (TrialPreset == INDEX_NONE && EquippedCharacter.IsValid() && GripState.GripVisual.IsValid()) PoseWeapon(EquippedCharacter.Get(), GripState.GripVisual.Get());
	if (!EquippedBytes.IsEmpty())
	{
		ACharacter* Character = Cast<ACharacter>(Controller->GetPawn());
		if (Character && Character != EquippedCharacter.Get())
		{
			EquippedCharacter = Character;
			if (!EquipGlb(Character, EquippedBytes, EquippedLengthCm, bEquippedReverseGrip, EquippedPreset, bEquippedIsSample))
				Status = Label(TEXT("角色重生后未能恢复武器。"));
			else if (TrialPreset != INDEX_NONE)
			{
				const FString Trait = TrialTrait;
				ActivateTrial(Character, TrialPreset, TrialReachCm, Trait);
			}
		}
	}
}

void UWeaponForgeSubsystem::Deinitialize()
{
	if (Pending)
	{
		Pending->OnProcessRequestComplete().Unbind();
		Pending->CancelRequest();
		Pending.Reset();
	}
	ClosePanel();
	ClearTrial();
	GripState.Reset();
	if (Controller.IsValid() && ForgeInput) Controller->PopInputComponent(ForgeInput);
	if (FSlateApplication::IsInitialized() && ForgeKeys)
		FSlateApplication::Get().UnregisterInputPreProcessor(ForgeKeys);
	ForgeKeys.Reset();
	if (ForgeEntry && GetWorld()->GetGameViewport())
		GetWorld()->GetGameViewport()->RemoveViewportWidgetContent(ForgeEntry.ToSharedRef());
	ForgeEntry.Reset();
	Canvas.Reset();
	Super::Deinitialize();
}

void UWeaponForgeSubsystem::Submit() { SubmitSketch(false); }
void UWeaponForgeSubsystem::SubmitPreview() { SubmitSketch(true); }

void UWeaponForgeSubsystem::SubmitSketch(bool bPreview)
{
	if (GetWorld()->GetGameInstance())
		if (URoomCodeSubsystem* Room = GetWorld()->GetGameInstance()->GetSubsystem<URoomCodeSubsystem>())
			if (IsSafeLocalService(Room->GetForgeServiceUrl())) ServiceUrl = Room->GetForgeServiceUrl();
	if (Stage != EStep::Idle) return;
	if (RetryStage != EStep::Idle) { Status = Label(TEXT("请先继续原任务，避免重复扣费。")); return; }
	if (Now() < CooldownUntil) { Status = Label(TEXT("锻造还在冷却。")); return; }
	if (!Canvas || !Canvas->HasDrawing()) { Status = Label(TEXT("先在画板上画出武器。")); return; }
	ActiveServiceUrl = ServiceUrl.TrimStartAndEnd();
	if (!IsSafeLocalService(ActiveServiceUrl)) { Status = Label(TEXT("请输入制作台的本机地址，例如 http://127.0.0.1:47833。")); return; }

	const TArray64<uint8> Png = Canvas->EncodePng();
	if (Png.IsEmpty()) { Status = Label(TEXT("草图编码失败。")); return; }
	TaskId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	const FString Boundary = TEXT("UEForge") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ContentType = TEXT("multipart/form-data; boundary=") + Boundary;
	SubmitBody.Reset();
	const auto Field = [&](const TCHAR* Key, const FString& Value)
	{
		AppendUtf8(SubmitBody, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"),
			*Boundary, Key, *Value));
	};
	const FString Title = Prompt.TrimStartAndEnd().Left(300);
	Field(TEXT("prompt"), Title.IsEmpty() ? TEXT("A weapon matching the player sketch") : Title);
	Field(TEXT("mode"), bPreview ? TEXT("mock") : TEXT("real"));
	Field(TEXT("consent"), TEXT("true"));
	Field(TEXT("classify_weapon"), TEXT("true"));
	Field(TEXT("request_id"), TaskId);
	AppendUtf8(SubmitBody, FString::Printf(TEXT("--%s\r\nContent-Disposition: form-data; name=\"sketch\"; filename=\"sketch.png\"\r\nContent-Type: image/png\r\n\r\n"), *Boundary));
	SubmitBody.Append(Png.GetData(), static_cast<int32>(Png.Num()));
	AppendUtf8(SubmitBody, TEXT("\r\n--") + Boundary + TEXT("--\r\n"));
	ReadyLengthCm = LengthCm;
	bReadyIsSample = false;
	bPendingPreview = bPreview;
	ReadyCombatPreset = INDEX_NONE;
	ReadySummary = FText::GetEmpty();
	bLocatingSubmission = false;
	SubmitReplays = 0;
	ConsecutiveConnectionFailures = 0;
	Request(EStep::Submit, TEXT("/api/v1/tasks"), TEXT("POST"));
}

void UWeaponForgeSubsystem::Retry()
{
	if (Stage != EStep::Idle) return;
	const EStep At = RetryStage;
	RetryStage = EStep::Idle;
	ConsecutiveConnectionFailures = 0;
	if (At == EStep::Submit) SubmitReplays = 0;
	if (At == EStep::Poll) Deadline = Now() + JobTimeout;
	ContinueStep(At);
}

void UWeaponForgeSubsystem::ContinueStep(EStep At)
{
	switch (At)
	{
	case EStep::Submit: Request(At, TEXT("/api/v1/tasks"), TEXT("POST")); break;
	case EStep::Resume: Request(At, TEXT("/api/v1/tasks/") + TaskId + TEXT("/resume"), TEXT("POST")); break;
	case EStep::Poll: Request(At, TEXT("/api/v1/tasks/") + TaskId); break;
	case EStep::Descriptor: Request(At, TEXT("/api/v1/weapons/") + TaskId); break;
	case EStep::Download: Request(At, ModelPath); break;
	case EStep::Equip: Stage = At; TryEquip(); break;
	default: Status = Label(TEXT("当前没有需要继续的任务。")); break;
	}
}

void UWeaponForgeSubsystem::Request(EStep Step, const FString& Path, const TCHAR* Verb)
{
	Stage = Step;
	Pending = FHttpModule::Get().CreateRequest();
	Pending->SetURL(ActiveServiceUrl + Path);
	Pending->SetVerb(Verb);
	Pending->SetTimeout(Step == EStep::Submit ? 60 : 30);
	UE_LOG(LogTemp, Display, TEXT("Weapon Forge request step=%d task=%s service=%s"), static_cast<int32>(Step), *TaskId, *ActiveServiceUrl);
	if (Step == EStep::Submit)
	{
		Pending->SetHeader(TEXT("Content-Type"), ContentType);
		Pending->SetContent(SubmitBody);
		Status = Label(TEXT("正在提交草图…"));
	}
	else if (Step == EStep::Resume)
	{
		Pending->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Pending->SetContentAsString(TEXT("{}"));
	}
	else if (Step == EStep::Download) Status = Label(TEXT("正在下载 GLB…"));
	Pending->OnProcessRequestComplete().BindWeakLambda(this,
		[this, Step](FHttpRequestPtr Completed, FHttpResponsePtr Response, bool bConnected)
		{
			if (Pending != Completed) return;
			Pending.Reset();
			Receive(Step, Response, bConnected);
		});
	if (!Pending->ProcessRequest())
	{
		Pending.Reset();
		Receive(Step, nullptr, false);
	}
}

void UWeaponForgeSubsystem::Fail(EStep RetryAt, const FText& Message)
{
	bLocatingSubmission = false;
	bDeferredRetry = false;
	Stage = EStep::Idle;
	RetryStage = RetryAt;
	Status = Message;
	UE_LOG(LogTemp, Warning, TEXT("Weapon Forge step %d failed: %s"), static_cast<int32>(RetryAt), *Message.ToString());
}

void UWeaponForgeSubsystem::RecoverSubmission()
{
	// The durable request_id makes resubmission idempotent. First look up the
	// original task: losing an HTTP acknowledgement does not mean it was rejected.
	if (!bLocatingSubmission)
	{
		bLocatingSubmission = true;
		Deadline = Now() + 90;
	}
	RetryStage = EStep::Idle;
	Stage = EStep::Poll;
	NextSubmitReplay = Now() + (SubmitReplays ? 10.0 : 5.0);
	NextPoll = Now() + .5;
	Status = Label(TEXT("提交回执暂未收到，正在找回原任务；无需重新画图或点击生成。"));
	UE_LOG(LogTemp, Warning, TEXT("Weapon Forge recovering original submission task=%s"), *TaskId);
}

bool UWeaponForgeSubsystem::RetryConnection(EStep Step)
{
	if (Step == EStep::Submit)
	{
		RecoverSubmission();
		return true;
	}
	if (Step == EStep::Poll)
	{
		if (Now() >= Deadline) return false;
	}
	else if (ConsecutiveConnectionFailures >= 3) return false;
	Stage = Step;
	bDeferredRetry = Step != EStep::Poll;
	const int32 Delay = 2 << FMath::Min(ConsecutiveConnectionFailures++, 2);
	NextPoll = Now() + Delay;
	Status = Label(TEXT("制作台暂时断线，正在自动重连原任务…"));
	return true;
}

void UWeaponForgeSubsystem::Receive(EStep Step, const FHttpResponsePtr& Response, bool bConnected)
{
	if (!bConnected || !Response)
	{
		if (RetryConnection(Step)) return;
		Fail(Step, FText::FromString(FString::Printf(TEXT("暂时无法连接制作台 %s。原任务编号 %s 已保留，可继续查询。"), *ActiveServiceUrl, *TaskId.Left(8))));
		return;
	}
	const int32 Code = Response->GetResponseCode();
	if (Step == EStep::Poll && bLocatingSubmission && Code == 404)
	{
		if (!SubmitBody.IsEmpty() && SubmitReplays < 2 && Now() >= NextSubmitReplay && Now() < Deadline)
		{
			++SubmitReplays;
			UE_LOG(LogTemp, Display, TEXT("Weapon Forge replaying original submission task=%s"), *TaskId);
			ContinueStep(EStep::Submit);
		}
		else NextPoll = Now() + 2;
		return;
	}
	if ((Code == 408 || Code == 429 || Code >= 500) && RetryConnection(Step)) return;
	if (Step == EStep::Resume && Code == 409)
	{
		Deadline = Now() + JobTimeout;
		Request(EStep::Poll, TEXT("/api/v1/tasks/") + TaskId);
		return;
	}
	if (Code < 200 || Code >= 300)
	{
		const bool bRejectedSubmit = Step == EStep::Submit && Code >= 400 && Code < 500;
		FString Detail;
		if (Code == 409)
		{
			TSharedPtr<FJsonObject> Error;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Error) && Error.IsValid())
			{
				Error->TryGetStringField(TEXT("detail"), Detail);
			}
		}
		Fail(bRejectedSubmit ? EStep::Idle : Step,
			FText::FromString(Detail.IsEmpty()
				? FString::Printf(TEXT("制作台返回 HTTP %d。请检查制作台任务或设置。"), Code)
				: Detail.Left(300)));
		return;
	}
	ConsecutiveConnectionFailures = 0;
	if (Step == EStep::Download)
	{
		if (!IsGlb(Response->GetContent())) { Fail(Step, Label(TEXT("GLB 无效或超过 32 MiB。"))); return; }
		ReadyBytes = Response->GetContent();
		Stage = EStep::Equip;
		TryEquip();
		return;
	}
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) || !Json)
	{
		Fail(Step, Label(TEXT("制作台返回了无法识别的任务数据。")));
		return;
	}
	if (Step == EStep::Descriptor)
	{
		const FString Expected = TEXT("/workflow-assets/") + TaskId + TEXT("/weapon.glb");
		bool bMock = true;
		if (!Json->TryGetStringField(TEXT("model_url"), ModelPath) || ModelPath != Expected
			|| !Json->TryGetBoolField(TEXT("is_mock"), bMock) || bMock != bPendingPreview)
		{
			Fail(Step, Label(TEXT("任务未返回可装备的真实 GLB。")));
			return;
		}
		const TSharedPtr<FJsonObject>* Combat = nullptr;
		FString Kind, TraitName, ClassificationSource;
		double LengthM = 0, ReachM = 0;
		if (!Json->TryGetObjectField(TEXT("combat"), Combat) || !Combat || !Combat->IsValid()
			|| !(*Combat)->TryGetStringField(TEXT("template_id"), Kind)
			|| !(*Combat)->TryGetStringField(TEXT("trait_id"), ReadyTrait)
			|| !(*Combat)->TryGetStringField(TEXT("trait_name"), TraitName)
			|| !(*Combat)->TryGetNumberField(TEXT("length_m"), LengthM)
			|| !(*Combat)->TryGetNumberField(TEXT("reach_m"), ReachM)
			|| !FMath::IsFinite(LengthM) || !FMath::IsFinite(ReachM))
		{
			Fail(Step, Label(TEXT("缺少武器模板数据，请确认制作台已更新。")));
			return;
		}
		ReadyCombatPreset = FindTrialPreset(Kind, ReadyTrait, LengthM, ReachM);
		if (ReadyCombatPreset == INDEX_NONE)
		{
			Fail(Step, Label(TEXT("武器数值超出预设范围。")));
			return;
		}
		ReadyLengthCm = LengthM * 100;
		ReadyReachCm = ReachM * 100;
		(*Combat)->TryGetStringField(TEXT("classification_source"), ClassificationSource);
		ReadySummary = FText::FromString(FString::Printf(TEXT("%s / %s · %s"),
			ClassificationSource == TEXT("outline") ? TEXT("本地轮廓判定") : TEXT("AI 识别"),
			bMock ? TEXT("模板预览") : TEXT("Lux3D 武器"), *TraitName));
		Request(EStep::Download, ModelPath);
		return;
	}
	if (Step == EStep::Submit || Step == EStep::Resume || Step == EStep::Poll)
	{
		FString ReturnedId;
		if (!Json->TryGetStringField(TEXT("id"), ReturnedId) || ReturnedId != TaskId)
		{
			Fail(Step, Label(TEXT("制作台任务编号不一致。请检查是否运行了更新后的服务。")));
			return;
		}
		if (Step == EStep::Submit || bLocatingSubmission)
		{
			CooldownUntil = Now() + (bPendingPreview ? 3.0 : ForgeCooldown);
		}
		if (Step != EStep::Poll || bLocatingSubmission) Deadline = Now() + JobTimeout;
		if (bLocatingSubmission) UE_LOG(LogTemp, Display, TEXT("Weapon Forge recovered original submission task=%s"), *TaskId);
		bLocatingSubmission = false;
	}
	FString TaskStatus, TaskStage;
	Json->TryGetStringField(TEXT("status"), TaskStatus);
	Json->TryGetStringField(TEXT("stage"), TaskStage);
	if (TaskStatus == TEXT("succeeded"))
	{
		FString WeaponId;
		if (!Json->TryGetStringField(TEXT("weapon_id"), WeaponId) || WeaponId != TaskId)
		{
			Fail(EStep::Poll, Label(TEXT("任务返回的武器编号不一致。")));
			return;
		}
		Request(EStep::Descriptor, TEXT("/api/v1/weapons/") + WeaponId);
	}
	else if (TaskStatus == TEXT("failed"))
	{
		FString Error;
		Json->TryGetStringField(TEXT("error"), Error);
		if (TaskStage == TEXT("classification"))
		{
			CooldownUntil = 0;
			Fail(EStep::Idle, FText::FromString(TEXT("识别未完成。") + Error.Left(220)));
		}
		else Fail(EStep::Resume, FText::FromString(TEXT("生成失败，可继续原任务。") + Error.Left(220)));
	}
	else if (TaskStatus == TEXT("queued") || TaskStatus == TEXT("running"))
	{
		if (TaskStatus == TEXT("queued"))
		{
			double Position=0;
			Json->TryGetNumberField(TEXT("queue_position"),Position);
			Status = FText::FromString(Position>0
				? FString::Printf(TEXT("武器已排队：第 %.0f 位（含正在制作的任务）· 无需重画，可关闭面板继续玩"),Position)
				: TEXT("武器已排队，等待前一把完成 · 无需重复提交，可关闭面板继续玩"));
			Stage = EStep::Poll;
			NextPoll = Now()+2;
			return;
		}
		FString Progress;
		Json->TryGetStringField(TEXT("retry_message"), Progress);
		const TSharedPtr<FJsonObject>* Remote = nullptr;
		if (Progress.IsEmpty() && Json->TryGetObjectField(TEXT("remote"), Remote) && Remote && Remote->IsValid())
			(*Remote)->TryGetStringField(TEXT("message"), Progress);
		Status = FText::FromString(Progress.IsEmpty()
			? TEXT("后台制作中 · ") + TaskStage + TEXT(" · 可以关闭面板继续玩")
			: Progress.Left(180) + TEXT(" · 可以关闭面板继续玩"));
		Stage = EStep::Poll;
		NextPoll = Now() + 2;
	}
	else Fail(EStep::Poll, Label(TEXT("制作台返回了未知的任务状态。")));
}

void UWeaponForgeSubsystem::ImportSample()
{
	if (Stage != EStep::Idle) return;
	if (RetryStage != EStep::Idle)
	{
		Status = Label(TEXT("请先继续原任务。"));
		return;
	}
	const FString Path = FPaths::ProjectContentDir() / TEXT("WeaponSamples/textured.glb");
	if (IFileManager::Get().FileSize(*Path) > MaxGlbBytes
		|| !FFileHelper::LoadFileToArray(ReadyBytes, *Path) || !IsGlb(ReadyBytes))
	{
		Status = Label(TEXT("找不到有效的工程样品 GLB。"));
		return;
	}
	ReadyLengthCm = LengthCm;
	bReadyIsSample = true;
	ReadyCombatPreset = 0;
	Stage = EStep::Equip;
	TryEquip();
}

void UWeaponForgeSubsystem::TryEquip()
{
	ACharacter* Character = Controller.IsValid() ? Cast<ACharacter>(Controller->GetPawn()) : nullptr;
	if (!Character) return;
	const FString PreviousModelUrl = EquippedModelUrl;
	EquippedModelUrl = bReadyIsSample ? FString() : ActiveServiceUrl + ModelPath;
	if (!EquipGlb(Character, ReadyBytes, ReadyLengthCm, bReverseGrip, ReadyCombatPreset, bReadyIsSample))
	{
		EquippedModelUrl = PreviousModelUrl;
		Fail(EStep::Equip, Label(TEXT("无法加载 GLB 或找不到右手挂点，保留旧武器。")));
		return;
	}
	ClearTrial();
	EquippedBytes = MoveTemp(ReadyBytes);
	EquippedLengthCm = ReadyLengthCm;
	EquippedPreset = ReadyCombatPreset;
	bEquippedReverseGrip = bReverseGrip;
	EquippedCharacter = Character;
	if (ReadyCombatPreset != INDEX_NONE && !bReadyIsSample)
		ActivateTrial(Character, ReadyCombatPreset, ReadyReachCm, ReadyTrait);
	SubmitBody.Reset();
	Stage = EStep::Idle;
	RetryStage = EStep::Idle;
	Status = bReadyIsSample ? Label(TEXT("样品已装备。")) : ReadySummary;
	UE_LOG(LogTemp, Display, TEXT("Weapon Forge result: %s"), *Status.ToString());
#if !UE_BUILD_SHIPPING
	if (CatalogTestIndex >= 0)
	{
		if (TrialPreset != CatalogTestIndex || !TrialVisual.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("ForgeCatalog FAIL index=%d received=%d"), CatalogTestIndex, TrialPreset);
			FPlatformMisc::RequestExit(false);
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("ForgeCatalog equipped %d"), CatalogTestIndex);
			TaskId.Reset();
			if (++CatalogTestIndex == 10)
			{
				UE_LOG(LogTemp, Display, TEXT("ForgeCatalog PASS"));
				FPlatformMisc::RequestExit(false);
			}
		}
	}
#endif
}

void UWeaponForgeSubsystem::FlipGrip()
{
	bReverseGrip = !bReverseGrip;
	if (Stage == EStep::Idle && EquippedCharacter.IsValid() && !EquippedBytes.IsEmpty()
		&& EquipGlb(EquippedCharacter.Get(), EquippedBytes, EquippedLengthCm, bReverseGrip, EquippedPreset, bEquippedIsSample))
	{
		bEquippedReverseGrip = bReverseGrip;
		if (TrialPreset != INDEX_NONE)
		{
			const FString Trait = TrialTrait;
			ActivateTrial(EquippedCharacter.Get(), TrialPreset, TrialReachCm, Trait);
		}
		Status = Label(TEXT("已翻转当前武器的握持方向。"));
	}
}

bool UWeaponForgeSubsystem::EquipGlb(ACharacter* Character, const TArray<uint8>& Bytes, float InLengthCm, bool bReverse, int32 Preset, bool bSample)
{
 if (!IsValid(Character) || Character->GetWorld() != GetWorld() || !GripState.Equip(Character, Bytes, InLengthCm, bReverse, Preset)) return false;
 bEquippedIsSample = bSample;
 if (Character->IsLocallyControlled())
 {
  if (auto* Sync = Character->FindComponentByClass<UWeaponSyncComponent>())
   { Sync->PublishEquipment(Bytes, InLengthCm, bReverse, Preset, bSample, bSample ? FString() : EquippedModelUrl); LastPublishedComponent = Sync; }
 }
 UE_LOG(LogTemp, Display, TEXT("Weapon Forge: local GLB attached, length %.1f cm"), InLengthCm);
 return true;
}

bool UWeaponForgeSubsystem::EquipInitialGun()
{
 if (!Controller.IsValid() || !Controller->GetPawn() || Panel || Stage != EStep::Idle) return false;
 SelectTrial(9);
 if (EquippedPreset != 9 || !GripState.GripVisual.IsValid()) return false;
 return true;
}

#include "WeaponForgeUI.inl"
