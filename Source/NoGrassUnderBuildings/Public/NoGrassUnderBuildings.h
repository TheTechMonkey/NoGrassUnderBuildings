#pragma once

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"

class AFGBuildable;
class AFGBuildableSubsystem;
class AFGCliffActor;
class AFGLightweightBuildableSubsystem;
class UFGFoliageInstancedSMC;
class UGrassInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UPrimitiveComponent;
class ULevel;
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
	FBox Bounds{ForceInit};
	FTransform Transform = FTransform::Identity;
};

struct FNoGrassCollisionFootprint
{
	FBox Bounds{ForceInit};
	FTransform ComponentTransform = FTransform::Identity;
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
	void HandleLevelAddedToWorld(ULevel* Level, UWorld* World);
	void HandleActorDestroyed(AActor* Actor);
	void HandleBuildableAdded(AFGBuildableSubsystem* Subsystem, AFGBuildable* Buildable);
	void HandleBuildableRemoved(AFGBuildableSubsystem* Subsystem, AFGBuildable* Buildable);
	void HandleLightweightAdded(
		AFGLightweightBuildableSubsystem* Subsystem,
		UClass* BuildableClass,
		int32 RuntimeIndex);
	void HandleLightweightRemoving(
		AFGLightweightBuildableSubsystem* Subsystem,
		UClass* BuildableClass,
		int32 RuntimeIndex);
	void QueueCoverageRefresh(const FBox& Bounds, const TCHAR* Reason);
	void ProcessPendingCoverageRefresh(UWorld* World);
	bool IsHorizontalAreaFullyCovered(const FBox& Bounds) const;
	FIntVector GetCoverageGridCell(const FVector& Location) const;
	void AddBuildableToCoverageGrid(const TWeakObjectPtr<AFGBuildable>& Buildable, const FBox& Bounds);
	void RemoveBuildableFromCoverageGrid(const TWeakObjectPtr<AFGBuildable>& Buildable, const FBox& Bounds);
	void AddLightweightToCoverageGrid(const FNoGrassLightweightKey& Key, const FBox& Bounds);
	void RemoveLightweightFromCoverageGrid(const FNoGrassLightweightKey& Key, const FBox& Bounds);
	void GatherCoverageBounds(const FBox& QueryBounds, TArray<FBox>& OutBounds) const;
	bool IsLocationCovered(const FVector& Location) const;
	void ScanBuildables(UWorld* World);
	void ScanBoundslessPowerPoles(UWorld* World);
	bool IsBoundslessPowerPole(const AActor* Actor) const;
	void AddBoundslessPowerPole(AActor* Actor, bool bRefresh = true);
	void RemoveBoundslessPowerPole(AActor* Actor, bool bRefresh = true);
	void AddPowerPoleToCoverageGrid(const TWeakObjectPtr<AActor>& Pole, const FBox& Bounds);
	void RemovePowerPoleFromCoverageGrid(const TWeakObjectPtr<AActor>& Pole, const FBox& Bounds);
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
	int32 FilterGrassUpload(
		UGrassInstancedStaticMeshComponent* Component,
		FStaticMeshInstanceData* InstanceData);
	void ReconcileDecorativeFoliage(
		UWorld* World,
		const TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>>* ComponentFilter = nullptr);
	void ProcessPendingStreamedFoliage(UWorld* World);
	void RestoreAllDecorativeFoliage();
	bool IsDecorativeGroundFoliage(const UHierarchicalInstancedStaticMeshComponent* Component) const;
	FBox GetLandscapeExclusionBounds(AFGBuildable* Buildable) const;
	bool IsCollisionFootprintCovered(
		const TWeakObjectPtr<AFGBuildable>& Buildable,
		const FVector& Location) const;
	void AddLandscapeExclusion(AFGBuildable* Buildable, bool bRefresh = true);
	void RemoveLandscapeExclusion(
		const TWeakObjectPtr<AFGBuildable>& Buildable,
		bool bRefresh = true);
	void RefreshLandscapeGrass(UWorld* World, const FBox& ChangedBounds);
	void RefreshLandscapeGrass(UWorld* World, const TArray<FBox>& ChangedBounds);
	void RefreshCliffGrass(UWorld* World, const TArray<FBox>& ChangedBounds);
	FDelegateHandle PostWorldInitializationHandle;
	FDelegateHandle WorldCleanupHandle;
	FDelegateHandle WorldPostActorTickHandle;
	FDelegateHandle LevelAddedToWorldHandle;
	FDelegateHandle ActorDestroyedHandle;
	TWeakObjectPtr<UWorld> ActiveGameWorld;
	TSet<TWeakObjectPtr<AFGBuildable>> KnownBuildables;
	TSet<TWeakObjectPtr<AFGBuildable>> ExcludedBuildables;
	TMap<TWeakObjectPtr<AFGBuildable>, FBox> ExclusionBounds;
	TMap<TWeakObjectPtr<AFGBuildable>, FNoGrassCollisionFootprint> CollisionFootprints;
	TMap<FIntVector, TSet<TWeakObjectPtr<AFGBuildable>>> BuildableCoverageGrid;
	TMap<TWeakObjectPtr<AActor>, FBox> PowerPoleExclusionBounds;
	TMap<FIntVector, TSet<TWeakObjectPtr<AActor>>> PowerPoleCoverageGrid;
	double NextBuildableScanAt = 0.0;
	bool bInitialBuildableScanComplete = false;
	int32 LastLightweightClassCount = INDEX_NONE;
	int32 LastLightweightInstanceCount = INDEX_NONE;
	TMap<FNoGrassLightweightKey, FNoGrassLightweightExclusion> LightweightExclusions;
	TMap<FIntVector, TSet<FNoGrassLightweightKey>> LightweightCoverageGrid;
	TArray<FBox> PendingRefreshBounds;
	int32 PendingCoverageEventCount = 0;
	double PendingCoverageQueuedAt = 0.0;
	IConsoleObject* ScanNearbyCommand = nullptr;
	IConsoleObject* ArmCliffTraceCommand = nullptr;
	bool bCliffTraceArmed = false;
	bool bAutoCliffTracePending = false;
	FVector CliffTraceCenter = FVector::ZeroVector;
	float CliffTraceRadiusSquared = 0.0f;
	TMap<FNoGrassFoliageInstanceKey, FTransform> SuppressedFoliage;
	TSet<TWeakObjectPtr<ULevel>> PendingStreamedLevels;
	uint64 CoverageRevision = 0;
	uint64 AppliedFoliageRevision = MAX_uint64;
	static constexpr double CoverageGridCellSize = 2000.0;
	static constexpr double CollisionFootprintCellSize = 25.0;
};
