// Included after SForgeCanvas, so all UI and its input routing share one canvas type.
#include "Framework/Application/IInputProcessor.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "WeaponSyncComponent.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Engine/UserInterfaceSettings.h"

static float ForgePixelScale(UWorld* World)
{
	FVector2D Size(960, 540);
	World->GetGameViewport()->GetViewportSize(Size);
	return 1.f / FMath::Max(.25f, GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(FIntPoint(Size.X, Size.Y)));
}

class FForgeKeyRouter final : public IInputProcessor
{
public:
	TFunction<bool(const FKeyEvent&)> Handler;
	virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
	{
		return Handler && Handler(Event);
	}
};

bool UWeaponForgeSubsystem::RouteForgeKey(const FKeyEvent& Event)
{
	if (!Controller.IsValid() || !GetWorld() || !GetWorld()->GetGameViewport()) return false;
	const TSharedPtr<SViewport> Widget = GetWorld()->GetGameViewport()->GetGameViewportWidget();
	if (!Widget || FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) != FSlateApplication::Get().GetActiveTopLevelWindow()) return false;
	const FKey Key = Event.GetKey();
	if (Key == EKeys::F2 || (Key == EKeys::B && !Panel))
	{
		if (!Event.IsRepeat()) TogglePanel();
		return true;
	}
	if (Panel && Key == EKeys::Escape)
	{
		ClosePanel();
		return true;
	}
	if (!Panel && !Event.IsRepeat())
	{
		if (Key == EKeys::G) { FlipGrip(); return true; }
		if (Key == EKeys::Y) { RotateGrip(); return true; }
		if (Key == EKeys::U) { AdjustGripBack(); return true; }
		if (Key == EKeys::I) { AdjustGripForward(); return true; }
	}
	return false;
}

void UWeaponForgeSubsystem::CreateForgeEntry()
{
	if (!FSlateApplication::IsInitialized()) return;
	auto Router = MakeShared<FForgeKeyRouter>();
	Router->Handler = [Weak = TWeakObjectPtr<UWeaponForgeSubsystem>(this)](const FKeyEvent& Event)
	{
		return Weak.IsValid() && Weak->RouteForgeKey(Event);
	};
	ForgeKeys = Router;
	FSlateApplication::Get().RegisterInputPreProcessor(ForgeKeys, 0);
	// Hide legacy grey top-right ForgeEntry button; keep F2 / B hotkey untouched for in-match Forge workbench.
	if (FParse::Param(FCommandLine::Get(), TEXT("ForgeOpen"))) TogglePanel();
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("ForgeInputSelfTest")))
	{
		ClosePanel();
		auto Press = [](FKey Key) {
			FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0));
		};
		const bool OriginalMove = Controller->IsMoveInputIgnored();
		const bool OriginalLook = Controller->IsLookInputIgnored();
		Press(EKeys::F2);
		const bool Opened = Panel.IsValid() && Controller->bShowMouseCursor && Controller->IsMoveInputIgnored();
		Press(EKeys::F2);
		const bool Closed = !Panel && OriginalMove == Controller->IsMoveInputIgnored() && OriginalLook == Controller->IsLookInputIgnored();
		Press(EKeys::B);
		const bool Backup = Panel.IsValid();
		Press(EKeys::Escape);
		UE_LOG(LogTemp, Display, TEXT("ForgeInput %s F2open=%d F2close=%d backup=%d escape=%d"),
			Opened && Closed && Backup && !Panel ? TEXT("PASS") : TEXT("FAIL"), Opened, Closed, Backup, !Panel);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("ForgePanelSelfTest")))
	{
		if (!Panel) TogglePanel();
		Canvas->SeedTestSketch(FParse::Param(FCommandLine::Get(), TEXT("ForgeColorSelfTest")));
		ForgePanelTestTime = 0;
	}
#endif
}

void UWeaponForgeSubsystem::ClosePanel()
{
	if (!Panel) return;
	if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport())
		Viewport->RemoveViewportWidgetContent(Panel.ToSharedRef());
	Panel.Reset();
	if (Controller.IsValid())
	{
		if (bOriginalCursor) Controller->SetInputMode(FInputModeGameAndUI());
		else Controller->SetInputMode(FInputModeGameOnly());
		Controller->bShowMouseCursor = bOriginalCursor;
		if (!bRestoredMoveInput) Controller->SetIgnoreMoveInput(false);
		if (!bRestoredLookInput) Controller->SetIgnoreLookInput(false);
		if (FSlateApplication::IsInitialized()) FSlateApplication::Get().SetAllUserFocusToGameViewport();
	}
}

void UWeaponForgeSubsystem::TogglePanel()
{
	if (Panel) { ClosePanel(); return; }
	if (!Canvas) SAssignNew(Canvas, SForgeCanvas);
	if (Status.IsEmpty()) Status = Label(TEXT("画出任意物体，AI 会选择最接近的攻击模板。预览使用样品模型；Lux3D 生成保留你的绘画外观。"));
	const auto Action = [this](const TCHAR* Title, void (UWeaponForgeSubsystem::*Method)())
	{
		return SNew(SButton).Text(Label(Title)).OnClicked_Lambda([this, Method]()
		{ (this->*Method)(); return FReply::Handled(); });
	};
	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");
	const TSharedRef<SHorizontalBox> Palette = SNew(SHorizontalBox);
	Palette->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 5, 0)
	[SNew(STextBlock).Text(Label(TEXT("画笔")))];
	for (const FForgeSwatch& Swatch : ForgeSwatches)
	{
		const FColor Color = Swatch.Color;
		Palette->AddSlot().AutoWidth().Padding(1, 0)
		[SNew(SBox).WidthOverride(28).HeightOverride(26)
			[SNew(SButton).ButtonStyle(FCoreStyle::Get(), "NoBorder").ContentPadding(FMargin(1.f))
				.HAlign(HAlign_Center).VAlign(VAlign_Center).Cursor(EMouseCursor::Hand).ToolTipText(Label(Swatch.Name))
				.OnClicked_Lambda([this, Color]() { Canvas->SetBrushColor(Color); return FReply::Handled(); })
				[SNew(SBorder).Padding(2).BorderImage(WhiteBrush)
					.BorderBackgroundColor_Lambda([this, Color]() -> FSlateColor
					{
						return Canvas->GetBrushColor() == Color ? FLinearColor::White : FLinearColor(0.07f, 0.07f, 0.07f);
					})
					[SNew(SBox).WidthOverride(18).HeightOverride(18)
						[SNew(SBorder).BorderImage(WhiteBrush).BorderBackgroundColor(FLinearColor::FromSRGBColor(Color))]]]]];
	}
	FVector2D Size(960, 540);
	GetWorld()->GetGameViewport()->GetViewportSize(Size);
	const float PanelHeight = FMath::Max(200.f, float(Size.Y) - 28.f);
	Panel = SNew(SDPIScaler).DPIScale_Lambda([this]() { return ForgePixelScale(GetWorld()); })
	[SNew(SOverlay)
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[SNew(SBorder).Padding(10)
			[SNew(SBox).WidthOverride(FMath::Min(500.f, float(Size.X) - 36.f)).MaxDesiredHeight(PanelHeight)
				[SNew(SScrollBox)
					+ SScrollBox::Slot()
					[SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(Label(TEXT("画个锤子 | 武器制作台")))]
							+ SHorizontalBox::Slot().AutoWidth()[Action(TEXT("关闭 [F2 / Esc]"), &UWeaponForgeSubsystem::ClosePanel)]]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5)
						[SNew(STextBlock).AutoWrapText(true).Text(Label(TEXT("任意草图 → 最接近的模板 → 模型、特性与攻击方式")))]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
						[SNew(SBox).WidthOverride(260).HeightOverride(260)
							[SNew(SBorder).Padding(0).BorderImage(WhiteBrush).Clipping(EWidgetClipping::ClipToBounds)[Canvas.ToSharedRef()]]]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 4, 0, 0)
						[Palette]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5)
						[SNew(SEditableTextBox).Text(FText::FromString(Prompt)).HintText(Label(TEXT("补充描述（可选）")))
							.OnTextChanged_Lambda([this](const FText& T) { Prompt = T.ToString(); })]
						+ SVerticalBox::Slot().AutoHeight()
						[SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("AI 判定 · 模板预览"), &UWeaponForgeSubsystem::SubmitPreview)]
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("AI 判定 · Lux3D 生成"), &UWeaponForgeSubsystem::Submit)]]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5)
						[SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Text(Label(TEXT("撤销一笔")))
								.IsEnabled_Lambda([this]() { return Canvas->HasDrawing(); })
								.OnClicked_Lambda([this]() { Canvas->Undo(); return FReply::Handled(); })]
							+ SHorizontalBox::Slot().FillWidth(1)[SNew(SButton).Text(Label(TEXT("清空画板")))
								.OnClicked_Lambda([this]() { Canvas->Clear(); return FReply::Handled(); })]
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("继续原任务"), &UWeaponForgeSubsystem::Retry)]]
						+ SVerticalBox::Slot().AutoHeight()
						[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]() {
						FString Message = Status.ToString();
						if (Controller.IsValid() && Controller->GetPawn()) if (auto* Sync = Controller->GetPawn()->FindComponentByClass<UWeaponSyncComponent>()) if (!Sync->GetStatus().IsEmpty()) Message += TEXT("\n") + Sync->GetStatus();
						return FText::FromString(Message); })]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5)
						[SNew(STextBlock).AutoWrapText(true).Text(Label(TEXT("G 反握 / Y 翻面 / U、I 微调握点。Tab 轮换四种动作，1 短剑、3 单手剑。左键攻击，H 放盾，R 重摆。")))]
						+ SVerticalBox::Slot().AutoHeight()
						[SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("反握"), &UWeaponForgeSubsystem::FlipGrip)]
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("翻面 90°"), &UWeaponForgeSubsystem::RotateGrip)]
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("握点 −"), &UWeaponForgeSubsystem::AdjustGripBack)]
							+ SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("握点 +"), &UWeaponForgeSubsystem::AdjustGripForward)]]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 5)
						[SNew(SEditableTextBox).Text(FText::FromString(ServiceUrl))
							.Visibility(FParse::Param(FCommandLine::Get(), TEXT("AutoLAN")) ? EVisibility::Collapsed : EVisibility::Visible)
							.OnTextChanged_Lambda([this](const FText& T) { ServiceUrl = T.ToString(); })]
					]
				]
			]
		]];
	GetWorld()->GetGameViewport()->AddViewportWidgetContent(Panel.ToSharedRef(), 50);
	bOriginalCursor = Controller->bShowMouseCursor;
	bRestoredMoveInput = Controller->IsMoveInputIgnored();
	bRestoredLookInput = Controller->IsLookInputIgnored();
	FInputModeGameAndUI Mode;
	Mode.SetWidgetToFocus(Panel);
	Mode.SetHideCursorDuringCapture(false);
	Controller->SetInputMode(Mode);
	Controller->bShowMouseCursor = true;
	if (!bRestoredMoveInput) Controller->SetIgnoreMoveInput(true);
	if (!bRestoredLookInput) Controller->SetIgnoreLookInput(true);
}
