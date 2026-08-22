#include "NoGrassUnderBuildings.h"
#include "NoGrassLightweightExclusionToken.h"

#include "Buildables/FGBuildable.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGCliffActor.h"
#include "FGFoliageInstancedSMC.h"
#include "GameFramework/PlayerController.h"
#include "GrassInstancedStaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Patching/NativeHookManager.h"
#include "RenderTransform.h"
#include "StaticMeshResources.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"

DEFINE_LOG_CATEGORY(LogNoGrassUnderBuildings);

void FNoGrassUnderBuildingsModule::StartupModule()
{
	ScanNearbyCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("NoGrassUnderBuildings.ScanNearby"),
		TEXT("Writes nearby instanced-foliage mesh diagnostics. Optional radius in meters."),
		FConsoleCommandWithArgsDelegate::CreateRaw(
			this,
			&FNoGrassUnderBuildingsModule::ScanNearbyFoliage),
		ECVF_Default);
	ArmCliffTraceCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("NoGrassUnderBuildings.TraceNextCliff"),
		TEXT("Captures the next nearby cliff-grass render upload once, then disarms. Optional radius in meters."),
		FConsoleCommandWithArgsDelegate::CreateRaw(
			this,
			&FNoGrassUnderBuildingsModule::ArmCliffTrace),
		ECVF_Default);
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
			TraceCliffGrassUpload(
				Component,
				ClusterTree,
				OcclusionLayerNum,
				NumBuiltRenderInstances,
				InstanceData);
			FilterCliffGrassUpload(Component, InstanceData);
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
	if (ScanNearbyCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ScanNearbyCommand);
		ScanNearbyCommand = nullptr;
	}
	if (ArmCliffTraceCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ArmCliffTraceCommand);
		ArmCliffTraceCommand = nullptr;
	}
	bCliffTraceArmed = false;
	FWorldDelegates::OnPostWorldInitialization.Remove(PostWorldInitializationHandle);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedToWorldHandle);
	PostWorldInitializationHandle.Reset();
	WorldCleanupHandle.Reset();
	WorldPostActorTickHandle.Reset();
	LevelAddedToWorldHandle.Reset();
	RestoreAllDecorativeFoliage();
	ClearLightweightExclusions(ActiveGameWorld.Get(), false);
	ActiveGameWorld.Reset();
	KnownBuildables.Empty();
	ExcludedBuildables.Empty();
	ExclusionBounds.Empty();
	LastLightweightClassCount = INDEX_NONE;
	LastLightweightInstanceCount = INDEX_NONE;
	PendingRefreshBounds.Empty();
	PendingCoverageEventCount = 0;
	PendingCoverageQueuedAt = 0.0;
	PendingStreamedLevels.Empty();
	UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("No Grass Under Buildings unloaded"));
}

int32 FNoGrassUnderBuildingsModule::FilterCliffGrassUpload(
	UGrassInstancedStaticMeshComponent* Component,
	FStaticMeshInstanceData* InstanceData)
{
	// The coverage maps are maintained on the game thread. Do not touch them
	// from an asynchronous render/grass task.
	if (!IsInGameThread() || !IsValid(Component) || !InstanceData)
	{
		return 0;
	}

	AFGCliffActor* Cliff = Component->GetTypedOuter<AFGCliffActor>();
	UWorld* World = Component->GetWorld();
	if (!IsValid(Cliff) || !IsValid(World) || World != ActiveGameWorld.Get())
	{
		return 0;
	}

	TArray<FBox, TInlineAllocator<32>> CoverageBounds;
	for (const auto& Pair : ExclusionBounds)
	{
		if (Pair.Key.IsValid() && Pair.Value.IsValid)
		{
			CoverageBounds.Add(Pair.Value);
		}
	}
	for (const auto& Pair : LightweightExclusions)
	{
		if (Pair.Value.Bounds.IsValid)
		{
			CoverageBounds.Add(Pair.Value.Bounds);
		}
	}
	if (CoverageBounds.IsEmpty())
	{
		return 0;
	}

	const FTransform ComponentToWorld = Component->GetComponentTransform();
	int32 HiddenCount = 0;
	for (int32 InstanceIndex = 0;
		InstanceIndex < InstanceData->GetNumInstances();
		++InstanceIndex)
	{
		FRenderTransform PackedTransform;
		InstanceData->GetInstanceTransform(InstanceIndex, PackedTransform);
		const FVector WorldRoot =
			(FTransform(PackedTransform.ToMatrix()) * ComponentToWorld).GetLocation();

		for (const FBox& Bounds : CoverageBounds)
		{
			if (Bounds.IsInsideOrOn(WorldRoot))
			{
				// Keep the buffer length and cluster indices unchanged. Unreal marks
				// only this instance invisible when it accepts the existing tree.
				InstanceData->NullifyInstance(InstanceIndex);
				++HiddenCount;
				break;
			}
		}
	}

	return HiddenCount;
}

void FNoGrassUnderBuildingsModule::ArmCliffTrace(const TArray<FString>& Args)
{
	UWorld* World = ActiveGameWorld.Get();
	if (!World)
	{
		return;
	}

	float RadiusMeters = 30.0f;
	if (!Args.IsEmpty())
	{
		RadiusMeters = FMath::Clamp(FCString::Atof(*Args[0]), 5.0f, 100.0f);
	}
	FRotator ViewRotation = FRotator::ZeroRotator;
	if (APlayerController* Controller = World->GetFirstPlayerController())
	{
		Controller->GetPlayerViewPoint(CliffTraceCenter, ViewRotation);
	}
	CliffTraceRadiusSquared = FMath::Square(RadiusMeters * 100.0f);
	bCliffTraceArmed = true;

	const FString OutputPath = FPaths::Combine(
		FPaths::ProjectLogDir(),
		TEXT("NoGrassUnderBuildings-CliffTrace.txt"));
	TArray<FString> Lines;
	Lines.Add(FString::Printf(
		TEXT("Cliff trace armed; center=%s; radius=%.1fm; waiting for one generated-grass upload"),
		*CliffTraceCenter.ToCompactString(),
		RadiusMeters));
	FFileHelper::SaveStringArrayToFile(Lines, *OutputPath);
	UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("Cliff trace armed once: %s"), *OutputPath);
}

void FNoGrassUnderBuildingsModule::TraceCliffGrassUpload(
	UGrassInstancedStaticMeshComponent* Component,
	const TArray<FClusterNode>& ClusterTree,
	int32 OcclusionLayerNum,
	int32 NumBuiltRenderInstances,
	const FStaticMeshInstanceData* InstanceData)
{
	if ((!bCliffTraceArmed && !bAutoCliffTracePending) || !IsValid(Component) || !InstanceData)
	{
		return;
	}
	AFGCliffActor* Cliff = Component->GetTypedOuter<AFGCliffActor>();
	UWorld* ComponentWorld = Component->GetWorld();
	if (!IsValid(Cliff) || !IsValid(ComponentWorld) || !ComponentWorld->IsGameWorld())
	{
		return;
	}
	const bool bManualTrace = bCliffTraceArmed;
	const FBox CliffBounds = Cliff->GetComponentsBoundingBox(true);
	if (bManualTrace && CliffBounds.ComputeSquaredDistanceToPoint(CliffTraceCenter) > CliffTraceRadiusSquared)
	{
		return;
	}

	// One matching upload is enough. Disarm before doing file work so a nested or
	// concurrent upload cannot produce a flood of diagnostic output.
	bCliffTraceArmed = false;
	bAutoCliffTracePending = false;
	const UStaticMesh* Mesh = Component->GetStaticMesh();
	TArray<FString> Lines;
	Lines.Add(TEXT("No Grass Under Buildings - one-shot cliff generation trace"));
	Lines.Add(FString::Printf(TEXT("capture-mode=%s"), bManualTrace ? TEXT("manual-nearby") : TEXT("automatic-first-world-upload")));
	Lines.Add(FString::Printf(TEXT("game-thread=%s"), IsInGameThread() ? TEXT("true") : TEXT("false")));
	Lines.Add(FString::Printf(TEXT("cliff=%s"), *Cliff->GetPathName()));
	Lines.Add(FString::Printf(TEXT("component=%s"), *Component->GetPathName()));
	Lines.Add(FString::Printf(TEXT("mesh=%s"), Mesh ? *Mesh->GetPathName() : TEXT("<none>")));
	Lines.Add(FString::Printf(TEXT("cliff-significant=%s"), Cliff->IsSignificant() ? TEXT("true") : TEXT("false")));
	Lines.Add(FString::Printf(TEXT("component-registered=%s"), Component->IsRegistered() ? TEXT("true") : TEXT("false")));
	Lines.Add(FString::Printf(TEXT("cpu-component-instances-before-upload=%d"), Component->GetInstanceCount()));
	Lines.Add(FString::Printf(TEXT("incoming-buffer-instances=%d"), InstanceData->GetNumInstances()));
	Lines.Add(FString::Printf(TEXT("declared-render-instances=%d"), NumBuiltRenderInstances));
	Lines.Add(FString::Printf(TEXT("cluster-nodes=%d"), ClusterTree.Num()));
	Lines.Add(FString::Printf(TEXT("occlusion-layer-nodes=%d"), OcclusionLayerNum));
	Lines.Add(FString::Printf(TEXT("cliff-bounds=%s"), *CliffBounds.ToString()));
	Lines.Add(TEXT("result=upload observed without modifying the buffer"));

	const FString OutputPath = FPaths::Combine(
		FPaths::ProjectLogDir(),
		TEXT("NoGrassUnderBuildings-CliffTrace.txt"));
	FFileHelper::SaveStringArrayToFile(Lines, *OutputPath);
	UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("Captured one cliff-grass upload and disarmed: %s"), *OutputPath);
}

void FNoGrassUnderBuildingsModule::ScanNearbyFoliage(const TArray<FString>& Args)
{
	UWorld* World = ActiveGameWorld.Get();
	if (!World)
	{
		return;
	}

	float RadiusMeters = 10.0f;
	if (!Args.IsEmpty())
	{
		RadiusMeters = FMath::Clamp(FCString::Atof(*Args[0]), 1.0f, 50.0f);
	}
	const float RadiusCm = RadiusMeters * 100.0f;
	const float RadiusSquared = FMath::Square(RadiusCm);

	FVector Center = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	if (APlayerController* Controller = World->GetFirstPlayerController())
	{
		Controller->GetPlayerViewPoint(Center, ViewRotation);
	}

	TArray<FString> Lines;
	Lines.Add(FString::Printf(
		TEXT("No Grass Under Buildings foliage scan; center=%s; radius=%.1fm"),
		*Center.ToCompactString(),
		RadiusMeters));
	int32 ComponentsMatched = 0;
	int32 InstancesMatched = 0;
	int32 CliffComponentsMatched = 0;
	for (TObjectIterator<UGrassInstancedStaticMeshComponent> It; It; ++It)
	{
		UGrassInstancedStaticMeshComponent* Component = *It;
		AFGCliffActor* Cliff = IsValid(Component)
			? Component->GetTypedOuter<AFGCliffActor>()
			: nullptr;
		if (!IsValid(Cliff) || Component->GetWorld() != World ||
			!Component->IsRegistered() ||
			Component->Bounds.GetBox().ComputeSquaredDistanceToPoint(Center) > RadiusSquared)
		{
			continue;
		}

		++CliffComponentsMatched;
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		Lines.Add(FString::Printf(
			TEXT("cliff-render-instances=%d component=%s owner=%s mesh=%s"),
			Component->GetNumRenderInstances(),
			*Component->GetPathName(),
			*Cliff->GetPathName(),
			Mesh ? *Mesh->GetPathName() : TEXT("<none>")));
	}
	for (TObjectIterator<UHierarchicalInstancedStaticMeshComponent> It; It; ++It)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = *It;
		if (!IsValid(Component) || Component->GetWorld() != World || !Component->IsRegistered())
		{
			continue;
		}

		int32 NearbyInstances = 0;
		FTransform InstanceTransform;
		for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
		{
			if (Component->GetInstanceTransform(Index, InstanceTransform, true) &&
				FVector::DistSquared(InstanceTransform.GetLocation(), Center) <= RadiusSquared)
			{
				++NearbyInstances;
			}
		}
		if (NearbyInstances <= 0)
		{
			continue;
		}

		++ComponentsMatched;
		InstancesMatched += NearbyInstances;
		const UStaticMesh* Mesh = Component->GetStaticMesh();
		Lines.Add(FString::Printf(
			TEXT("instances=%d component=%s owner=%s mesh=%s"),
			NearbyInstances,
			*Component->GetPathName(),
			Component->GetOwner() ? *Component->GetOwner()->GetPathName() : TEXT("<none>"),
			Mesh ? *Mesh->GetPathName() : TEXT("<none>")));
	}
	Lines.Add(FString::Printf(
		TEXT("summary: components=%d instances=%d cliff-components=%d"),
		ComponentsMatched,
		InstancesMatched,
		CliffComponentsMatched));

	const FString OutputPath = FPaths::Combine(
		FPaths::ProjectLogDir(),
		TEXT("NoGrassUnderBuildings-Scan.txt"));
	FFileHelper::SaveStringArrayToFile(Lines, *OutputPath);
	UE_LOG(
		LogNoGrassUnderBuildings,
		Display,
		TEXT("Nearby foliage scan complete: components=%d instances=%d file=%s"),
		ComponentsMatched,
		InstancesMatched,
		*OutputPath);
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
		MeshPath.Contains(TEXT("/SmallFoliage/LowerVegatation/SM_Plant_07."), ESearchCase::IgnoreCase);
}

void FNoGrassUnderBuildingsModule::ReconcileDecorativeFoliage(
	UWorld* World,
	const TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>>* ComponentFilter)
{
	if (!World)
	{
		return;
	}

	TArray<FBox> CoverageBounds;
	ExclusionBounds.GenerateValueArray(CoverageBounds);
	for (const auto& Pair : LightweightExclusions)
	{
		if (Pair.Value.Bounds.IsValid)
		{
			CoverageBounds.Add(Pair.Value.Bounds);
		}
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
		for (const FBox& Bounds : CoverageBounds)
		{
			if (!Bounds.IsValid || !ComponentBounds.Intersect(Bounds))
			{
				continue;
			}
			for (const int32 InstanceIndex : Component->GetInstancesOverlappingBox(Bounds, true))
			{
				DesiredSuppression.Add({Component, InstanceIndex});
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
		const FVector OriginalLocation = Pair.Value.GetLocation();
		for (const FBox& Bounds : CoverageBounds)
		{
			if (Bounds.IsValid && Bounds.IsInsideOrOn(OriginalLocation))
			{
				DesiredSuppression.Add(Pair.Key);
				break;
			}
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
		UE_LOG(
			LogNoGrassUnderBuildings,
			Verbose,
			TEXT("Reconciled %d decorative foliage component(s) from %d streamed level(s) in %.2f ms"),
			Components.Num(),
			StreamedLevels.Num(),
			(FPlatformTime::Seconds() - StartedAt) * 1000.0);
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
			RestoreAllDecorativeFoliage();
			ClearLightweightExclusions(PreviousWorld, false);
		}
		ActiveGameWorld = World;
		KnownBuildables.Empty();
		ExcludedBuildables.Empty();
		ExclusionBounds.Empty();
		NextBuildableScanAt = 0.0;
		bInitialBuildableScanComplete = false;
		LastLightweightClassCount = INDEX_NONE;
		LastLightweightInstanceCount = INDEX_NONE;
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
		RestoreAllDecorativeFoliage();
		ClearLightweightExclusions(World, false);
		ActiveGameWorld.Reset();
		KnownBuildables.Empty();
		ExcludedBuildables.Empty();
		ExclusionBounds.Empty();
		NextBuildableScanAt = 0.0;
		bInitialBuildableScanComplete = false;
		LastLightweightClassCount = INDEX_NONE;
		LastLightweightInstanceCount = INDEX_NONE;
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
	if (!bInitialBuildableScanComplete && Now >= NextBuildableScanAt)
	{
		ScanBuildables(World);
		ScanLightweightBuildables(World);
		if (AppliedFoliageRevision != CoverageRevision)
		{
			ReconcileDecorativeFoliage(World);
			AppliedFoliageRevision = CoverageRevision;
		}
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Initial coverage scan complete; event-driven tracking active"));
	}

	ProcessPendingCoverageRefresh(World);
	ProcessPendingStreamedFoliage(World);
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

	const TWeakObjectPtr<AFGBuildable> Key(Buildable);
	if (ExclusionBounds.Contains(Key))
	{
		return;
	}

	const FBox Bounds = Buildable->GetComponentsBoundingBox(true).ExpandBy(
		FVector(100.0f, 100.0f, 200.0f));
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
	for (const auto& Pair : ExclusionBounds)
	{
		const FBox& Candidate = Pair.Value;
		if (Candidate.IsValid && Candidate.Max.X > Bounds.Min.X && Candidate.Min.X < Bounds.Max.X &&
			Candidate.Max.Y > Bounds.Min.Y && Candidate.Min.Y < Bounds.Max.Y)
		{
			Candidates.Add(Candidate);
		}
	}
	for (const auto& Pair : LightweightExclusions)
	{
		const FBox& Candidate = Pair.Value.Bounds;
		if (Candidate.IsValid && Candidate.Max.X > Bounds.Min.X && Candidate.Min.X < Bounds.Max.X &&
			Candidate.Max.Y > Bounds.Min.Y && Candidate.Min.Y < Bounds.Max.Y)
		{
			Candidates.Add(Candidate);
		}
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

	UNoGrassLightweightExclusionToken* Token =
		NewObject<UNoGrassLightweightExclusionToken>(GetTransientPackage());
	if (!IsValid(Token))
	{
		return;
	}
	Token->AddToRoot();
	ALandscapeProxy::AddExclusionBox(FWeakObjectPtr(Token), WorldBounds);
	LightweightExclusions.Add(Key, {Token, WorldBounds, Transform});
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

	if (IsValid(Existing->Token))
	{
		ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Existing->Token));
		Existing->Token->RemoveFromRoot();
		Existing->Token->MarkAsGarbage();
	}
	if (World && Existing->Bounds.IsValid)
	{
		RefreshBounds.Add(Existing->Bounds);
	}
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
					if (const FBox* Bounds = ExclusionBounds.Find(Buildable))
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
			if (ExcludedBuildables.Contains(Buildable))
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

void FNoGrassUnderBuildingsModule::AddLandscapeExclusion(
	AFGBuildable* Buildable,
	bool bRefresh)
{
	if (!IsValid(Buildable))
	{
		return;
	}

	const FBox Bounds = Buildable->GetComponentsBoundingBox(true).ExpandBy(
		FVector(100.0f, 100.0f, 200.0f));
	if (!Bounds.IsValid)
	{
		return;
	}

	const TWeakObjectPtr<AFGBuildable> Owner(Buildable);
	ALandscapeProxy::AddExclusionBox(FWeakObjectPtr(Buildable), Bounds);
	ExcludedBuildables.Add(Owner);
	ExclusionBounds.Add(Owner, Bounds);
	if (bRefresh)
	{
		RefreshLandscapeGrass(Buildable->GetWorld(), Bounds);
	}
}

void FNoGrassUnderBuildingsModule::RemoveLandscapeExclusion(
	const TWeakObjectPtr<AFGBuildable>& Buildable,
	bool bRefresh)
{
	UWorld* World = ActiveGameWorld.Get();
	FBox PreviousBounds(ForceInit);
	if (const FBox* StoredBounds = ExclusionBounds.Find(Buildable))
	{
		PreviousBounds = *StoredBounds;
	}

	ALandscapeProxy::RemoveExclusionBox(FWeakObjectPtr(Buildable));
	ExcludedBuildables.Remove(Buildable);
	ExclusionBounds.Remove(Buildable);
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
