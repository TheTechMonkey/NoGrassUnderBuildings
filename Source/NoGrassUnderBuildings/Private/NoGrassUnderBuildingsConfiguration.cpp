#include "NoGrassUnderBuildingsConfiguration.h"

#include "Configuration/ConfigManager.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Configuration/Properties/WidgetExtension/CP_Section.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Util/EngineUtil.h"

#define LOCTEXT_NAMESPACE "NoGrassUnderBuildings"

UNoGrassUnderBuildingsConfiguration::UNoGrassUnderBuildingsConfiguration()
{
	ConfigId = {TEXT("NoGrassUnderBuildings"), TEXT("")};
	DisplayName = LOCTEXT("ConfigName", "No Grass Under Buildings");
	Description = LOCTEXT("ConfigDescription", "Controls which structures and routes hide foliage.");

	static ConstructorHelpers::FClassFinder<UConfigPropertySection> SectionPropertyClass(
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertySection"));
	check(SectionPropertyClass.Succeeded());
	RootSection = CastChecked<UConfigPropertySection>(CreateDefaultSubobject(
		TEXT("RootSection"),
		UConfigPropertySection::StaticClass(),
		SectionPropertyClass.Class,
		true,
		false));
	if (UCP_Section* Section = Cast<UCP_Section>(RootSection))
	{
		Section->WidgetType = ECP_SectionWidgetType::CPS_Vertical;
		Section->HasHeader = false;
	}

	static ConstructorHelpers::FClassFinder<UCP_Bool> BoolPropertyClass(
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyBool"));
	check(BoolPropertyClass.Succeeded());
	UCP_Bool* VehiclePaths = CastChecked<UCP_Bool>(CreateDefaultSubobject(
		TEXT("EnableVehiclePaths"),
		UCP_Bool::StaticClass(),
		BoolPropertyClass.Class,
		true,
		false));
	VehiclePaths->DisplayName = LOCTEXT("VehiclePathsName", "Vehicle paths");
	VehiclePaths->Tooltip = LOCTEXT(
		"VehiclePathsTooltip",
		"Hide grass and small foliage along saved vehicle paths. Disable to affect buildings only.");
	VehiclePaths->DefaultValue = true;
	VehiclePaths->Value = true;
	VehiclePaths->bRequiresWorldReload = false;
	RootSection->SectionProperties.Add(TEXT("EnableVehiclePaths"), VehiclePaths);
}

void UNoGrassUnderBuildingsConfigurationRegistrar::Initialize(FSubsystemCollectionBase& Collection)
{
	Collection.InitializeDependency<UConfigManager>();
	Super::Initialize(Collection);
	if (UConfigManager* ConfigManager = GetGameInstance()->GetSubsystem<UConfigManager>())
	{
		ConfigManager->RegisterModConfiguration(UNoGrassUnderBuildingsConfiguration::StaticClass());
		PollForConfigurationChanges();
		FEngineUtil::DispatchWhenTimerManagerIsReady(
			TDelegate<void(FTimerManager*)>::CreateUObject(
				this, &UNoGrassUnderBuildingsConfigurationRegistrar::StartPersistenceTimer));
	}
}

void UNoGrassUnderBuildingsConfigurationRegistrar::Deinitialize()
{
	PollForConfigurationChanges();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PersistenceTimer);
	}
	Super::Deinitialize();
}

void UNoGrassUnderBuildingsConfigurationRegistrar::StartPersistenceTimer(FTimerManager* TimerManager)
{
	if (!TimerManager) return;
	TimerManager->SetTimer(
		PersistenceTimer,
		FTimerDelegate::CreateUObject(
			this, &UNoGrassUnderBuildingsConfigurationRegistrar::PollForConfigurationChanges),
		0.5f,
		true);
}

void UNoGrassUnderBuildingsConfigurationRegistrar::PollForConfigurationChanges()
{
	UConfigManager* ConfigManager = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UConfigManager>()
		: nullptr;
	if (!ConfigManager) return;

	static const FConfigId ConfigId{TEXT("NoGrassUnderBuildings"), TEXT("")};
	UConfigPropertySection* Root = ConfigManager->GetConfigurationRootSection(ConfigId);
	if (!Root) return;
	const TObjectPtr<UConfigProperty>* Property =
		Root->SectionProperties.Find(TEXT("EnableVehiclePaths"));
	const UConfigPropertyBool* VehiclePaths = Property
		? Cast<UConfigPropertyBool>(Property->Get())
		: nullptr;
	if (!VehiclePaths) return;

	const int8 CurrentValue = VehiclePaths->Value ? 1 : 0;
	if (LastObservedVehiclePaths < 0)
	{
		LastObservedVehiclePaths = CurrentValue;
		return;
	}
	if (CurrentValue == LastObservedVehiclePaths) return;

	LastObservedVehiclePaths = CurrentValue;
	ConfigManager->MarkConfigurationDirty(ConfigId);
}

bool FNoGrassUnderBuildingsConfigurationStruct::ShouldEnableVehiclePaths(const UObject* WorldContext)
{
	FNoGrassUnderBuildingsConfigurationStruct Config;
	const UWorld* World = GEngine && WorldContext
		? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	if (UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr)
	{
		if (UConfigManager* ConfigManager = GameInstance->GetSubsystem<UConfigManager>())
		{
			static const FConfigId ConfigId{TEXT("NoGrassUnderBuildings"), TEXT("")};
			if (UConfigPropertySection* Root = ConfigManager->GetConfigurationRootSection(ConfigId))
			{
				if (const TObjectPtr<UConfigProperty>* Property =
					Root->SectionProperties.Find(TEXT("EnableVehiclePaths")))
				{
					if (const UConfigPropertyBool* VehiclePaths =
						Cast<UConfigPropertyBool>(Property->Get()))
					{
						return VehiclePaths->Value;
					}
				}
			}
			ConfigManager->FillConfigurationStruct(
				ConfigId,
				FDynamicStructInfo{StaticStruct(), &Config});
		}
	}
	return Config.EnableVehiclePaths;
}

#undef LOCTEXT_NAMESPACE
