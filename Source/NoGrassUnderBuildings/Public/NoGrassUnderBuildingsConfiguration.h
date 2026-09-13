#pragma once

#include "Configuration/ModConfiguration.h"
#include "Configuration/Properties/WidgetExtension/CP_Bool.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NoGrassUnderBuildingsConfiguration.generated.h"

UCLASS()
class NOGRASSUNDERBUILDINGS_API UNoGrassUnderBuildingsConfiguration : public UModConfiguration
{
	GENERATED_BODY()

public:
	UNoGrassUnderBuildingsConfiguration();
};

UCLASS()
class NOGRASSUNDERBUILDINGS_API UNoGrassUnderBuildingsConfigurationRegistrar : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void StartPersistenceTimer(class FTimerManager* TimerManager);
	void PollForConfigurationChanges();

	FTimerHandle PersistenceTimer;
	int8 LastObservedVehiclePaths = -1;
};

USTRUCT(BlueprintType)
struct NOGRASSUNDERBUILDINGS_API FNoGrassUnderBuildingsConfigurationStruct
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	bool EnableVehiclePaths = true;

	static bool ShouldEnableVehiclePaths(const UObject* WorldContext);
};
