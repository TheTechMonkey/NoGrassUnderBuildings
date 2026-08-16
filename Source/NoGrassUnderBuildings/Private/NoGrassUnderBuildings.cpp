#include "NoGrassUnderBuildings.h"
#include "NoGrassLightweightExclusionToken.h"

#include "Buildables/FGBuildable.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
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

	UE_LOG(
		LogNoGrassUnderBuildings,
		Display,
		TEXT("Clean-room stage 8 loaded: targeted decorative-foliage suppression"));
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
	PostWorldInitializationHandle.Reset();
	WorldCleanupHandle.Reset();
	WorldPostActorTickHandle.Reset();
	RestoreAllDecorativeFoliage();
	ClearLightweightExclusions(ActiveGameWorld.Get(), false);
	ActiveGameWorld.Reset();
	KnownBuildables.Empty();
	ExcludedBuildables.Empty();
	ExclusionBounds.Empty();
	LastLightweightClassCount = INDEX_NONE;
	LastLightweightInstanceCount = INDEX_NONE;
	UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("Clean-room stage 8 unloaded"));
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

void FNoGrassUnderBuildingsModule::ReconcileDecorativeFoliage(UWorld* World)
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
			!Component->IsRegistered() || !IsDecorativeGroundFoliage(Component))
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
		if (!Pair.Key.Component.IsValid())
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
		if (SuppressedFoliage.Contains(Key))
		{
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
		if (!DesiredSuppression.Contains(Pair.Key))
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
	if (Now >= NextBuildableScanAt)
	{
		NextBuildableScanAt = Now + 1.0;
		ScanBuildables(World);
		ScanLightweightBuildables(World);
		if (AppliedFoliageRevision != CoverageRevision)
		{
			ReconcileDecorativeFoliage(World);
			AppliedFoliageRevision = CoverageRevision;
		}
	}
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
	const TWeakObjectPtr<AFGBuildable>& Buildable)
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
	if (World && PreviousBounds.IsValid)
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
