#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "TrioCombatAuthoring.generated.h"

UCLASS()
class UTrioCombatAuthoring : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="Trio Combat")
    static bool InstallCombatAssets();

    UFUNCTION(BlueprintCallable, Category="Trio Combat")
    static bool VerifyCombatData();

    UFUNCTION(BlueprintCallable, Category="Trio Combat")
    static bool InstallBotAttackHooks();

    UFUNCTION(BlueprintCallable, Category="Trio Combat")
    static bool RemoveCharacterCrouch();

    UFUNCTION(BlueprintCallable, Category="Trio Combat")
    static bool BuildDeathPhysics();
};
