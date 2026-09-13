#include "NoGrassUnderBuildings.h"
#include "NoGrassUnderBuildingsConfiguration.h"

#include "Buildables/FGBuildable.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGCliffActor.h"
#include "FGFoliageInstancedSMC.h"
#include "GrassInstancedStaticMeshComponent.h"
#include "Patching/NativeHookManager.h"
#include "RenderTransform.h"
#include "StaticMeshResources.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "SpaceElevatorFootprint.inl"

DEFINE_LOG_CATEGORY(LogNoGrassUnderBuildings);

namespace
{
	constexpr double GNoGrassMaxComponentSpan = 100000.0;
	constexpr double GNoGrassMaxComponentDistance = 200000.0;

	bool IsFiniteBox(const FBox& Bounds)
	{
		return Bounds.IsValid &&
			FMath::IsFinite(Bounds.Min.X) && FMath::IsFinite(Bounds.Min.Y) && FMath::IsFinite(Bounds.Min.Z) &&
			FMath::IsFinite(Bounds.Max.X) && FMath::IsFinite(Bounds.Max.Y) && FMath::IsFinite(Bounds.Max.Z);
	}

	bool IsUsableComponentBounds(const FBox& Bounds, const FVector& ActorLocation)
	{
		if (!IsFiniteBox(Bounds))
		{
			return false;
		}

		const FVector Size = Bounds.GetSize();
		const FVector CenterOffset = Bounds.GetCenter() - ActorLocation;
		return Size.X <= GNoGrassMaxComponentSpan &&
			Size.Y <= GNoGrassMaxComponentSpan &&
			Size.Z <= GNoGrassMaxComponentSpan &&
			FMath::Abs(CenterOffset.X) <= GNoGrassMaxComponentDistance &&
			FMath::Abs(CenterOffset.Y) <= GNoGrassMaxComponentDistance &&
			FMath::Abs(CenterOffset.Z) <= GNoGrassMaxComponentDistance;
	}
}

void FNoGrassUnderBuildingsModule::StartupModule()
{
#if !WITH_EDITOR
	SUBSCRIBE_METHOD_AFTER(
		AFGBuildableSubsystem::AddBuildable,
		[this](AFGBuildableSubsystem* Subsystem, AFGBuildable* Buildable)
		{
			HandleBuildableAdded(Subsystem, Buildable);
		});
	SUBSCRIBE_METHOD_AFTER(
		AFGBuildableSubsystem::RemoveBuildable,
		[this](AFGBuildableSubsystem* Subsystem, AFGBuildable* Buildable)
		{
			HandleBuildableRemoved(Subsystem, Buildable);
		});
	SUBSCRIBE_METHOD_AFTER(
		AFGVehicleSubsystem::AddPathSegment,
		[this](AFGVehicleSubsystem* Subsystem, AFGVehiclePathSegment* PathSegment)
		{
			HandleVehiclePathAdded(Subsystem, PathSegment);
		});
	SUBSCRIBE_METHOD_AFTER(
		AFGVehicleSubsystem::RemovePathSegment,
		[this](AFGVehicleSubsystem* Subsystem, AFGVehiclePathSegment* PathSegment)
		{
			HandleVehiclePathRemoved(Subsystem, PathSegment);
		});
	SUBSCRIBE_METHOD_AFTER(
		AFGLightweightBuildableSubsystem::AddFromBuildableInstanceData,
		[this](
			const int32& RuntimeIndex,
			AFGLightweightBuildableSubsystem* Subsystem,
			TSubclassOf<AFGBuildable> BuildableClass,
			FRuntimeBuildableInstanceData&,
			bool,
			int32,
			uint16,
			AActor*,
			int32)
		{
			HandleLightweightAdded(Subsystem, BuildableClass.Get(), RuntimeIndex);
		});
	SUBSCRIBE_METHOD(
		AFGLightweightBuildableSubsystem::InvalidateRuntimeInstanceDataForIndex,
		[this](
			auto&,
			AFGLightweightBuildableSubsystem* Subsystem,
			TSubclassOf<AFGBuildable> BuildableClass,
			int32 RuntimeIndex)
		{
			HandleLightweightRemoving(Subsystem, BuildableClass.Get(), RuntimeIndex);
		});
	SUBSCRIBE_METHOD(
		UGrassInstancedStaticMeshComponent::AcceptPrebuiltTree,
		[this](auto& Scope,
			UGrassInstancedStaticMeshComponent* Component,
			TArray<FClusterNode>& ClusterTree,
			int32 OcclusionLayerNum,
			int32 NumBuiltRenderInstances,
			FStaticMeshInstanceData* InstanceData)
		{
			FilterGrassUpload(Component, InstanceData);
			Scope(
				Component,
				ClusterTree,
				OcclusionLayerNum,
				NumBuiltRenderInstances,
				InstanceData);
		});
#endif
	PostWorldInitializationHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(
		this,
		&FNoGrassUnderBuildingsModule::HandlePostWorldInitialization);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this,
		&FNoGrassUnderBuildingsModule::HandleWorldCleanup);
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(
		this,
		&FNoGrassUnderBuildingsModule::HandleWorldPostActorTick);
	LevelAddedToWorldHandle = FWorldDelegates::LevelAddedToWorld.AddRaw(
		this,
		&FNoGrassUnderBuildingsModule::HandleLevelAddedToWorld);

	UE_LOG(
		LogNoGrassUnderBuildings,
		Display,
		TEXT("Event-driven coverage with streamed-foliage reconciliation loaded"));
}

void FNoGrassUnderBuildingsModule::ShutdownModule()
{
	FWorldDelegates::OnPostWorldInitialization.Remove(PostWorldInitializationHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedToWorldHandle);
	PostWorldInitializationHandle.Reset();
	WorldCleanupHandle.Reset();
	WorldPostActorTickHandle.Reset();
	LevelAddedToWorldHandle.Reset();
	if (UWorld* World = ActiveGameWorld.Get())
	{
		if (ActorDestroyedHandle.IsValid())
		{
			World->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
		}
		for (const TWeakObjectPtr<AFGBuildable>& Buildable : ExcludedBuildables)
		{
			ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Buildable));
		}
		for (const auto& Pair : PowerPoleExclusionBounds)
		{
			ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Pair.Key));
		}
	}
	ActorDestroyedHandle.Reset();
	RestoreAllDecorativeFoliage();
	ClearLightweightExclusions(ActiveGameWorld.Get(), false);
	ActiveGameWorld.Reset();
	KnownBuildables.Empty();
	ExcludedBuildables.Empty();
	ExclusionBounds.Empty();
	CollisionFootprints.Empty();
	BuildableCoverageGrid.Empty();
	VehiclePathExclusions.Empty();
	VehiclePathCoverageGrid.Empty();
	PowerPoleExclusionBounds.Empty();
	PowerPoleCoverageGrid.Empty();
	LastLightweightClassCount = INDEX_NONE;
	LastLightweightInstanceCount = INDEX_NONE;
	LightweightCoverageGrid.Empty();
	PendingRefreshBounds.Empty();
	PendingCoverageEventCount = 0;
	PendingCoverageQueuedAt = 0.0;
	PendingStreamedLevels.Empty();
	UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("No Grass Under Buildings unloaded"));
}

int32 FNoGrassUnderBuildingsModule::FilterGrassUpload(
	UGrassInstancedStaticMeshComponent* Component,
	FStaticMeshInstanceData* InstanceData)
{
	// The coverage maps are maintained on the game thread. Do not touch them
	// from an asynchronous render/grass task.
	if (!IsInGameThread() || !IsValid(Component) || !InstanceData)
	{
		return 0;
	}

	UWorld* World = Component->GetWorld();
	if (!IsValid(World) || World != ActiveGameWorld.Get())
	{
		return 0;
	}

	if (BuildableCoverageGrid.IsEmpty() && VehiclePathCoverageGrid.IsEmpty() &&
		LightweightCoverageGrid.IsEmpty() && PowerPoleCoverageGrid.IsEmpty())
	{
		return 0;
	}

	const double StartedAt = FPlatformTime::Seconds();
	const int32 InstanceCount = InstanceData->GetNumInstances();
	const FTransform ComponentToWorld = Component->GetComponentTransform();
	int32 HiddenCount = 0;
	for (int32 InstanceIndex = 0;
		InstanceIndex < InstanceCount;
		++InstanceIndex)
	{
		FRenderTransform PackedTransform;
		InstanceData->GetInstanceTransform(InstanceIndex, PackedTransform);
		const FVector WorldRoot =
			(FTransform(PackedTransform.ToMatrix()) * ComponentToWorld).GetLocation();

		if (IsLocationCovered(WorldRoot))
		{
			// Keep the buffer length and cluster indices unchanged. Unreal marks
			// only this instance invisible when it accepts the existing tree.
			InstanceData->NullifyInstance(InstanceIndex);
			++HiddenCount;
		}
	}
	const double ElapsedMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
	if (ElapsedMs >= 10.0)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Grass upload spatial filter: instances=%d hidden=%d regular-cells=%d lightweight-cells=%d time=%.2f ms"),
			InstanceCount,
			HiddenCount,
			BuildableCoverageGrid.Num(),
			LightweightCoverageGrid.Num(),
			ElapsedMs);
	}

	return HiddenCount;
}


bool FNoGrassUnderBuildingsModule::IsDecorativeGroundFoliage(
	const UHierarchicalInstancedStaticMeshComponent* Component) const
{
	if (!IsValid(Component) || !Component->GetStaticMesh())
	{
		return false;
	}

	const FString MeshPath = Component->GetStaticMesh()->GetPathName();

	// Field flowers use Unreal's ordinary foliage component rather than the
	// FactoryGame subclass. Keep that path exact so this does not absorb every
	// landscape-generated HISM into the transform suppression system.
	if (MeshPath.Contains(TEXT("/Grass/FieldFlower/Mesh/SM_FieldFlower."), ESearchCase::IgnoreCase))
	{
		return true;
	}

	// Cliff actors render their grass through the engine grass component rather
	// than FactoryGame's foliage component. Admit only cliff-owned grass here;
	// keeping the owner and mesh checks narrow avoids touching unrelated
	// landscape-generated components.
	const bool IsCliffGrass =
		Component->IsA<UGrassInstancedStaticMeshComponent>() &&
		Component->GetTypedOuter<AFGCliffActor>() != nullptr &&
		MeshPath.Contains(TEXT("/Foliage/Grass/"), ESearchCase::IgnoreCase);

	// The remaining supported meshes are deliberately restricted to
	// FactoryGame foliage components.
	if (!IsCliffGrass && !Component->IsA<UFGFoliageInstancedSMC>())
	{
		return false;
	}

	return MeshPath.Contains(TEXT("/Foliage/Grass/"), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("Flower"), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("GroundCover"), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("Ground_Cover"), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("/SmallFoliage/LowerVegatation/SM_CoverGround_01."), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("/SmallFoliage/LowerVegatation/SM_Plant_02."), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("/SmallFoliage/LowerVegatation/SM_Plant_04."), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("/SmallFoliage/LowerVegatation/SM_Plant_07."), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("/SmallFoliage/Ivy/SM_Cliff_Ivy_02."), ESearchCase::IgnoreCase) ||
		MeshPath.Contains(TEXT("/SmallFoliage/PlantModular/SM_PlantModular_D."), ESearchCase::IgnoreCase);
}

void FNoGrassUnderBuildingsModule::ReconcileDecorativeFoliage(
	UWorld* World,
	const TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>>* ComponentFilter)
{
	if (!World)
	{
		return;
	}

	TSet<FNoGrassFoliageInstanceKey> DesiredSuppression;
	TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> DirtyComponents;
	for (TObjectIterator<UHierarchicalInstancedStaticMeshComponent> It; It; ++It)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != World ||
			!Component->IsRegistered() || !IsDecorativeGroundFoliage(Component) ||
			(ComponentFilter && !ComponentFilter->Contains(Component)))
		{
			continue;
		}

		const FBox ComponentBounds = Component->Bounds.GetBox();
		TArray<FBox> CoverageBounds;
		GatherCoverageBounds(ComponentBounds, CoverageBounds);
		for (const FBox& Bounds : CoverageBounds)
		{
			if (!Bounds.IsValid || !ComponentBounds.Intersect(Bounds))
			{
				continue;
			}
			for (const int32 InstanceIndex : Component->GetInstancesOverlappingBox(Bounds, true))
			{
				FTransform InstanceTransform;
				if (!Component->GetInstanceTransform(InstanceIndex, InstanceTransform, true))
				{
					continue;
				}
				if (IsLocationCovered(InstanceTransform.GetLocation()))
				{
					DesiredSuppression.Add({Component, InstanceIndex});
				}
			}
		}
	}

	// Hidden instances no longer overlap their original world-space boxes, so test
	// their saved transforms explicitly when coverage overlaps or is removed.
	for (const auto& Pair : SuppressedFoliage)
	{
		if (!Pair.Key.Component.IsValid() ||
			(ComponentFilter && !ComponentFilter->Contains(Pair.Key.Component)))
		{
			continue;
		}
		if (IsLocationCovered(Pair.Value.GetLocation()))
		{
			DesiredSuppression.Add(Pair.Key);
		}
	}

	for (const FNoGrassFoliageInstanceKey& Key : DesiredSuppression)
	{
		if (const FTransform* StoredTransform = SuppressedFoliage.Find(Key))
		{
			UHierarchicalInstancedStaticMeshComponent* Component = Key.Component.Get();
			if (IsValid(Component) && Key.InstanceIndex >= 0 &&
				Key.InstanceIndex < Component->GetInstanceCount())
			{
				FTransform HiddenTransform = *StoredTransform;
				HiddenTransform.AddToTranslation(FVector(0.0f, 0.0f, -100000.0f));
				if (Component->UpdateInstanceTransform(
					Key.InstanceIndex,
					HiddenTransform,
					true,
					false,
					true))
				{
					DirtyComponents.Add(Component);
				}
			}
			continue;
		}
		UHierarchicalInstancedStaticMeshComponent* Component = Key.Component.Get();
		if (!IsValid(Component) || Key.InstanceIndex < 0 || Key.InstanceIndex >= Component->GetInstanceCount())
		{
			continue;
		}

		FTransform OriginalTransform;
		if (!Component->GetInstanceTransform(Key.InstanceIndex, OriginalTransform, true))
		{
			continue;
		}
		FTransform HiddenTransform = OriginalTransform;
		HiddenTransform.AddToTranslation(FVector(0.0f, 0.0f, -100000.0f));
		if (Component->UpdateInstanceTransform(
			Key.InstanceIndex,
			HiddenTransform,
			true,
			false,
			true))
		{
			SuppressedFoliage.Add(Key, OriginalTransform);
			DirtyComponents.Add(Component);
		}
	}

	TArray<FNoGrassFoliageInstanceKey> RestoreKeys;
	for (const auto& Pair : SuppressedFoliage)
	{
		if ((!ComponentFilter || ComponentFilter->Contains(Pair.Key.Component)) &&
			!DesiredSuppression.Contains(Pair.Key))
		{
			RestoreKeys.Add(Pair.Key);
		}
	}
	for (const FNoGrassFoliageInstanceKey& Key : RestoreKeys)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = Key.Component.Get();
		const FTransform* OriginalTransform = SuppressedFoliage.Find(Key);
		if (IsValid(Component) && OriginalTransform &&
			Key.InstanceIndex >= 0 && Key.InstanceIndex < Component->GetInstanceCount())
		{
			Component->UpdateInstanceTransform(
				Key.InstanceIndex,
				*OriginalTransform,
				true,
				false,
				true);
			DirtyComponents.Add(Component);
		}
		SuppressedFoliage.Remove(Key);
	}

	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Component : DirtyComponents)
	{
		if (Component.IsValid())
		{
			Component->MarkRenderStateDirty();
		}
	}
}

void FNoGrassUnderBuildingsModule::HandleLevelAddedToWorld(ULevel* Level, UWorld* World)
{
	if (!Level || !World || World != ActiveGameWorld.Get() || !bInitialBuildableScanComplete)
	{
		return;
	}
	PendingStreamedLevels.Add(Level);
}

void FNoGrassUnderBuildingsModule::ProcessPendingStreamedFoliage(UWorld* World)
{
	if (!World || PendingStreamedLevels.IsEmpty())
	{
		return;
	}

	TSet<TWeakObjectPtr<ULevel>> StreamedLevels = MoveTemp(PendingStreamedLevels);
	PendingStreamedLevels.Reset();
	TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Components;
	for (TObjectIterator<UHierarchicalInstancedStaticMeshComponent> It; It; ++It)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != World ||
			!Component->IsRegistered() || !IsDecorativeGroundFoliage(Component))
		{
			continue;
		}
		ULevel* ComponentLevel = Component->GetTypedOuter<ULevel>();
		if (!ComponentLevel && Component->GetOwner())
		{
			ComponentLevel = Component->GetOwner()->GetLevel();
		}
		if (StreamedLevels.Contains(ComponentLevel))
		{
			Components.Add(Component);
		}
	}

	if (!Components.IsEmpty())
	{
		const double StartedAt = FPlatformTime::Seconds();
		ReconcileDecorativeFoliage(World, &Components);
		const double ElapsedMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;
		if (ElapsedMs >= 10.0)
		{
			UE_LOG(
				LogNoGrassUnderBuildings,
				Display,
				TEXT("Slow streamed-foliage reconciliation: components=%d levels=%d time=%.2f ms"),
				Components.Num(),
				StreamedLevels.Num(),
				ElapsedMs);
		}
		else
		{
			UE_LOG(
				LogNoGrassUnderBuildings,
				Verbose,
				TEXT("Reconciled %d decorative foliage component(s) from %d streamed level(s) in %.2f ms"),
				Components.Num(),
				StreamedLevels.Num(),
				ElapsedMs);
		}
	}
}

void FNoGrassUnderBuildingsModule::RestoreAllDecorativeFoliage()
{
	TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> DirtyComponents;
	for (const auto& Pair : SuppressedFoliage)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = Pair.Key.Component.Get();
		if (IsValid(Component) && Pair.Key.InstanceIndex >= 0 &&
			Pair.Key.InstanceIndex < Component->GetInstanceCount())
		{
			Component->UpdateInstanceTransform(
				Pair.Key.InstanceIndex,
				Pair.Value,
				true,
				false,
				true);
			DirtyComponents.Add(Component);
		}
	}
	SuppressedFoliage.Empty();
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Component : DirtyComponents)
	{
		if (Component.IsValid())
		{
			Component->MarkRenderStateDirty();
		}
	}
}

void FNoGrassUnderBuildingsModule::HandlePostWorldInitialization(
	UWorld* World,
	const UWorld::InitializationValues InitializationValues)
{
	(void)InitializationValues;
	if (World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE))
	{
		if (UWorld* PreviousWorld = ActiveGameWorld.Get(); PreviousWorld && PreviousWorld != World)
		{
			if (ActorDestroyedHandle.IsValid())
			{
				PreviousWorld->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
			}
			for (const TWeakObjectPtr<AFGBuildable>& Buildable : ExcludedBuildables)
			{
				ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Buildable));
			}
			for (const auto& Pair : PowerPoleExclusionBounds)
			{
				ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Pair.Key));
			}
			RestoreAllDecorativeFoliage();
			ClearLightweightExclusions(PreviousWorld, false);
		}
		ActiveGameWorld = World;
		ActorDestroyedHandle = World->AddOnActorDestroyedHandler(
			FOnActorDestroyed::FDelegate::CreateRaw(this, &FNoGrassUnderBuildingsModule::HandleActorDestroyed));
		KnownBuildables.Empty();
		ExcludedBuildables.Empty();
		ExclusionBounds.Empty();
		CollisionFootprints.Empty();
		BuildableCoverageGrid.Empty();
		VehiclePathExclusions.Empty();
		VehiclePathCoverageGrid.Empty();
		PowerPoleExclusionBounds.Empty();
		PowerPoleCoverageGrid.Empty();
		NextBuildableScanAt = 0.0;
		NextVehiclePathSettingCheckAt = 0.0;
		bVehiclePathsEnabled = FNoGrassUnderBuildingsConfigurationStruct::ShouldEnableVehiclePaths(World);
		bInitialBuildableScanComplete = false;
		LastLightweightClassCount = INDEX_NONE;
		LastLightweightInstanceCount = INDEX_NONE;
		LightweightCoverageGrid.Empty();
		PendingRefreshBounds.Empty();
		PendingCoverageEventCount = 0;
		PendingCoverageQueuedAt = 0.0;
		PendingStreamedLevels.Empty();
		CoverageRevision = 0;
		AppliedFoliageRevision = MAX_uint64;
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Game world initialized: %s"),
			*World->GetPathName());
	}
}

void FNoGrassUnderBuildingsModule::HandleWorldCleanup(
	UWorld* World,
	bool bSessionEnded,
	bool bCleanupResources)
{
	if (World && ActiveGameWorld.Get() == World)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Game world cleanup: %s session-ended=%s cleanup-resources=%s"),
			*World->GetPathName(),
			bSessionEnded ? TEXT("true") : TEXT("false"),
			bCleanupResources ? TEXT("true") : TEXT("false"));
		for (const TWeakObjectPtr<AFGBuildable>& Buildable : ExcludedBuildables)
		{
			ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Buildable));
		}
		for (const auto& Pair : PowerPoleExclusionBounds)
		{
			ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Pair.Key));
		}
		if (ActorDestroyedHandle.IsValid())
		{
			World->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
			ActorDestroyedHandle.Reset();
		}
		RestoreAllDecorativeFoliage();
		ClearLightweightExclusions(World, false);
		ActiveGameWorld.Reset();
		KnownBuildables.Empty();
		ExcludedBuildables.Empty();
		ExclusionBounds.Empty();
		CollisionFootprints.Empty();
		BuildableCoverageGrid.Empty();
		VehiclePathExclusions.Empty();
		VehiclePathCoverageGrid.Empty();
		PowerPoleExclusionBounds.Empty();
		PowerPoleCoverageGrid.Empty();
		NextBuildableScanAt = 0.0;
		NextVehiclePathSettingCheckAt = 0.0;
		bVehiclePathsEnabled = true;
		bInitialBuildableScanComplete = false;
		LastLightweightClassCount = INDEX_NONE;
		LastLightweightInstanceCount = INDEX_NONE;
		LightweightCoverageGrid.Empty();
		PendingRefreshBounds.Empty();
		PendingCoverageEventCount = 0;
		PendingCoverageQueuedAt = 0.0;
		PendingStreamedLevels.Empty();
		CoverageRevision = 0;
		AppliedFoliageRevision = MAX_uint64;
	}
}

void FNoGrassUnderBuildingsModule::HandleWorldPostActorTick(
	UWorld* World,
	ELevelTick TickType,
	float DeltaSeconds)
{
	(void)TickType;
	(void)DeltaSeconds;
	if (!World || World != ActiveGameWorld.Get() || !World->HasBegunPlay())
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	if (Now >= NextVehiclePathSettingCheckAt)
	{
		NextVehiclePathSettingCheckAt = Now + 1.0;
		const bool bSettingEnabled =
			FNoGrassUnderBuildingsConfigurationStruct::ShouldEnableVehiclePaths(World);
		if (bSettingEnabled != bVehiclePathsEnabled)
		{
			ApplyVehiclePathSetting(World, bSettingEnabled);
		}
	}
	if (!bInitialBuildableScanComplete && Now >= NextBuildableScanAt)
	{
		ScanBuildables(World);
		ScanLightweightBuildables(World);
		ScanBoundslessPowerPoles(World);
		if (AppliedFoliageRevision != CoverageRevision)
		{
			ReconcileDecorativeFoliage(World);
			AppliedFoliageRevision = CoverageRevision;
		}
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Initial coverage scan complete; event-driven tracking active"));
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Coverage spatial index: regular-cells=%d vehicle-path-cells=%d lightweight-cells=%d"),
			BuildableCoverageGrid.Num(),
			VehiclePathCoverageGrid.Num(),
			LightweightCoverageGrid.Num());
	}

	ProcessPendingCoverageRefresh(World);
	ProcessPendingStreamedFoliage(World);
}

bool FNoGrassUnderBuildingsModule::IsBoundslessPowerPole(const AActor* Actor) const
{
	return IsValid(Actor) &&
		Actor->GetClass()->GetName().StartsWith(TEXT("Build_PowerPole")) &&
		!Actor->GetComponentsBoundingBox(true).IsValid;
}

void FNoGrassUnderBuildingsModule::HandleActorDestroyed(AActor* Actor)
{
	if (Actor && PowerPoleExclusionBounds.Contains(TWeakObjectPtr<AActor>(Actor)))
	{
		RemoveBoundslessPowerPole(Actor, true);
	}
}

void FNoGrassUnderBuildingsModule::ScanBoundslessPowerPoles(UWorld* World)
{
	if (!World)
	{
		return;
	}

	TArray<FBox> InitialRefreshBounds;
	int32 Added = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		const TWeakObjectPtr<AActor> Key(Actor);
		if (!IsBoundslessPowerPole(Actor) || PowerPoleExclusionBounds.Contains(Key))
		{
			continue;
		}

		AddBoundslessPowerPole(Actor, false);
		if (const FBox* Bounds = PowerPoleExclusionBounds.Find(Key))
		{
			InitialRefreshBounds.Add(*Bounds);
			++Added;
		}
	}

	if (!InitialRefreshBounds.IsEmpty())
	{
		RefreshLandscapeGrass(World, InitialRefreshBounds);
		++CoverageRevision;
	}
	UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("Bounds-less power pole scan: added=%d tracked=%d"),
		Added, PowerPoleExclusionBounds.Num());
}

void FNoGrassUnderBuildingsModule::AddBoundslessPowerPole(AActor* Actor, bool bRefresh)
{
	if (!IsBoundslessPowerPole(Actor))
	{
		return;
	}

	const TWeakObjectPtr<AActor> Key(Actor);
	if (PowerPoleExclusionBounds.Contains(Key))
	{
		return;
	}

	const FVector Center = Actor->GetActorLocation();
	const FBox Bounds(
		Center - FVector(125.0f, 125.0f, 200.0f),
		Center + FVector(125.0f, 125.0f, 200.0f));
	ALandscapeProxy::AddExclusionBox(FWeakObjectPtr(Actor), Bounds);
	PowerPoleExclusionBounds.Add(Key, Bounds);
	AddPowerPoleToCoverageGrid(Key, Bounds);
	if (bRefresh)
	{
		QueueCoverageRefresh(Bounds, TEXT("boundsless-power-pole-added"));
	}
}

void FNoGrassUnderBuildingsModule::RemoveBoundslessPowerPole(AActor* Actor, bool bRefresh)
{
	if (!Actor)
	{
		return;
	}

	const TWeakObjectPtr<AActor> Key(Actor);
	const FBox* StoredBounds = PowerPoleExclusionBounds.Find(Key);
	if (!StoredBounds)
	{
		return;
	}

	const FBox PreviousBounds = *StoredBounds;
	ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Actor));
	RemovePowerPoleFromCoverageGrid(Key, PreviousBounds);
	PowerPoleExclusionBounds.Remove(Key);
	if (bRefresh)
	{
		QueueCoverageRefresh(PreviousBounds, TEXT("boundsless-power-pole-removed"));
	}
}

void FNoGrassUnderBuildingsModule::HandleBuildableAdded(
	AFGBuildableSubsystem* Subsystem,
	AFGBuildable* Buildable)
{
	UWorld* World = IsValid(Subsystem) ? Subsystem->GetWorld() : nullptr;
	if (!bInitialBuildableScanComplete || World != ActiveGameWorld.Get() ||
		!IsValid(Buildable) || Buildable->GetWorld() != World)
	{
		return;
	}
	if (IsBoundslessPowerPole(Buildable))
	{
		AddBoundslessPowerPole(Buildable, true);
		return;
	}
	const TWeakObjectPtr<AFGBuildable> Key(Buildable);
	if (AFGVehiclePathSegment* PathSegment = Cast<AFGVehiclePathSegment>(Buildable))
	{
		KnownBuildables.Add(Key);
		HandleVehiclePathAdded(AFGVehicleSubsystem::Get(World), PathSegment);
		return;
	}
	if (ExclusionBounds.Contains(Key))
	{
		return;
	}

	const FBox Bounds = GetLandscapeExclusionBounds(Buildable);
	if (!Bounds.IsValid)
	{
		return;
	}
	const bool bAlreadyCovered = IsHorizontalAreaFullyCovered(Bounds);
	KnownBuildables.Add(Key);
	AddLandscapeExclusion(Buildable, false);
	if (!bAlreadyCovered)
	{
		QueueCoverageRefresh(Bounds, TEXT("buildable-added"));
	}
}

void FNoGrassUnderBuildingsModule::HandleBuildableRemoved(
	AFGBuildableSubsystem* Subsystem,
	AFGBuildable* Buildable)
{
	UWorld* World = IsValid(Subsystem) ? Subsystem->GetWorld() : nullptr;
	if (!bInitialBuildableScanComplete || World != ActiveGameWorld.Get() || !Buildable)
	{
		return;
	}

	const TWeakObjectPtr<AFGBuildable> Key(Buildable);
	if (VehiclePathExclusions.Contains(Key))
	{
		RemoveVehiclePathExclusion(Key, true);
		KnownBuildables.Remove(Key);
		return;
	}
	const FBox* StoredBounds = ExclusionBounds.Find(Key);
	if (!StoredBounds)
	{
		KnownBuildables.Remove(Key);
		return;
	}

	const FBox PreviousBounds = *StoredBounds;
	RemoveLandscapeExclusion(Key, false);
	KnownBuildables.Remove(Key);
	if (!IsHorizontalAreaFullyCovered(PreviousBounds))
	{
		QueueCoverageRefresh(PreviousBounds, TEXT("buildable-removed"));
	}
}

void FNoGrassUnderBuildingsModule::HandleVehiclePathAdded(
	AFGVehicleSubsystem* Subsystem,
	AFGVehiclePathSegment* PathSegment)
{
	UWorld* World = IsValid(Subsystem) ? Subsystem->GetWorld() : nullptr;
	if (!bVehiclePathsEnabled || !bInitialBuildableScanComplete || World != ActiveGameWorld.Get() ||
		!IsValid(PathSegment) || PathSegment->GetWorld() != World)
	{
		return;
	}

	const TWeakObjectPtr<AFGBuildable> Key(PathSegment);
	if (VehiclePathExclusions.Contains(Key))
	{
		return;
	}

	KnownBuildables.Add(Key);
	AddVehiclePathExclusion(PathSegment, false);
	if (const FNoGrassVehiclePathExclusion* Exclusion = VehiclePathExclusions.Find(Key))
	{
		for (const FBox& Bounds : Exclusion->SegmentBounds)
		{
			QueueCoverageRefresh(Bounds, TEXT("vehicle-path-added"));
		}
	}
}

void FNoGrassUnderBuildingsModule::HandleVehiclePathRemoved(
	AFGVehicleSubsystem* Subsystem,
	AFGVehiclePathSegment* PathSegment)
{
	UWorld* World = IsValid(Subsystem) ? Subsystem->GetWorld() : nullptr;
	if (!bInitialBuildableScanComplete || World != ActiveGameWorld.Get() || !PathSegment)
	{
		return;
	}

	const TWeakObjectPtr<AFGBuildable> Key(PathSegment);
	RemoveVehiclePathExclusion(Key, true);
	KnownBuildables.Remove(Key);
}

void FNoGrassUnderBuildingsModule::HandleLightweightAdded(
	AFGLightweightBuildableSubsystem* Subsystem,
	UClass* BuildableClass,
	int32 RuntimeIndex)
{
	UWorld* World = IsValid(Subsystem) ? Subsystem->GetWorld() : nullptr;
	if (!bInitialBuildableScanComplete || World != ActiveGameWorld.Get() ||
		!BuildableClass || RuntimeIndex == INDEX_NONE)
	{
		return;
	}

	const FNoGrassLightweightKey Key{BuildableClass, RuntimeIndex};
	if (LightweightExclusions.Contains(Key))
	{
		return;
	}

	FRuntimeBuildableInstanceData* RuntimeData =
		Subsystem->GetRuntimeDataForBuildableClassAndIndex(BuildableClass, RuntimeIndex);
	if (!RuntimeData || !RuntimeData->IsValidOnLoad())
	{
		return;
	}
	const FBox Bounds = RuntimeData->BoundingBox.TransformBy(RuntimeData->Transform).ExpandBy(
		FVector(100.0f, 100.0f, 200.0f));
	const bool bAlreadyCovered = IsHorizontalAreaFullyCovered(Bounds);
	TArray<FBox> AddedBounds;
	AddLightweightExclusion(
		World,
		Key,
		RuntimeData->Transform,
		RuntimeData->BoundingBox,
		AddedBounds);
	if (!bAlreadyCovered && !AddedBounds.IsEmpty())
	{
		QueueCoverageRefresh(AddedBounds[0], TEXT("lightweight-added"));
	}
}

void FNoGrassUnderBuildingsModule::HandleLightweightRemoving(
	AFGLightweightBuildableSubsystem* Subsystem,
	UClass* BuildableClass,
	int32 RuntimeIndex)
{
	UWorld* World = IsValid(Subsystem) ? Subsystem->GetWorld() : nullptr;
	if (!bInitialBuildableScanComplete || World != ActiveGameWorld.Get() || !BuildableClass)
	{
		return;
	}

	const FNoGrassLightweightKey Key{BuildableClass, RuntimeIndex};
	const FNoGrassLightweightExclusion* Existing = LightweightExclusions.Find(Key);
	if (!Existing)
	{
		return;
	}

	const FBox PreviousBounds = Existing->Bounds;
	TArray<FBox> RemovedBounds;
	RemoveLightweightExclusion(World, Key, RemovedBounds);
	if (!IsHorizontalAreaFullyCovered(PreviousBounds))
	{
		QueueCoverageRefresh(PreviousBounds, TEXT("lightweight-removed"));
	}
}

void FNoGrassUnderBuildingsModule::QueueCoverageRefresh(
	const FBox& Bounds,
	const TCHAR* Reason)
{
	if (!Bounds.IsValid)
	{
		return;
	}

	PendingRefreshBounds.Add(Bounds);
	++PendingCoverageEventCount;
	if (PendingCoverageQueuedAt <= 0.0)
	{
		PendingCoverageQueuedAt = FPlatformTime::Seconds();
	}
	UE_LOG(LogNoGrassUnderBuildings, Verbose, TEXT("Queued coverage change: %s"), Reason);
}

void FNoGrassUnderBuildingsModule::ProcessPendingCoverageRefresh(UWorld* World)
{
	if (!World || PendingRefreshBounds.IsEmpty())
	{
		return;
	}

	TArray<FBox> RefreshBounds = MoveTemp(PendingRefreshBounds);
	PendingRefreshBounds.Reset();
	const int32 EventCount = PendingCoverageEventCount;
	PendingCoverageEventCount = 0;
	const double QueuedAt = PendingCoverageQueuedAt;
	PendingCoverageQueuedAt = 0.0;
	const double StartedAt = FPlatformTime::Seconds();

	RefreshLandscapeGrass(World, RefreshBounds);
	++CoverageRevision;
	ReconcileDecorativeFoliage(World);
	AppliedFoliageRevision = CoverageRevision;

	const double FinishedAt = FPlatformTime::Seconds();
	UE_LOG(
		LogNoGrassUnderBuildings,
		Verbose,
		TEXT("Processed %d coverage event(s) across %d affected bound(s) in %.2f ms (queued %.2f ms)"),
		EventCount,
		RefreshBounds.Num(),
		(FinishedAt - StartedAt) * 1000.0,
		QueuedAt > 0.0 ? (StartedAt - QueuedAt) * 1000.0 : 0.0);
}

bool FNoGrassUnderBuildingsModule::IsHorizontalAreaFullyCovered(const FBox& Bounds) const
{
	if (!Bounds.IsValid)
	{
		return false;
	}

	TArray<FBox> Candidates;
	auto AddCandidate = [&Candidates, &Bounds](const FBox& Candidate)
	{
		if (Candidate.IsValid && Candidate.Max.X > Bounds.Min.X && Candidate.Min.X < Bounds.Max.X &&
			Candidate.Max.Y > Bounds.Min.Y && Candidate.Min.Y < Bounds.Max.Y &&
			Candidate.Max.Z > Bounds.Min.Z && Candidate.Min.Z < Bounds.Max.Z)
		{
			Candidates.Add(Candidate);
		}
	};
	for (const auto& Pair : ExclusionBounds)
	{
		// A physical collision footprint is not a filled rectangle, so it cannot
		// prove that an entire horizontal area is already covered.
		if (!CollisionFootprints.Contains(Pair.Key))
		{
			AddCandidate(Pair.Value);
		}
	}
	for (const auto& Pair : LightweightExclusions)
	{
		AddCandidate(Pair.Value.Bounds);
	}
	if (Candidates.IsEmpty())
	{
		return false;
	}

	TArray<double> XCoordinates{Bounds.Min.X, Bounds.Max.X};
	TArray<double> YCoordinates{Bounds.Min.Y, Bounds.Max.Y};
	for (const FBox& Candidate : Candidates)
	{
		XCoordinates.Add(FMath::Clamp<double>(Candidate.Min.X, Bounds.Min.X, Bounds.Max.X));
		XCoordinates.Add(FMath::Clamp<double>(Candidate.Max.X, Bounds.Min.X, Bounds.Max.X));
		YCoordinates.Add(FMath::Clamp<double>(Candidate.Min.Y, Bounds.Min.Y, Bounds.Max.Y));
		YCoordinates.Add(FMath::Clamp<double>(Candidate.Max.Y, Bounds.Min.Y, Bounds.Max.Y));
	}
	XCoordinates.Sort();
	YCoordinates.Sort();

	for (int32 XIndex = 0; XIndex + 1 < XCoordinates.Num(); ++XIndex)
	{
		if (FMath::IsNearlyEqual(XCoordinates[XIndex], XCoordinates[XIndex + 1]))
		{
			continue;
		}
		const double X = (XCoordinates[XIndex] + XCoordinates[XIndex + 1]) * 0.5;
		for (int32 YIndex = 0; YIndex + 1 < YCoordinates.Num(); ++YIndex)
		{
			if (FMath::IsNearlyEqual(YCoordinates[YIndex], YCoordinates[YIndex + 1]))
			{
				continue;
			}
			const double Y = (YCoordinates[YIndex] + YCoordinates[YIndex + 1]) * 0.5;
			const bool bCellCovered = Candidates.ContainsByPredicate(
				[X, Y](const FBox& Candidate)
				{
					return X >= Candidate.Min.X && X <= Candidate.Max.X &&
						Y >= Candidate.Min.Y && Y <= Candidate.Max.Y;
				});
			if (!bCellCovered)
			{
				return false;
			}
		}
	}

	return true;
}

FIntVector FNoGrassUnderBuildingsModule::GetCoverageGridCell(const FVector& Location) const
{
	return FIntVector(
		FMath::FloorToInt(Location.X / CoverageGridCellSize),
		FMath::FloorToInt(Location.Y / CoverageGridCellSize),
		FMath::FloorToInt(Location.Z / CoverageGridCellSize));
}

bool FNoGrassUnderBuildingsModule::TryGetCoverageGridRange(
	const FBox& Bounds,
	FIntVector& OutMinCell,
	FIntVector& OutMaxCell) const
{
	if (!IsFiniteBox(Bounds))
	{
		return false;
	}

	const FVector MinGrid = Bounds.Min / CoverageGridCellSize;
	const FVector MaxGrid = Bounds.Max / CoverageGridCellSize;
	constexpr double MinSafeCell = static_cast<double>(MIN_int32) + 1.0;
	constexpr double MaxSafeCell = static_cast<double>(MAX_int32) - 1.0;
	if (MinGrid.X < MinSafeCell || MinGrid.Y < MinSafeCell || MinGrid.Z < MinSafeCell ||
		MaxGrid.X > MaxSafeCell || MaxGrid.Y > MaxSafeCell || MaxGrid.Z > MaxSafeCell)
	{
		return false;
	}

	OutMinCell = GetCoverageGridCell(Bounds.Min);
	OutMaxCell = GetCoverageGridCell(Bounds.Max);
	const int64 CellCountX = static_cast<int64>(OutMaxCell.X) - OutMinCell.X + 1;
	const int64 CellCountY = static_cast<int64>(OutMaxCell.Y) - OutMinCell.Y + 1;
	const int64 CellCountZ = static_cast<int64>(OutMaxCell.Z) - OutMinCell.Z + 1;
	if (CellCountX <= 0 || CellCountY <= 0 || CellCountZ <= 0 ||
		CellCountX > CoverageGridMaxCellsPerBounds ||
		CellCountY > CoverageGridMaxCellsPerBounds / CellCountX ||
		CellCountZ > CoverageGridMaxCellsPerBounds / (CellCountX * CellCountY))
	{
		return false;
	}
	return true;
}

void FNoGrassUnderBuildingsModule::AddBuildableToCoverageGrid(
	const TWeakObjectPtr<AFGBuildable>& Buildable,
	const FBox& Bounds)
{
	if (!Buildable.IsValid() || !Bounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
	{
		UE_LOG(LogNoGrassUnderBuildings, Warning,
			TEXT("Skipped unsafe buildable coverage-grid bounds: min=%s max=%s class=%s"),
			*Bounds.Min.ToString(), *Bounds.Max.ToString(), *GetNameSafe(Buildable.IsValid() ? Buildable->GetClass() : nullptr));
		return;
	}
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				BuildableCoverageGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(Buildable);
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::RemoveBuildableFromCoverageGrid(
	const TWeakObjectPtr<AFGBuildable>& Buildable,
	const FBox& Bounds)
{
	if (!Bounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
	{
		return;
	}
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				const FIntVector Cell(X, Y, Z);
				if (TSet<TWeakObjectPtr<AFGBuildable>>* Entries = BuildableCoverageGrid.Find(Cell))
				{
					Entries->Remove(Buildable);
					if (Entries->IsEmpty())
					{
						BuildableCoverageGrid.Remove(Cell);
					}
				}
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::AddVehiclePathToCoverageGrid(
	const TWeakObjectPtr<AFGBuildable>& PathSegment,
	const TArray<FBox>& SegmentBounds)
{
	if (!PathSegment.IsValid())
	{
		return;
	}
	for (const FBox& Bounds : SegmentBounds)
	{
		FIntVector MinCell;
		FIntVector MaxCell;
		if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
		{
			continue;
		}
		for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
		{
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			{
				for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
				{
					VehiclePathCoverageGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(PathSegment);
				}
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::RemoveVehiclePathFromCoverageGrid(
	const TWeakObjectPtr<AFGBuildable>& PathSegment,
	const TArray<FBox>& SegmentBounds)
{
	for (const FBox& Bounds : SegmentBounds)
	{
		FIntVector MinCell;
		FIntVector MaxCell;
		if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
		{
			continue;
		}
		for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
		{
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			{
				for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
				{
					const FIntVector Cell(X, Y, Z);
					if (TSet<TWeakObjectPtr<AFGBuildable>>* Entries = VehiclePathCoverageGrid.Find(Cell))
					{
						Entries->Remove(PathSegment);
						if (Entries->IsEmpty())
						{
							VehiclePathCoverageGrid.Remove(Cell);
						}
					}
				}
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::AddPowerPoleToCoverageGrid(
	const TWeakObjectPtr<AActor>& Pole,
	const FBox& Bounds)
{
	if (!Pole.IsValid() || !Bounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
	{
		UE_LOG(LogNoGrassUnderBuildings, Warning,
			TEXT("Skipped unsafe power-pole coverage-grid bounds: min=%s max=%s"),
			*Bounds.Min.ToString(), *Bounds.Max.ToString());
		return;
	}
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				PowerPoleCoverageGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(Pole);
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::RemovePowerPoleFromCoverageGrid(
	const TWeakObjectPtr<AActor>& Pole,
	const FBox& Bounds)
{
	if (!Bounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
	{
		return;
	}
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				const FIntVector Cell(X, Y, Z);
				if (TSet<TWeakObjectPtr<AActor>>* Entries = PowerPoleCoverageGrid.Find(Cell))
				{
					Entries->Remove(Pole);
					if (Entries->IsEmpty())
					{
						PowerPoleCoverageGrid.Remove(Cell);
					}
				}
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::AddLightweightToCoverageGrid(
	const FNoGrassLightweightKey& Key,
	const FBox& Bounds)
{
	if (!Key.BuildableClass || !Bounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
	{
		UE_LOG(LogNoGrassUnderBuildings, Warning,
			TEXT("Skipped unsafe lightweight coverage-grid bounds: min=%s max=%s class=%s"),
			*Bounds.Min.ToString(), *Bounds.Max.ToString(), *GetNameSafe(Key.BuildableClass));
		return;
	}
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				LightweightCoverageGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(Key);
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::RemoveLightweightFromCoverageGrid(
	const FNoGrassLightweightKey& Key,
	const FBox& Bounds)
{
	if (!Bounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(Bounds, MinCell, MaxCell))
	{
		return;
	}
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				const FIntVector Cell(X, Y, Z);
				if (TSet<FNoGrassLightweightKey>* Entries = LightweightCoverageGrid.Find(Cell))
				{
					Entries->Remove(Key);
					if (Entries->IsEmpty())
					{
						LightweightCoverageGrid.Remove(Cell);
					}
				}
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::GatherCoverageBounds(
	const FBox& QueryBounds,
	TArray<FBox>& OutBounds) const
{
	if (!QueryBounds.IsValid)
	{
		return;
	}
	FIntVector MinCell;
	FIntVector MaxCell;
	if (!TryGetCoverageGridRange(QueryBounds, MinCell, MaxCell))
	{
		return;
	}
	TSet<TWeakObjectPtr<AFGBuildable>> SeenBuildables;
	TSet<TWeakObjectPtr<AFGBuildable>> SeenVehiclePaths;
	TSet<FNoGrassLightweightKey> SeenLightweights;
	TSet<TWeakObjectPtr<AActor>> SeenPowerPoles;
	for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
	{
		for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
		{
			for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
			{
				const FIntVector Cell(X, Y, Z);
				if (const TSet<TWeakObjectPtr<AFGBuildable>>* Entries = BuildableCoverageGrid.Find(Cell))
				{
					for (const TWeakObjectPtr<AFGBuildable>& Key : *Entries)
					{
						if (!SeenBuildables.Contains(Key))
						{
							SeenBuildables.Add(Key);
							if (const FBox* Bounds = ExclusionBounds.Find(Key);
								Bounds && Bounds->IsValid && Bounds->Intersect(QueryBounds))
							{
								OutBounds.Add(*Bounds);
							}
						}
					}
				}
				if (const TSet<TWeakObjectPtr<AFGBuildable>>* Entries = VehiclePathCoverageGrid.Find(Cell))
				{
					for (const TWeakObjectPtr<AFGBuildable>& Key : *Entries)
					{
						if (SeenVehiclePaths.Contains(Key))
						{
							continue;
						}
						SeenVehiclePaths.Add(Key);
						if (const FNoGrassVehiclePathExclusion* Exclusion = VehiclePathExclusions.Find(Key))
						{
							for (const FBox& Bounds : Exclusion->SegmentBounds)
							{
								if (Bounds.Intersect(QueryBounds))
								{
									OutBounds.Add(Bounds);
								}
							}
						}
					}
				}
				if (const TSet<FNoGrassLightweightKey>* Entries = LightweightCoverageGrid.Find(Cell))
				{
					for (const FNoGrassLightweightKey& Key : *Entries)
					{
						if (!SeenLightweights.Contains(Key))
						{
							SeenLightweights.Add(Key);
							if (const FNoGrassLightweightExclusion* Existing = LightweightExclusions.Find(Key);
								Existing && Existing->Bounds.IsValid && Existing->Bounds.Intersect(QueryBounds))
							{
								OutBounds.Add(Existing->Bounds);
							}
						}
					}
				}
				if (const TSet<TWeakObjectPtr<AActor>>* Entries = PowerPoleCoverageGrid.Find(Cell))
				{
					for (const TWeakObjectPtr<AActor>& Key : *Entries)
					{
						if (!SeenPowerPoles.Contains(Key))
						{
							SeenPowerPoles.Add(Key);
							if (const FBox* Bounds = PowerPoleExclusionBounds.Find(Key);
								Bounds && Bounds->IsValid && Bounds->Intersect(QueryBounds))
							{
								OutBounds.Add(*Bounds);
							}
						}
					}
				}
			}
		}
	}
}

bool FNoGrassUnderBuildingsModule::IsLocationCovered(const FVector& Location) const
{
	const FIntVector Cell = GetCoverageGridCell(Location);
	if (const TSet<TWeakObjectPtr<AFGBuildable>>* Entries = BuildableCoverageGrid.Find(Cell))
	{
		for (const TWeakObjectPtr<AFGBuildable>& Key : *Entries)
		{
			if (CollisionFootprints.Contains(Key))
			{
				if (IsCollisionFootprintCovered(Key, Location))
				{
					return true;
				}
			}
			else if (const FBox* Bounds = ExclusionBounds.Find(Key);
				Bounds && Bounds->IsValid && Bounds->IsInsideOrOn(Location))
			{
				return true;
			}
		}
	}
	if (const TSet<TWeakObjectPtr<AFGBuildable>>* Entries = VehiclePathCoverageGrid.Find(Cell))
	{
		for (const TWeakObjectPtr<AFGBuildable>& Key : *Entries)
		{
			if (const FNoGrassVehiclePathExclusion* Exclusion = VehiclePathExclusions.Find(Key);
				Exclusion && IsVehiclePathCovered(*Exclusion, Location))
			{
				return true;
			}
		}
	}
	if (const TSet<FNoGrassLightweightKey>* Entries = LightweightCoverageGrid.Find(Cell))
	{
		for (const FNoGrassLightweightKey& Key : *Entries)
		{
			if (const FNoGrassLightweightExclusion* Existing = LightweightExclusions.Find(Key);
				Existing && Existing->Bounds.IsValid && Existing->Bounds.IsInsideOrOn(Location))
			{
				return true;
			}
		}
	}
	if (const TSet<TWeakObjectPtr<AActor>>* Entries = PowerPoleCoverageGrid.Find(Cell))
	{
		for (const TWeakObjectPtr<AActor>& Key : *Entries)
		{
			if (const FBox* Bounds = PowerPoleExclusionBounds.Find(Key);
				Bounds && Bounds->IsValid && Bounds->IsInsideOrOn(Location))
			{
				return true;
			}
		}
	}
	return false;
}

void FNoGrassUnderBuildingsModule::ScanLightweightBuildables(UWorld* World)
{
	AFGLightweightBuildableSubsystem* Subsystem = AFGLightweightBuildableSubsystem::Get(World);
	if (!IsValid(Subsystem))
	{
		return;
	}

	const auto& AllInstances = Subsystem->GetAllLightweightBuildableInstances();
	int32 ValidClassCount = 0;
	int32 ValidInstanceCount = 0;
	TArray<FString> FoundationClasses;
	TSet<FNoGrassLightweightKey> CurrentKeys;
	TArray<FBox> RefreshBounds;
	for (const auto& Pair : AllInstances)
	{
		int32 ValidForClass = 0;
		for (int32 RuntimeIndex = 0; RuntimeIndex < Pair.Value.Num(); ++RuntimeIndex)
		{
			const FRuntimeBuildableInstanceData& RuntimeData = Pair.Value[RuntimeIndex];
			if (RuntimeData.IsValidOnLoad())
			{
				++ValidForClass;
				const FNoGrassLightweightKey Key{Pair.Key.Get(), RuntimeIndex};
				CurrentKeys.Add(Key);
				const FNoGrassLightweightExclusion* Existing = LightweightExclusions.Find(Key);
				if (!Existing || !Existing->Transform.Equals(RuntimeData.Transform))
				{
					if (Existing)
					{
						RemoveLightweightExclusion(World, Key, RefreshBounds);
					}
					AddLightweightExclusion(
						World,
						Key,
						RuntimeData.Transform,
						RuntimeData.BoundingBox,
						RefreshBounds);
				}
			}
		}
		if (ValidForClass <= 0)
		{
			continue;
		}

		++ValidClassCount;
		ValidInstanceCount += ValidForClass;
		if (Pair.Key && Pair.Key->GetName().Contains(TEXT("Foundation"), ESearchCase::IgnoreCase))
		{
			FoundationClasses.Add(FString::Printf(TEXT("%s=%d"), *Pair.Key->GetName(), ValidForClass));
		}
	}

	TArray<FNoGrassLightweightKey> RemovedKeys;
	for (const auto& Pair : LightweightExclusions)
	{
		if (!CurrentKeys.Contains(Pair.Key))
		{
			RemovedKeys.Add(Pair.Key);
		}
	}
	for (const FNoGrassLightweightKey& Key : RemovedKeys)
	{
		RemoveLightweightExclusion(World, Key, RefreshBounds);
	}
	if (!RefreshBounds.IsEmpty())
	{
		RefreshLandscapeGrass(World, RefreshBounds);
		++CoverageRevision;
	}

	if (ValidClassCount != LastLightweightClassCount || ValidInstanceCount != LastLightweightInstanceCount)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Lightweight scan: classes=%d instances=%d foundation-classes=[%s]"),
			ValidClassCount,
			ValidInstanceCount,
			*FString::Join(FoundationClasses, TEXT(", ")));
		LastLightweightClassCount = ValidClassCount;
		LastLightweightInstanceCount = ValidInstanceCount;
	}
}

void FNoGrassUnderBuildingsModule::AddLightweightExclusion(
	UWorld* World,
	const FNoGrassLightweightKey& Key,
	const FTransform& Transform,
	const FBox& LocalBounds,
	TArray<FBox>& RefreshBounds)
{
	if (!World || !Key.BuildableClass)
	{
		return;
	}

	FBox WorldBounds(ForceInit);
	if (LocalBounds.IsValid)
	{
		WorldBounds = LocalBounds.TransformBy(Transform.ToMatrixWithScale());
	}
	else
	{
		// Foundation runtime bounds should normally be populated. This conservative
		// fallback covers a single 8 m buildable without clearing a broad area.
		const FVector Center = Transform.GetLocation();
		WorldBounds = FBox(Center - FVector(400.0f, 400.0f, 200.0f), Center + FVector(400.0f, 400.0f, 200.0f));
	}
	WorldBounds = WorldBounds.ExpandBy(FVector(100.0f, 100.0f, 200.0f));

	// Lightweight instances are not UObjects. Keep their coverage as plain data
	// and filter regenerated landscape/cliff grass through AcceptPrebuiltTree.
	// A transient UObject owner per instance can exhaust Unreal's global object
	// table on very large saves.
	LightweightExclusions.Add(Key, {WorldBounds, Transform});
	AddLightweightToCoverageGrid(Key, WorldBounds);
	RefreshBounds.Add(WorldBounds);
}

void FNoGrassUnderBuildingsModule::RemoveLightweightExclusion(
	UWorld* World,
	const FNoGrassLightweightKey& Key,
	TArray<FBox>& RefreshBounds)
{
	FNoGrassLightweightExclusion* Existing = LightweightExclusions.Find(Key);
	if (!Existing)
	{
		return;
	}

	if (World && Existing->Bounds.IsValid)
	{
		RefreshBounds.Add(Existing->Bounds);
	}
	RemoveLightweightFromCoverageGrid(Key, Existing->Bounds);
	LightweightExclusions.Remove(Key);
}

void FNoGrassUnderBuildingsModule::ClearLightweightExclusions(UWorld* World, bool bRefresh)
{
	TArray<FBox> RefreshBounds;
	TArray<FNoGrassLightweightKey> Keys;
	LightweightExclusions.GetKeys(Keys);
	for (const FNoGrassLightweightKey& Key : Keys)
	{
		RemoveLightweightExclusion(World, Key, RefreshBounds);
	}
	if (bRefresh && World && !RefreshBounds.IsEmpty())
	{
		RefreshLandscapeGrass(World, RefreshBounds);
	}
}

bool FNoGrassUnderBuildingsModule::BuildVehiclePathExclusion(
	AFGVehiclePathSegment* PathSegment,
	FNoGrassVehiclePathExclusion& OutExclusion) const
{
	if (!IsValid(PathSegment))
	{
		return false;
	}
	USplineComponent* Spline = PathSegment->GetSplineComponent();
	if (!IsValid(Spline))
	{
		return false;
	}

	const float SplineLength = Spline->GetSplineLength();
	if (!FMath::IsFinite(SplineLength) || SplineLength <= KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const FVector VehicleExtents = PathSegment->CalculateLargestAllowedVehicleExtents();
	const float VehicleHalfWidth = FMath::IsFinite(VehicleExtents.Y) && VehicleExtents.Y >= 50.0f
		? FMath::Abs(VehicleExtents.Y)
		: 250.0f;
	const float VehicleHalfHeight = FMath::IsFinite(VehicleExtents.Z) && VehicleExtents.Z >= 50.0f
		? FMath::Abs(VehicleExtents.Z)
		: 150.0f;
	OutExclusion.HalfWidth = FMath::Clamp(VehicleHalfWidth + 100.0f, 200.0f, 600.0f);
	OutExclusion.VerticalTolerance = FMath::Clamp(VehicleHalfHeight + 200.0f, 300.0f, 1000.0f);

	constexpr float DesiredSampleSpacing = 200.0f;
	constexpr int32 MaxSampleIntervals = 4096;
	const int32 IntervalCount = FMath::Clamp(
		FMath::CeilToInt(SplineLength / DesiredSampleSpacing),
		1,
		MaxSampleIntervals);
	OutExclusion.SamplePoints.Reserve(IntervalCount + 1);
	OutExclusion.SegmentBounds.Reserve(IntervalCount);
	for (int32 Index = 0; Index <= IntervalCount; ++Index)
	{
		const float Distance = SplineLength * static_cast<float>(Index) /
			static_cast<float>(IntervalCount);
		const FVector Point = Spline->GetLocationAtDistanceAlongSpline(
			Distance,
			ESplineCoordinateSpace::World);
		if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y) || !FMath::IsFinite(Point.Z))
		{
			return false;
		}
		OutExclusion.SamplePoints.Add(Point);
		if (Index > 0)
		{
			FBox SegmentBounds(ForceInit);
			SegmentBounds += OutExclusion.SamplePoints[Index - 1];
			SegmentBounds += Point;
			OutExclusion.SegmentBounds.Add(SegmentBounds.ExpandBy(FVector(
				OutExclusion.HalfWidth,
				OutExclusion.HalfWidth,
				OutExclusion.VerticalTolerance)));
		}
	}
	return OutExclusion.SamplePoints.Num() >= 2 && !OutExclusion.SegmentBounds.IsEmpty();
}

bool FNoGrassUnderBuildingsModule::IsVehiclePathCovered(
	const FNoGrassVehiclePathExclusion& Exclusion,
	const FVector& Location) const
{
	const float WidthSquared = FMath::Square(Exclusion.HalfWidth);
	for (int32 Index = 1; Index < Exclusion.SamplePoints.Num(); ++Index)
	{
		const FVector& Start = Exclusion.SamplePoints[Index - 1];
		const FVector& End = Exclusion.SamplePoints[Index];
		const FVector2D StartXY(Start.X, Start.Y);
		const FVector2D EndXY(End.X, End.Y);
		const FVector2D LocationXY(Location.X, Location.Y);
		const FVector2D Delta = EndXY - StartXY;
		const float LengthSquared = Delta.SizeSquared();
		const float Alpha = LengthSquared > SMALL_NUMBER
			? FMath::Clamp(FVector2D::DotProduct(LocationXY - StartXY, Delta) / LengthSquared, 0.0f, 1.0f)
			: 0.0f;
		const FVector2D ClosestXY = StartXY + Delta * Alpha;
		const float ClosestZ = FMath::Lerp(Start.Z, End.Z, Alpha);
		if (FVector2D::DistSquared(LocationXY, ClosestXY) <= WidthSquared &&
			FMath::Abs(Location.Z - ClosestZ) <= Exclusion.VerticalTolerance)
		{
			return true;
		}
	}
	return false;
}

void FNoGrassUnderBuildingsModule::AddVehiclePathExclusion(
	AFGVehiclePathSegment* PathSegment,
	bool bRefresh)
{
	if (!bVehiclePathsEnabled || !IsValid(PathSegment))
	{
		return;
	}
	const TWeakObjectPtr<AFGBuildable> Key(PathSegment);
	if (VehiclePathExclusions.Contains(Key))
	{
		return;
	}

	FNoGrassVehiclePathExclusion Exclusion;
	if (!BuildVehiclePathExclusion(PathSegment, Exclusion))
	{
		return;
	}
	AddVehiclePathToCoverageGrid(Key, Exclusion.SegmentBounds);
	VehiclePathExclusions.Add(Key, MoveTemp(Exclusion));
	if (bRefresh)
	{
		if (const FNoGrassVehiclePathExclusion* Added = VehiclePathExclusions.Find(Key))
		{
			for (const FBox& Bounds : Added->SegmentBounds)
			{
				QueueCoverageRefresh(Bounds, TEXT("vehicle-path-added"));
			}
		}
	}
}

void FNoGrassUnderBuildingsModule::ApplyVehiclePathSetting(UWorld* World, bool bEnabled)
{
	if (!World || World != ActiveGameWorld.Get() || bEnabled == bVehiclePathsEnabled)
	{
		return;
	}

	bVehiclePathsEnabled = bEnabled;
	if (bEnabled)
	{
		int32 Added = 0;
		for (TActorIterator<AFGVehiclePathSegment> It(World); It; ++It)
		{
			AFGVehiclePathSegment* PathSegment = *It;
			if (!IsValid(PathSegment)) continue;
			const int32 PreviousCount = VehiclePathExclusions.Num();
			AddVehiclePathExclusion(PathSegment, true);
			Added += VehiclePathExclusions.Num() > PreviousCount ? 1 : 0;
		}
		UE_LOG(LogNoGrassUnderBuildings, Display,
			TEXT("Vehicle path foliage enabled: tracked=%d"), Added);
		return;
	}

	TArray<TWeakObjectPtr<AFGBuildable>> Paths;
	VehiclePathExclusions.GetKeys(Paths);
	for (const TWeakObjectPtr<AFGBuildable>& Path : Paths)
	{
		RemoveVehiclePathExclusion(Path, true);
	}
	UE_LOG(LogNoGrassUnderBuildings, Display,
		TEXT("Vehicle path foliage disabled: restored-routes=%d"), Paths.Num());
}

void FNoGrassUnderBuildingsModule::RemoveVehiclePathExclusion(
	const TWeakObjectPtr<AFGBuildable>& PathSegment,
	bool bRefresh)
{
	const FNoGrassVehiclePathExclusion* Existing = VehiclePathExclusions.Find(PathSegment);
	if (!Existing)
	{
		return;
	}
	const TArray<FBox> PreviousBounds = Existing->SegmentBounds;
	RemoveVehiclePathFromCoverageGrid(PathSegment, PreviousBounds);
	VehiclePathExclusions.Remove(PathSegment);
	if (bRefresh)
	{
		for (const FBox& Bounds : PreviousBounds)
		{
			QueueCoverageRefresh(Bounds, TEXT("vehicle-path-removed"));
		}
	}
}

void FNoGrassUnderBuildingsModule::ScanBuildables(UWorld* World)
{
	TSet<TWeakObjectPtr<AFGBuildable>> CurrentBuildables;
	for (TActorIterator<AFGBuildable> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			CurrentBuildables.Add(*It);
		}
	}

	TArray<FBox> InitialRefreshBounds;
	int32 Added = 0;
	for (const TWeakObjectPtr<AFGBuildable>& Buildable : CurrentBuildables)
	{
		if (!KnownBuildables.Contains(Buildable))
		{
			++Added;
			if (Buildable.IsValid())
			{
				const bool bRefreshImmediately = bInitialBuildableScanComplete;
				AddLandscapeExclusion(Buildable.Get(), bRefreshImmediately);
				if (!bRefreshImmediately)
				{
					if (const FNoGrassVehiclePathExclusion* PathExclusion = VehiclePathExclusions.Find(Buildable))
					{
						InitialRefreshBounds.Append(PathExclusion->SegmentBounds);
					}
					else if (const FBox* Bounds = ExclusionBounds.Find(Buildable))
					{
						InitialRefreshBounds.Add(*Bounds);
					}
				}
			}
		}
	}

	int32 Removed = 0;
	for (const TWeakObjectPtr<AFGBuildable>& Buildable : KnownBuildables)
	{
		if (!CurrentBuildables.Contains(Buildable))
		{
			++Removed;
			if (VehiclePathExclusions.Contains(Buildable))
			{
				RemoveVehiclePathExclusion(Buildable);
			}
			else if (ExcludedBuildables.Contains(Buildable))
			{
				RemoveLandscapeExclusion(Buildable);
			}
		}
	}

	KnownBuildables = MoveTemp(CurrentBuildables);
	if (!bInitialBuildableScanComplete && !InitialRefreshBounds.IsEmpty())
	{
		RefreshLandscapeGrass(World, InitialRefreshBounds);
	}
	bInitialBuildableScanComplete = true;
	if (Added > 0 || Removed > 0)
	{
		++CoverageRevision;
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Buildable scan: added=%d removed=%d tracked=%d"),
			Added,
			Removed,
			KnownBuildables.Num());
	}
}

FBox FNoGrassUnderBuildingsModule::GetLandscapeExclusionBounds(AFGBuildable* Buildable) const
{
	if (!IsValid(Buildable))
	{
		return FBox(ForceInit);
	}

	FBox RawBounds = Buildable->GetComponentsBoundingBox(true);
	const bool bSpaceElevator = Buildable->GetClass()->GetName() == TEXT("Build_SpaceElevator_C");
	if (bSpaceElevator)
	{
		static const FName MainMeshName(TEXT("MainMesh"));
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Buildable);
		for (UPrimitiveComponent* Component : PrimitiveComponents)
		{
			if (!IsValid(Component) || Component->GetFName() != MainMeshName)
			{
				continue;
			}
			const FBox MainMeshBounds = Component->Bounds.GetBox();
			if (MainMeshBounds.IsValid)
			{
				RawBounds = MainMeshBounds;
			}
			break;
		}
	}
	else if (!IsUsableComponentBounds(RawBounds, Buildable->GetActorLocation()))
	{
		FBox SanitizedBounds(ForceInit);
		int32 AcceptedComponents = 0;
		int32 RejectedComponents = 0;
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Buildable);
		for (UPrimitiveComponent* Component : PrimitiveComponents)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			const FBox ComponentBounds = Component->Bounds.GetBox();
			if (IsUsableComponentBounds(ComponentBounds, Buildable->GetActorLocation()))
			{
				SanitizedBounds += ComponentBounds;
				++AcceptedComponents;
			}
			else
			{
				++RejectedComponents;
			}
		}

		if (!IsFiniteBox(SanitizedBounds))
		{
			UE_LOG(LogNoGrassUnderBuildings, Verbose,
				TEXT("Ignored buildable with unusable runtime bounds: class=%s min=%s max=%s rejected-components=%d"),
				*GetNameSafe(Buildable->GetClass()), *RawBounds.Min.ToString(), *RawBounds.Max.ToString(), RejectedComponents);
			return FBox(ForceInit);
		}

		UE_LOG(LogNoGrassUnderBuildings, Verbose,
			TEXT("Sanitized abnormal runtime bounds: class=%s raw-min=%s raw-max=%s accepted-components=%d rejected-components=%d"),
			*GetNameSafe(Buildable->GetClass()), *RawBounds.Min.ToString(), *RawBounds.Max.ToString(),
			AcceptedComponents, RejectedComponents);
		RawBounds = SanitizedBounds;
	}

	if (!IsFiniteBox(RawBounds))
	{
		return FBox(ForceInit);
	}
	// The precomputed physical mask includes the visible reach of nearby grass.
	// Keep its world-space query bounds large enough for those outer cells.
	return RawBounds.ExpandBy(
		bSpaceElevator
			? FVector(200.0f, 200.0f, 200.0f)
			: FVector(100.0f, 100.0f, 200.0f));
}

bool FNoGrassUnderBuildingsModule::IsCollisionFootprintCovered(
	const TWeakObjectPtr<AFGBuildable>& Buildable,
	const FVector& Location) const
{
	const FNoGrassCollisionFootprint* Footprint = CollisionFootprints.Find(Buildable);
	if (!Footprint || !Footprint->Bounds.IsInsideXY(Location))
	{
		return false;
	}

	const FVector LocalLocation = Footprint->ComponentTransform.InverseTransformPosition(Location);
	const int32 LocalCellX =
		FMath::FloorToInt(LocalLocation.X / CollisionFootprintCellSize) -
		GNoGrassSpaceElevatorMinCellX;
	const int32 LocalCellY =
		FMath::FloorToInt(LocalLocation.Y / CollisionFootprintCellSize) -
		GNoGrassSpaceElevatorMinCellY;
	if (LocalCellX < 0 || LocalCellX >= GNoGrassSpaceElevatorCellCountX ||
		LocalCellY < 0 || LocalCellY >= GNoGrassSpaceElevatorCellCountY)
	{
		return false;
	}

	const FNoGrassSpaceElevatorFootprintRow& Row = GNoGrassSpaceElevatorRows[LocalCellY];
	return (LocalCellX >= Row.MinA && LocalCellX <= Row.MaxA) ||
		(Row.MinB != GNoGrassNoRun && LocalCellX >= Row.MinB && LocalCellX <= Row.MaxB);
}

void FNoGrassUnderBuildingsModule::AddLandscapeExclusion(
	AFGBuildable* Buildable,
	bool bRefresh)
{
	if (!IsValid(Buildable))
	{
		return;
	}
	if (AFGVehiclePathSegment* PathSegment = Cast<AFGVehiclePathSegment>(Buildable))
	{
		AddVehiclePathExclusion(PathSegment, bRefresh);
		return;
	}

	const FBox Bounds = GetLandscapeExclusionBounds(Buildable);
	if (!Bounds.IsValid)
	{
		return;
	}

	const TWeakObjectPtr<AFGBuildable> Owner(Buildable);
	UPrimitiveComponent* CollisionComponent = nullptr;
	if (Buildable->GetClass()->GetName() == TEXT("Build_SpaceElevator_C"))
	{
		static const FName MainMeshName(TEXT("MainMesh"));
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Buildable);
		for (UPrimitiveComponent* Component : PrimitiveComponents)
		{
			if (IsValid(Component) && Component->GetFName() == MainMeshName)
			{
				CollisionComponent = Component;
				break;
			}
		}
	}

	if (CollisionComponent)
	{
		FNoGrassCollisionFootprint Footprint;
		Footprint.Bounds = Bounds;
		Footprint.ComponentTransform = CollisionComponent->GetComponentTransform();
		CollisionFootprints.Add(Owner, Footprint);
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Precomputed physical footprint ready: class=%s occupied=%d"),
			*GetNameSafe(Buildable->GetClass()),
			GNoGrassSpaceElevatorOccupiedCellCount);
	}
	else
	{
		ALandscapeProxy::AddExclusionBox(FWeakObjectPtr(Buildable), Bounds);
	}
	ExcludedBuildables.Add(Owner);
	ExclusionBounds.Add(Owner, Bounds);
	AddBuildableToCoverageGrid(Owner, Bounds);
	if (bRefresh)
	{
		RefreshLandscapeGrass(Buildable->GetWorld(), Bounds);
	}
}

void FNoGrassUnderBuildingsModule::RemoveLandscapeExclusion(
	const TWeakObjectPtr<AFGBuildable>& Buildable,
	bool bRefresh)
{
	if (VehiclePathExclusions.Contains(Buildable))
	{
		RemoveVehiclePathExclusion(Buildable, bRefresh);
		return;
	}
	UWorld* World = ActiveGameWorld.Get();
	FBox PreviousBounds(ForceInit);
	if (const FBox* StoredBounds = ExclusionBounds.Find(Buildable))
	{
		PreviousBounds = *StoredBounds;
	}

	if (!CollisionFootprints.Contains(Buildable))
	{
		ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Buildable));
	}
	RemoveBuildableFromCoverageGrid(Buildable, PreviousBounds);
	ExcludedBuildables.Remove(Buildable);
	ExclusionBounds.Remove(Buildable);
	CollisionFootprints.Remove(Buildable);
	if (bRefresh && World && PreviousBounds.IsValid)
	{
		RefreshLandscapeGrass(World, PreviousBounds);
	}
}

void FNoGrassUnderBuildingsModule::RefreshLandscapeGrass(
	UWorld* World,
	const FBox& ChangedBounds)
{
	TArray<FBox> Bounds;
	Bounds.Add(ChangedBounds);
	RefreshLandscapeGrass(World, Bounds);
}

void FNoGrassUnderBuildingsModule::RefreshLandscapeGrass(
	UWorld* World,
	const TArray<FBox>& ChangedBounds)
{
	if (!World || ChangedBounds.IsEmpty())
	{
		return;
	}

	RefreshCliffGrass(World, ChangedBounds);

	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Proxy = *It;
		if (!IsValid(Proxy))
		{
			continue;
		}

		TSet<ULandscapeComponent*> Components;
		for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
		{
			if (!IsValid(Component))
			{
				continue;
			}

			const FBox ComponentBounds = Component->Bounds.GetBox();
			for (const FBox& Bounds : ChangedBounds)
			{
				if (Bounds.IsValid && ComponentBounds.Intersect(Bounds))
				{
					Components.Add(Component);
					break;
				}
			}
		}

		if (!Components.IsEmpty())
		{
			Proxy->FlushGrassComponents(&Components, false);
		}
	}
}

void FNoGrassUnderBuildingsModule::RefreshCliffGrass(
	UWorld* World,
	const TArray<FBox>& ChangedBounds)
{
	if (!World || ChangedBounds.IsEmpty())
	{
		return;
	}

	for (TActorIterator<AFGCliffActor> It(World); It; ++It)
	{
		AFGCliffActor* Cliff = *It;
		if (!IsValid(Cliff) || !Cliff->IsSignificant())
		{
			continue;
		}

		const FBox CliffBounds = Cliff->GetComponentsBoundingBox(true);
		bool bIntersectsChange = false;
		for (const FBox& Bounds : ChangedBounds)
		{
			if (Bounds.IsValid && CliffBounds.Intersect(Bounds))
			{
				bIntersectsChange = true;
				break;
			}
		}

		if (!bIntersectsChange)
		{
			continue;
		}

		const TWeakObjectPtr<AFGCliffActor> WeakCliff(Cliff);
		IFGSignificanceInterface::Execute_LostSignificance(Cliff);
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateLambda([WeakCliff]()
			{
				if (AFGCliffActor* ValidCliff = WeakCliff.Get())
				{
					IFGSignificanceInterface::Execute_GainedSignificance(ValidCliff);
				}
			}));
	}
}

IMPLEMENT_MODULE(FNoGrassUnderBuildingsModule, NoGrassUnderBuildings)
