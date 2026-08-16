#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"

class AFGBuildable;
class AFGCliffActor;
class UNoGrassLightweightExclusionToken;
class UFGFoliageInstancedSMC;
class UGrassInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class IConsoleObject;
struct FClusterNode;
class FStaticMeshInstanceData;

struct FNoGrassFoliageInstanceKey
{
	TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component;
	int32 InstanceIndex = INDEX_NONE;

	bool operator==(const FNoGrassFoliageInstanceKey& Other) const
	{
		return Component == Other.Component && InstanceIndex == Other.InstanceIndex;
	}

	friend uint32 GetTypeHash(const FNoGrassFoliageInstanceKey& Key)
	{
		return HashCombineFast(GetTypeHash(Key.Component), GetTypeHash(Key.InstanceIndex));
	}
};

struct FNoGrassLightweightKey
{
	UClass* BuildableClass = nullptr;
	int32 RuntimeIndex = INDEX_NONE;

	bool operator==(const FNoGrassLightweightKey& Other) const
	{
		return BuildableClass == Other.BuildableClass && RuntimeIndex == Other.RuntimeIndex;
	}

	friend uint32 GetTypeHash(const FNoGrassLightweightKey& Key)
	{
		return HashCombineFast(GetTypeHash(Key.BuildableClass), GetTypeHash(Key.RuntimeIndex));
	}
};

struct FNoGrassLightweightExclusion
{
	UNoGrassLightweightExclusionToken* Token = nullptr;
	FBox Bounds{ForceInit};
	FTransform Transform = FTransform::Identity;
};

DECLARE_LOG_CATEGORY_EXTERN(LogNoGrassUnderBuildings, Log, All);

class FNoGrassUnderBuildingsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void HandlePostWorldInitialization(UWorld* World, const UWorld::InitializationValues InitializationValues);
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void HandleWorldPostActorTick(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	void ScanBuildables(UWorld* World);
	void ScanLightweightBuildables(UWorld* World);
	void AddLightweightExclusion(
		UWorld* World,
		const FNoGrassLightweightKey& Key,
		const FTransform& Transform,
		const FBox& LocalBounds,
		TArray<FBox>& RefreshBounds);
	void RemoveLightweightExclusion(
		UWorld* World,
		const FNoGrassLightweightKey& Key,
		TArray<FBox>& RefreshBounds);
	void ClearLightweightExclusions(UWorld* World, bool bRefresh);
	void ScanNearbyFoliage(const TArray<FString>& Args);
	void ArmCliffTrace(const TArray<FString>& Args);
	void TraceCliffGrassUpload(
		UGrassInstancedStaticMeshComponent* Component,
		const TArray<FClusterNode>& ClusterTree,
		int32 OcclusionLayerNum,
		int32 NumBuiltRenderInstances,
		const FStaticMeshInstanceData* InstanceData);
	int32 FilterCliffGrassUpload(
		UGrassInstancedStaticMeshComponent* Component,
		FStaticMeshInstanceData* InstanceData);
	void ReconcileDecorativeFoliage(UWorld* World);
	void RestoreAllDecorativeFoliage();
	bool IsDecorativeGroundFoliage(const UHierarchicalInstancedStaticMeshComponent* Component) const;
	void AddLandscapeExclusion(AFGBuildable* Buildable, bool bRefresh = true);
	void RemoveLandscapeExclusion(const TWeakObjectPtr<AFGBuildable>& Buildable);
	void RefreshLandscapeGrass(UWorld* World, const FBox& ChangedBounds);
	void RefreshLandscapeGrass(UWorld* World, const TArray<FBox>& ChangedBounds);
	void RefreshCliffGrass(UWorld* World, const TArray<FBox>& ChangedBounds);
	FDelegateHandle PostWorldInitializationHandle;
	FDelegateHandle WorldCleanupHandle;
	FDelegateHandle WorldPostActorTickHandle;
	TWeakObjectPtr<UWorld> ActiveGameWorld;
	TSet<TWeakObjectPtr<AFGBuildable>> KnownBuildables;
	TSet<TWeakObjectPtr<AFGBuildable>> ExcludedBuildables;
	TMap<TWeakObjectPtr<AFGBuildable>, FBox> ExclusionBounds;
	double NextBuildableScanAt = 0.0;
	bool bInitialBuildableScanComplete = false;
	int32 LastLightweightClassCount = INDEX_NONE;
	int32 LastLightweightInstanceCount = INDEX_NONE;
	TMap<FNoGrassLightweightKey, FNoGrassLightweightExclusion> LightweightExclusions;
	IConsoleObject* ScanNearbyCommand = nullptr;
	IConsoleObject* ArmCliffTraceCommand = nullptr;
	bool bCliffTraceArmed = false;
	bool bAutoCliffTracePending = true;
	FVector CliffTraceCenter = FVector::ZeroVector;
	float CliffTraceRadiusSquared = 0.0f;
	TMap<FNoGrassFoliageInstanceKey, FTransform> SuppressedFoliage;
	uint64 CoverageRevision = 0;
	uint64 AppliedFoliageRevision = MAX_uint64;
};
