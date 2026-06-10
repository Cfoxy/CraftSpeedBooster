// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

class UFGWorkBench;

// Per-workbench craft streak state
struct FAccelState
{
	float  ContinuousCraftTime = 0.0f;
	double LastCallTime        = 0.0;
	float  LastMultiplier      = 1.0f;
};

// Configurable tier entry — loaded from .ini
struct FTierConfig
{
	float Time; // seconds of continuous crafting required
	float Mult; // speed multiplier applied
};

// Full mod configuration
struct FModConfig
{
	TArray<FTierConfig> Tiers;
	float InactivityDelay = 1.5f; // seconds of inactivity before streak reset
	float WidgetScale     = 1.0f; // HUD widget scale factor (0.5 .. 2.0)
};

class FManualCraftingAcceleratorLightModule : public IModuleInterface
{
public:
	virtual void StartupModule()  override;
	virtual void ShutdownModule() override;

	static const FModConfig& GetConfig() { return Config; }

private:
	TMap<UFGWorkBench*, FAccelState> WorkbenchStates;

	static FModConfig Config;

	// Config — loaded from .ini on startup
	static void LoadConfig();
	static void WriteDefaultConfig(const FString& IniPath);

	// Tier helpers
	static float ComputeMultiplier(float T);
	static float CurrentTierTime(float T);
	static float NextTierTime(float T);

	// HUD
	static void CreateHUDWidget();
	static void UpdateHUD(float ContinuousTime, float Multiplier);
	static void HideHUD();
};
