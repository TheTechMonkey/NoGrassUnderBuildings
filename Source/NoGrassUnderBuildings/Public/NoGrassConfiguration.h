#pragma once

#include "CoreMinimal.h"
#include "NoGrassConfiguration.generated.h"

USTRUCT(BlueprintType)
struct NOGRASSUNDERBUILDINGS_API FNoGrassConfigurationStruct
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite)
	float RegrowthMinSeconds = 600.0f;

	UPROPERTY(BlueprintReadWrite)
	float RegrowthMaxSeconds = 900.0f;

	UPROPERTY(BlueprintReadWrite)
	bool GradualRegrowth = true;

	UPROPERTY(BlueprintReadWrite)
	float HorizontalBufferMeters = 1.0f;

	UPROPERTY(BlueprintReadWrite)
	float VerticalBufferMeters = 2.0f;

	UPROPERTY(BlueprintReadWrite)
	bool RegrowGrass = true;

	static FNoGrassConfigurationStruct GetActiveConfig(const UObject* WorldContext);
};
