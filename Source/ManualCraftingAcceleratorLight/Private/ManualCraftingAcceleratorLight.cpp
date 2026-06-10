// Copyright Epic Games, Inc. All Rights Reserved.
#include "ManualCraftingAcceleratorLight.h"
#include "Patching/NativeHookManager.h"
#include "FGWorkBench.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Styling/CoreStyle.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "FManualCraftingAcceleratorLightModule"

// ---------------------------------------------------------------------------
// Static config instance
// ---------------------------------------------------------------------------
FModConfig FManualCraftingAcceleratorLightModule::Config;

// ---------------------------------------------------------------------------
// HUD state — read by Slate lambdas every frame
// ---------------------------------------------------------------------------
static float        GCurrentMult  = 1.f;
static float        GProgress     = 0.f;
static FText        GTextMain;
static FText        GTextBar;
static FText        GTextNext;
static FText        GTextETA;
static FLinearColor GBarColor     = FLinearColor::White;
static FLinearColor GAccentColor  = FLinearColor::White;
static bool         GHUDVisible   = false;
static const int32  GBarBlocks    = 16;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static TMap<UFGWorkBench*, FAccelState>* GWorkbenchStates = nullptr;
static FTSTicker::FDelegateHandle        GTickerHandle;
static TSharedPtr<SWidget>               GHUDRoot          = nullptr;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static FString GetIniPath()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() /
		TEXT("Mods/GameFeatures/ManualCraftingAcceleratorLight/Config/ManualCraftingAcceleratorLight.ini")
	);
}

/* static */ void FManualCraftingAcceleratorLightModule::WriteDefaultConfig(const FString& IniPath)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(IniPath), true);

	const FString Content =
		TEXT("; ManualCraftingAcceleratorLight - Configuration\n")
		TEXT("; Edit this file and restart the game to apply changes.\n")
		TEXT(";\n")
		TEXT("; InactivityDelay : seconds of inactivity before streak reset (default: 1.5)\n")
		TEXT("; WidgetScale     : HUD widget size, from 0.5 (small) to 2.0 (large) (default: 1.0)\n")
		TEXT(";\n")
		TEXT("; Tiers: Time = seconds of continuous crafting, Mult = speed multiplier\n")
		TEXT("; You can add, remove, or edit tiers freely.\n")
		TEXT("; The first tier (Time=0) is mandatory and represents the base state.\n")
		TEXT(";\n")
		TEXT("[Settings]\n")
		TEXT("InactivityDelay=1.5\n")
		TEXT("WidgetScale=1.0\n")
		TEXT("\n")
		TEXT("[Tiers]\n")
		TEXT("; Format: Tier_N=Time,Multiplier\n")
		TEXT("Tier_0=0,1\n")
		TEXT("Tier_1=10,2\n")
		TEXT("Tier_2=25,4\n")
		TEXT("Tier_3=45,8\n")
		TEXT("Tier_4=75,12\n")
		TEXT("Tier_5=120,16\n")
		TEXT("Tier_6=180,20\n")
		TEXT("Tier_7=300,25\n");

	FFileHelper::SaveStringToFile(Content, *IniPath);
	UE_LOG(LogTemp, Warning, TEXT("[MCA] Default config written to: %s"), *IniPath);
}

/* static */ void FManualCraftingAcceleratorLightModule::LoadConfig()
{
	const FString IniPath = GetIniPath();
	if (!FPaths::FileExists(IniPath)) WriteDefaultConfig(IniPath);

	GConfig->LoadFile(IniPath);

	float InactivityDelay = 1.5f, WidgetScale = 1.0f;
	GConfig->GetFloat(TEXT("Settings"), TEXT("InactivityDelay"), InactivityDelay, IniPath);
	GConfig->GetFloat(TEXT("Settings"), TEXT("WidgetScale"),     WidgetScale,     IniPath);
	Config.InactivityDelay = FMath::Clamp(InactivityDelay, 0.5f, 30.f);
	Config.WidgetScale     = FMath::Clamp(WidgetScale,     0.5f,  2.f);

	Config.Tiers.Empty();
	for (int32 i = 0; i < 32; ++i)
	{
		FString Value;
		if (!GConfig->GetString(TEXT("Tiers"),
			*FString::Printf(TEXT("Tier_%d"), i), Value, IniPath)) break;
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() < 2) continue;
		FTierConfig T;
		T.Time = FCString::Atof(*Parts[0].TrimStartAndEnd());
		T.Mult = FMath::Max(1.f, FCString::Atof(*Parts[1].TrimStartAndEnd()));
		Config.Tiers.Add(T);
	}

	if (Config.Tiers.Num() == 0)
	{
		Config.Tiers = {
			{  0.f,1.f},{10.f,2.f},{25.f,4.f},{45.f,8.f},
			{75.f,12.f},{120.f,16.f},{180.f,20.f},{300.f,25.f}
		};
	}

	Config.Tiers.Sort([](const FTierConfig& A, const FTierConfig& B)
	{
		return A.Time < B.Time;
	});

	UE_LOG(LogTemp, Warning,
		TEXT("[MCA] Config loaded: %d tiers, InactivityDelay=%.1fs, WidgetScale=%.1f"),
		Config.Tiers.Num(), Config.InactivityDelay, Config.WidgetScale);
}

// ---------------------------------------------------------------------------
// Tier helpers
// ---------------------------------------------------------------------------
/* static */ float FManualCraftingAcceleratorLightModule::ComputeMultiplier(float T)
{
	const auto& Tiers = Config.Tiers;
	for (int32 i = Tiers.Num()-1; i >= 0; --i)
		if (T >= Tiers[i].Time) return Tiers[i].Mult;
	return 1.f;
}

/* static */ float FManualCraftingAcceleratorLightModule::CurrentTierTime(float T)
{
	const auto& Tiers = Config.Tiers;
	for (int32 i = Tiers.Num()-1; i >= 0; --i)
		if (T >= Tiers[i].Time) return Tiers[i].Time;
	return 0.f;
}

/* static */ float FManualCraftingAcceleratorLightModule::NextTierTime(float T)
{
	for (const FTierConfig& Tier : Config.Tiers)
		if (T < Tier.Time) return Tier.Time;
	return Config.Tiers.Last().Time;
}

static FLinearColor ColorForMult(float M)
{
	if      (M >= 25.f) return FLinearColor(1.f,  0.40f, 0.f );
	else if (M >= 16.f) return FLinearColor(1.f,  0.55f, 0.f );
	else if (M >=  8.f) return FLinearColor(1.f,  0.72f, 0.1f);
	else if (M >=  4.f) return FLinearColor(1.f,  0.90f, 0.3f);
	else if (M >=  2.f) return FLinearColor(0.45f,1.f,   0.3f);
	return FLinearColor(0.8f, 0.8f, 0.8f, 1.f);
}

static FText BuildBarText()
{
	const int32 Filled = FMath::RoundToInt(GProgress * GBarBlocks);
	FString Bar;
	Bar.Reserve(GBarBlocks + 2);
	Bar += TEXT("[");
	for (int32 i = 0; i < GBarBlocks; ++i)
		Bar += (i < Filled) ? TEXT("|") : TEXT(".");
	Bar += TEXT("]");
	return FText::FromString(Bar);
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
/* static */ void FManualCraftingAcceleratorLightModule::CreateHUDWidget()
{
	if (!GEngine || !GEngine->GameViewport) return;
	if (GHUDRoot.IsValid()) return;

	const float S      = Config.WidgetScale;
	const int32 FontLg = FMath::RoundToInt(20.f * S);
	const int32 FontMd = FMath::RoundToInt(16.f * S);
	const int32 FontSm = FMath::RoundToInt(13.f * S);

	TSharedRef<SBox> Root = SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.f, 0.f, 0.f, 120.f))
		.Visibility_Lambda([]() -> EVisibility
		{
			return GHUDVisible ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
		})
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderBackgroundColor(FLinearColor(0.04f, 0.04f, 0.04f, 0.92f))
				.Padding(FMargin(14.f*S, 10.f*S, 16.f*S, 10.f*S))
				.Visibility(EVisibility::HitTestInvisible)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					.Padding(0.f, 0.f, 0.f, 6.f*S)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", FontLg))
						.Text_Lambda([]() { return GTextMain; })
						.ColorAndOpacity_Lambda([]() -> FSlateColor
						{
							return FSlateColor(ColorForMult(GCurrentMult));
						})
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					.Padding(0.f, 0.f, 0.f, 6.f*S)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", FontMd))
						.Text_Lambda([]() { return GTextBar; })
						.ColorAndOpacity_Lambda([]() -> FSlateColor
						{
							return FSlateColor(GBarColor);
						})
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						.Padding(0.f, 0.f, 10.f*S, 0.f)
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", FontSm))
							.Text_Lambda([]() { return GTextNext; })
							.ColorAndOpacity(FLinearColor(0.7f, 0.7f, 0.7f, 1.f))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", FontSm))
							.Text_Lambda([]() { return GTextETA; })
							.ColorAndOpacity_Lambda([]() -> FSlateColor
							{
								return FSlateColor(ColorForMult(GCurrentMult));
							})
						]
					]
				]
			]
			// Colored left accent border
			+ SOverlay::Slot().HAlign(HAlign_Left)
			[
				SNew(SBox).WidthOverride(FMath::Max(2.f, 4.f*S))
				[
					SNew(SBorder)
					.BorderBackgroundColor_Lambda([]() -> FSlateColor
					{
						return FSlateColor(GAccentColor);
					})
					.Padding(0)
				]
			]
		];

	GHUDRoot = Root;
	GEngine->GameViewport->AddViewportWidgetContent(Root, 10);
}

/* static */ void FManualCraftingAcceleratorLightModule::UpdateHUD(
	float ContinuousTime, float Multiplier)
{
	if (!IsInGameThread()) return;
	CreateHUDWidget();

	GCurrentMult = Multiplier;
	GBarColor    = ColorForMult(Multiplier);
	GAccentColor = ColorForMult(Multiplier);
	GHUDVisible  = true;

	GTextMain = FText::FromString(
		FString::Printf(TEXT("Craft Speed  x%.0f"), Multiplier));

	const auto& Tiers = Config.Tiers;
	const bool  bMax  = Tiers.Num() > 0 && Multiplier >= Tiers.Last().Mult;

	if (bMax)
	{
		GProgress = 1.f;
		GBarColor = GAccentColor = FLinearColor(1.f, 0.35f, 0.f, 1.f);
		GTextBar  = BuildBarText();
		GTextNext = FText::FromString(TEXT("MAX"));
		GTextETA  = FText::GetEmpty();
	}
	else
	{
		const float CurTime  = CurrentTierTime(ContinuousTime);
		const float NextTime = NextTierTime(ContinuousTime);
		const float Range    = NextTime - CurTime;
		GProgress = Range > 0.f
			? FMath::Clamp((ContinuousTime - CurTime) / Range, 0.f, 1.f)
			: 0.f;
		GTextBar = BuildBarText();

		float NextMult = Tiers.Last().Mult;
		for (const FTierConfig& T : Tiers)
			if (ContinuousTime < T.Time) { NextMult = T.Mult; break; }

		GTextNext = FText::FromString(FString::Printf(TEXT("-> x%.0f"), NextMult));
		GTextETA  = FText::FromString(
			FString::Printf(TEXT("%ds"), FMath::CeilToInt(NextTime - ContinuousTime)));
	}
}

/* static */ void FManualCraftingAcceleratorLightModule::HideHUD()
{
	GHUDVisible  = false;
	GProgress    = 0.f;
	GCurrentMult = 1.f;
}

// ---------------------------------------------------------------------------
// StartupModule
// ---------------------------------------------------------------------------
void FManualCraftingAcceleratorLightModule::StartupModule()
{
	LoadConfig();
	UE_LOG(LogTemp, Warning,
		TEXT("[ManualCraftingAcceleratorLight] StartupModule OK - v1.9"));

#if !WITH_EDITOR

	GWorkbenchStates = &WorkbenchStates;

	GTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (!GWorkbenchStates) return true;
			const double Now       = FPlatformTime::Seconds();
			const float InactDelay = Config.InactivityDelay;
			bool AnyActive         = false;

			for (auto& Pair : *GWorkbenchStates)
			{
				FAccelState& State = Pair.Value;
				if (State.LastCallTime <= 0.0) continue;

				if ((Now - State.LastCallTime) > InactDelay)
				{
					if (State.ContinuousCraftTime > 0.f)
						UE_LOG(LogTemp, Warning,
							TEXT("[MCA] Streak reset bench=%p (inactive %.1fs)"),
							Pair.Key,
							static_cast<float>(Now - State.LastCallTime));
					State.ContinuousCraftTime = 0.f;
					State.LastMultiplier      = 1.f;
					State.LastCallTime        = 0.0;
				}
				else AnyActive = true;
			}

			if (!AnyActive) HideHUD();
			return true;
		}),
		0.5f
	);

	SUBSCRIBE_METHOD(UFGWorkBench::Produce, [](auto& scope, UFGWorkBench* self, float dt)
	{
		if (!self || !GWorkbenchStates) { scope(self, dt); return; }

		FAccelState& State        = GWorkbenchStates->FindOrAdd(self);
		State.LastCallTime        = FPlatformTime::Seconds();
		State.ContinuousCraftTime = FMath::Min(
			State.ContinuousCraftTime + dt,
			Config.Tiers.Num() > 0 ? Config.Tiers.Last().Time : 300.f
		);

		const float Multiplier = ComputeMultiplier(State.ContinuousCraftTime);
		UpdateHUD(State.ContinuousCraftTime, Multiplier);

		if (Multiplier != State.LastMultiplier)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[MCA] x%.0f at t=%.1fs bench=%p"),
				Multiplier, State.ContinuousCraftTime, self);
			State.LastMultiplier = Multiplier;
		}

		scope(self, dt * Multiplier);
	});

#endif // !WITH_EDITOR
}

// ---------------------------------------------------------------------------
// ShutdownModule
// ---------------------------------------------------------------------------
void FManualCraftingAcceleratorLightModule::ShutdownModule()
{
	FTSTicker::GetCoreTicker().RemoveTicker(GTickerHandle);

	if (GEngine && GEngine->GameViewport && GHUDRoot.IsValid())
		GEngine->GameViewport->RemoveViewportWidgetContent(GHUDRoot.ToSharedRef());

	GHUDRoot.Reset();
	GWorkbenchStates = nullptr;
	WorkbenchStates.Empty();

	UE_LOG(LogTemp, Warning,
		TEXT("[ManualCraftingAcceleratorLight] ShutdownModule OK"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FManualCraftingAcceleratorLightModule, ManualCraftingAcceleratorLight)
