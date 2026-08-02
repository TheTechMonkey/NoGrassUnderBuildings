#include "NoGrassConfiguration.h"

#include "Configuration/ConfigManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

FNoGrassConfigurationStruct FNoGrassConfigurationStruct::GetActiveConfig(
	const UObject* WorldContext)
{
	FNoGrassConfigurationStruct Config;
	if (!GEngine || !WorldContext)
	{
		return Config;
	}

	const UWorld* World = GEngine->GetWorldFromContextObject(
		WorldContext,
		EGetWorldErrorMode::ReturnNull);
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UConfigManager* ConfigManager =
		GameInstance ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
	if (ConfigManager)
	{
		static const FConfigId ConfigId{
			TEXT("NoGrassUnderBuildings"),
			TEXT("")};
		ConfigManager->FillConfigurationStruct(
			ConfigId,
			FDynamicStructInfo{
				FNoGrassConfigurationStruct::StaticStruct(),
				&Config});
	}
	return Config;
}
