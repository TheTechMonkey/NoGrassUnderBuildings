// Landscape and cliff generated-grass buffer filtering and staged regrowth refresh.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"

#include "FGCliffActor.h"
#include "GrassInstancedStaticMeshComponent.h"
#include "LandscapeProxy.h"
#include "EngineUtils.h"
#include "RenderTransform.h"
#include "StaticMeshResources.h"

// ── Buffer snapshot type ──────────────────────────────────────────────────────
// Forward-declared in the public header; fully defined here because it is only
// needed inside this translation unit.

struct FNoGrassGeneratedGrassBufferSnapshot
{
	TArray<FClusterNode> ClusterTree;
	int32 OcclusionLayerNum = 0;
	int32 NumBuiltRenderInstances = 0;
	TUniquePtr<FStaticMeshInstanceData> InstanceData;
};

// ── Instance data clone ───────────────────────────────────────────────────────

static TUniquePtr<FStaticMeshInstanceData> CloneInstanceData(
	const FStaticMeshInstanceData& Source)
{
	TUniquePtr<FStaticMeshInstanceData> Clone =
		MakeUnique<FStaticMeshInstanceData>(Source.GetTranslationUsesHalfs());
	Clone->AllocateInstances(
		Source.GetNumInstances(),
		Source.GetNumCustomDataFloats(),
		EResizeBufferFlags::None,
		true);

	TArray<float> CustomData;
	CustomData.SetNumUninitialized(Source.GetNumCustomDataFloats());
	for (int32 InstanceIndex = 0;
		InstanceIndex < Source.GetNumInstances();
		++InstanceIndex)
	{
		FRenderTransform Transform;
		float RandomInstanceId = 0.0f;
		FVector4f LightMapData(0.0f, 0.0f, 0.0f, 0.0f);
		Source.GetInstanceTransform(InstanceIndex, Transform);
		Source.GetInstanceRandomID(InstanceIndex, RandomInstanceId);
		Source.GetInstanceLightMapData(InstanceIndex, LightMapData);
		Clone->SetInstance(
			InstanceIndex,
			Transform.ToMatrix44f(),
			RandomInstanceId,
			FVector2D(LightMapData.X, LightMapData.Y),
			FVector2D(LightMapData.Z, LightMapData.W));

		if (!CustomData.IsEmpty())
		{
			Source.GetInstanceCustomDataValues(
				InstanceIndex,
				MakeArrayView(CustomData));
			for (int32 CustomIndex = 0;
				CustomIndex < CustomData.Num();
				++CustomIndex)
			{
				Clone->SetInstanceCustomData(
					InstanceIndex,
					CustomIndex,
					CustomData[CustomIndex]);
			}
		}
	}
	return Clone;
}

// ── Public entry point hooked from AcceptPrebuiltTree ─────────────────────────

int32 UNoGrassWorldSubsystem::FilterGeneratedGrass(
	UGrassInstancedStaticMeshComponent* Component,
	TArray<FClusterNode>& ClusterTree,
	int32& OcclusionLayerNum,
	int32& NumBuiltRenderInstances,
	FStaticMeshInstanceData* InstanceData)
{
	if (bApplyingGeneratedGrassBufferSnapshot)
	{
		return 0;
	}

	if (!IsValid(Component) || !InstanceData)
	{
		return 0;
	}
	const AFGCliffActor* CliffActor =
		Component->GetTypedOuter<AFGCliffActor>();
	const ALandscapeProxy* LandscapeProxy =
		Component->GetTypedOuter<ALandscapeProxy>();
	if (!IsValid(CliffActor) && !IsValid(LandscapeProxy))
	{
		return 0;
	}

	TSharedPtr<FNoGrassGeneratedGrassBufferSnapshot> Snapshot =
		MakeShared<FNoGrassGeneratedGrassBufferSnapshot>();
	Snapshot->ClusterTree = ClusterTree;
	Snapshot->OcclusionLayerNum = OcclusionLayerNum;
	Snapshot->NumBuiltRenderInstances = NumBuiltRenderInstances;
	Snapshot->InstanceData = CloneInstanceData(*InstanceData);
	GeneratedGrassBufferSnapshots.Add(Component, MoveTemp(Snapshot));

	return FilterGeneratedGrassBuffer(
		Component,
		InstanceData,
		ClusterTree,
		OcclusionLayerNum,
		NumBuiltRenderInstances);
}

// ── Core buffer filter ────────────────────────────────────────────────────────

int32 UNoGrassWorldSubsystem::FilterGeneratedGrassBuffer(
	UGrassInstancedStaticMeshComponent* Component,
	FStaticMeshInstanceData* InstanceData,
	TArray<FClusterNode>& ClusterTree,
	int32& OcclusionLayerNum,
	int32& NumBuiltRenderInstances)
{
	if (!IsValid(Component) || !InstanceData || !Component->GetStaticMesh())
	{
		return 0;
	}

	AFGCliffActor* CliffActor = Component->GetTypedOuter<AFGCliffActor>();
	ALandscapeProxy* LandscapeProxy = Component->GetTypedOuter<ALandscapeProxy>();
	const bool bLandscapeGrass = IsValid(LandscapeProxy);
	if ((!IsValid(CliffActor) && !bLandscapeGrass) ||
		Component->GetWorld() != GetWorld())
	{
		return 0;
	}

	// ── Determine bounds for the generated component ──────────────────────────
	FBox GeneratedBounds = Component->Bounds.GetBox();
	if (IsValid(CliffActor))
	{
		GeneratedBounds = CliffActor->GetComponentsBoundingBox(true);
		if (UStaticMeshComponent* CliffMesh = CliffActor->GetMeshComponent())
		{
			GeneratedBounds = CliffMesh->Bounds.GetBox();
		}
	}
	if (!GeneratedBounds.IsValid || GeneratedBounds.GetExtent().IsNearlyZero())
	{
		GeneratedBounds.Init();
		const FTransform ComponentToWorld = Component->GetComponentTransform();
		for (int32 InstanceIndex = 0;
			InstanceIndex < InstanceData->GetNumInstances();
			++InstanceIndex)
		{
			FRenderTransform PackedTransform;
			InstanceData->GetInstanceTransform(InstanceIndex, PackedTransform);
			const FTransform LocalTransform(PackedTransform.ToMatrix());
			GeneratedBounds +=
				(LocalTransform * ComponentToWorld).GetLocation();
		}
	}
	if (!GeneratedBounds.IsValid)
	{
		return 0;
	}
	const FBox CandidateBounds =
		GeneratedBounds.ExpandBy(NoGrass::GetCandidateQueryMargin());

	// ── Collect covering volumes ──────────────────────────────────────────────
	TArray<const FNoGrassCoverageVolume*> CandidateVolumes;
	TArray<const FNoGrassCliffRegrowthVolume*> CandidateRegrowthVolumes;

	for (const TPair<uint64, FNoGrassOwnerState>& Pair : OwnerStates)
	{
		for (const FNoGrassCoverageVolume& Volume : Pair.Value.CoverageVolumes)
		{
			if (!Volume.bLandscapeOnly &&
				Volume.WorldBounds.IsValid &&
				Volume.WorldBounds.Intersect(CandidateBounds))
			{
				CandidateVolumes.Add(&Volume);
			}
		}
	}
	for (const FNoGrassPendingOwnerRelease& Pending : PendingOwnerReleases)
	{
		for (const FNoGrassCoverageVolume& Volume :
			Pending.OwnerState.CoverageVolumes)
		{
			if (!Volume.bLandscapeOnly &&
				Volume.WorldBounds.IsValid &&
				Volume.WorldBounds.Intersect(CandidateBounds))
			{
				CandidateVolumes.Add(&Volume);
			}
		}
	}
	for (const FNoGrassCliffRegrowthVolume& Regrowth : CliffRegrowthVolumes)
	{
		if (!Regrowth.Volume.bLandscapeOnly &&
			Regrowth.Volume.WorldBounds.IsValid &&
			Regrowth.Volume.WorldBounds.Intersect(CandidateBounds))
		{
			CandidateRegrowthVolumes.Add(&Regrowth);
		}
	}
	// Permanent coverage is intentionally retained after dismantling. Query its
	// spatial grid instead of making every generated component inspect every
	// foundation ever built.
	TSet<int32> PermanentCandidates;
	const FIntPoint MinPermanentCell = SpatialCellForLocation(CandidateBounds.Min);
	const FIntPoint MaxPermanentCell = SpatialCellForLocation(CandidateBounds.Max);
	for (int32 X = MinPermanentCell.X; X <= MaxPermanentCell.X; ++X)
	{
		for (int32 Y = MinPermanentCell.Y; Y <= MaxPermanentCell.Y; ++Y)
		{
			if (const TArray<int32>* Cell =
				PermanentCoverageSpatialGrid.Find(FIntPoint(X, Y)))
			{
				PermanentCandidates.Append(*Cell);
			}
		}
	}
	for (const int32 Index : PermanentCandidates)
	{
		if (!PermanentCoverageVolumes.IsValidIndex(Index))
		{
			continue;
		}
		const FNoGrassCoverageVolume& Volume = PermanentCoverageVolumes[Index];
		if (Volume.WorldBounds.IsValid &&
			Volume.WorldBounds.Intersect(CandidateBounds))
		{
			CandidateVolumes.Add(&Volume);
		}
	}

	if (CandidateVolumes.IsEmpty() && CandidateRegrowthVolumes.IsEmpty())
	{
		return 0;
	}

	// ── Filter instances ──────────────────────────────────────────────────────
	const FTransform ComponentToWorld = Component->GetComponentTransform();
	UStaticMesh* GrassMesh = Component->GetStaticMesh();
	const bool bRelocateAnimatedPlant = GrassMesh->GetName().Equals(
		TEXT("SM_Plant_08"),
		ESearchCase::IgnoreCase);
	const float GeneratedGrassPadding = bLandscapeGrass
		? EffectiveLandscapePadding
		: FMath::Max(NoGrass::CVarCliffGrassPadding.GetValueOnGameThread(), 0.0f);
	const float VerticalPadding = bLandscapeGrass
		? EffectiveLandscapeVerticalPadding
		: FMath::Max(NoGrass::CVarVerticalPadding.GetValueOnGameThread(), 0.0f);
	const FVector GeneratedCoveragePadding(
		GeneratedGrassPadding,
		GeneratedGrassPadding,
		VerticalPadding);
	int32 HiddenCount = 0;
	TBitArray<> CoveredAnimatedPlantInstances(
		false,
		bRelocateAnimatedPlant ? InstanceData->GetNumInstances() : 0);
	const double Now = GetWorld()->GetTimeSeconds();

	for (int32 InstanceIndex = 0;
		InstanceIndex < InstanceData->GetNumInstances();
		++InstanceIndex)
	{
		FRenderTransform PackedTransform;
		InstanceData->GetInstanceTransform(InstanceIndex, PackedTransform);
		const FTransform LocalTransform(PackedTransform.ToMatrix());
		const FTransform WorldTransform = LocalTransform * ComponentToWorld;

		bool bCovered = false;
		for (const FNoGrassCoverageVolume* Volume : CandidateVolumes)
		{
			if (!Volume)
			{
				continue;
			}
			// Generated grass is stored in multi-blade clumps. Testing the whole
			// clump bounds removes adjacent clumps and creates a conspicuous bare
			// halo. Test the planted root instead, with a modest dedicated edge
			// buffer to catch blades that lean across the building boundary.
			const FVector RootInCoverage =
				Volume->LocalToWorld.InverseTransformPosition(
					WorldTransform.GetLocation());
			if (Volume->LocalBounds
				.ExpandBy(GeneratedCoveragePadding)
				.IsInsideOrOn(RootInCoverage))
			{
				bCovered = true;
				break;
			}
		}

		if (!bCovered)
		{
			for (const FNoGrassCliffRegrowthVolume* Regrowth :
				CandidateRegrowthVolumes)
			{
				if (!Regrowth)
				{
					continue;
				}

				const FNoGrassCoverageVolume& Volume = Regrowth->Volume;
				const FVector RootInCoverage =
					Volume.LocalToWorld.InverseTransformPosition(
						WorldTransform.GetLocation());
				const FBox ExpandedBounds =
					Volume.LocalBounds.ExpandBy(GeneratedCoveragePadding);
				if (!ExpandedBounds.IsInsideOrOn(RootInCoverage))
				{
					continue;
				}

				double InstanceRestoreAt = Regrowth->RestoreEndAt;
				if (Regrowth->RestoreEndAt >
					Regrowth->RestoreStartAt + UE_SMALL_NUMBER)
				{
					const uint32 Seed = GetTypeHash(
						QuantizeLocation(WorldTransform.GetLocation()));
					const float RestoreAlpha = NoGrass::GetRegrowthAlpha(
						Regrowth->Wave,
						WorldTransform.GetLocation(),
						Seed);
					InstanceRestoreAt = FMath::Lerp(
						Regrowth->RestoreStartAt,
						Regrowth->RestoreEndAt,
						FMath::Max(RestoreAlpha, 0.0f));
				}

				if (Now < InstanceRestoreAt)
				{
					bCovered = true;
					break;
				}
			}
		}

		if (bCovered)
		{
			if (bRelocateAnimatedPlant)
			{
				CoveredAnimatedPlantInstances[InstanceIndex] = true;
			}
			else
			{
				// Nullification keeps the instance count and cluster indices stable
				// while marking ordinary generated foliage hidden on the GPU.
				InstanceData->NullifyInstance(InstanceIndex);
			}
			++HiddenCount;
		}
	}

	// ── SM_Plant_08 special path ───────────────────────────────────────────────
	// The close-range SM_Plant_08 material ignores transformed/zeroed retained
	// instances. Match Unreal's working exclusion-box result by omitting covered
	// records from this one local render buffer and rebuilding only its HISM
	// cluster tree. This does not invalidate Landscape Grass elsewhere.
	if (bRelocateAnimatedPlant && HiddenCount > 0 &&
		HiddenCount < InstanceData->GetNumInstances())
	{
		const int32 SourceCount = InstanceData->GetNumInstances();
		const int32 VisibleCount = SourceCount - HiddenCount;
		const int32 NumCustomDataFloats = InstanceData->GetNumCustomDataFloats();
		TUniquePtr<FStaticMeshInstanceData> VisibleInstanceData =
			MakeUnique<FStaticMeshInstanceData>(
				InstanceData->GetTranslationUsesHalfs());
		VisibleInstanceData->AllocateInstances(
			VisibleCount,
			NumCustomDataFloats,
			EResizeBufferFlags::None,
			true);

		TArray<FMatrix> VisibleTransforms;
		VisibleTransforms.Reserve(VisibleCount);
		TArray<float> VisibleCustomData;
		VisibleCustomData.Reserve(VisibleCount * NumCustomDataFloats);
		int32 VisibleIndex = 0;
		for (int32 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
		{
			if (CoveredAnimatedPlantInstances[SourceIndex])
			{
				continue;
			}

			FRenderTransform SourceTransform;
			InstanceData->GetInstanceTransform(SourceIndex, SourceTransform);
			float RandomInstanceId = 0.0f;
			InstanceData->GetInstanceRandomID(SourceIndex, RandomInstanceId);
			FVector4f LightMapData;
			InstanceData->GetInstanceLightMapData(SourceIndex, LightMapData);
			VisibleInstanceData->SetInstance(
				VisibleIndex,
				SourceTransform.ToMatrix44f(),
				RandomInstanceId,
				FVector2D(LightMapData.X, LightMapData.Y),
				FVector2D(LightMapData.Z, LightMapData.W));
			VisibleTransforms.Add(SourceTransform.ToMatrix());

			if (NumCustomDataFloats > 0)
			{
				TArray<float, TInlineAllocator<8>> CustomValues;
				CustomValues.SetNumUninitialized(NumCustomDataFloats);
				InstanceData->GetInstanceCustomDataValues(
					SourceIndex,
					MakeArrayView(CustomValues));
				for (int32 CustomIndex = 0;
					CustomIndex < NumCustomDataFloats;
					++CustomIndex)
				{
					const float Value = CustomValues[CustomIndex];
					VisibleInstanceData->SetInstanceCustomData(
						VisibleIndex, CustomIndex, Value);
					VisibleCustomData.Add(Value);
				}
			}
			++VisibleIndex;
		}

		TArray<FClusterNode> VisibleClusterTree;
		TArray<int32> SortedInstances;
		TArray<int32> InstanceReorderTable;
		int32 VisibleOcclusionLayerNum = 0;
		UGrassInstancedStaticMeshComponent::BuildTreeAnyThread(
			VisibleTransforms,
			VisibleCustomData,
			NumCustomDataFloats,
			GrassMesh->GetBoundingBox(),
			VisibleClusterTree,
			SortedInstances,
			InstanceReorderTable,
			VisibleOcclusionLayerNum,
			Component->DesiredInstancesPerLeaf(),
			false);

		// BuildTreeAnyThread emits cluster ranges in sorted-instance order. Apply
		// the same in-place permutation used by LandscapeGrass.cpp.
		for (int32 FirstUnfixedIndex = 0;
			FirstUnfixedIndex < VisibleCount;
			++FirstUnfixedIndex)
		{
			const int32 LoadFrom = SortedInstances[FirstUnfixedIndex];
			if (LoadFrom == FirstUnfixedIndex)
			{
				continue;
			}
			VisibleInstanceData->SwapInstance(FirstUnfixedIndex, LoadFrom);
			const int32 SwapGoesTo = InstanceReorderTable[FirstUnfixedIndex];
			SortedInstances[SwapGoesTo] = LoadFrom;
			InstanceReorderTable[LoadFrom] = SwapGoesTo;
			InstanceReorderTable[FirstUnfixedIndex] = FirstUnfixedIndex;
			SortedInstances[FirstUnfixedIndex] = FirstUnfixedIndex;
		}

		Swap(*InstanceData, *VisibleInstanceData);
		ClusterTree = MoveTemp(VisibleClusterTree);
		OcclusionLayerNum = VisibleOcclusionLayerNum;
		NumBuiltRenderInstances = VisibleCount;
	}
	else if (bRelocateAnimatedPlant && HiddenCount > 0)
	{
		// AcceptPrebuiltTree requires at least one render instance. A wholly
		// covered component is rare; keep its records valid but off-map until the
		// next local refresh restores or recompacts them.
		for (int32 InstanceIndex = 0;
			InstanceIndex < InstanceData->GetNumInstances();
			++InstanceIndex)
		{
			if (!CoveredAnimatedPlantInstances[InstanceIndex])
			{
				continue;
			}
			FRenderTransform PackedTransform;
			InstanceData->GetInstanceTransform(InstanceIndex, PackedTransform);
			FTransform HiddenWorldTransform =
				FTransform(PackedTransform.ToMatrix()) * ComponentToWorld;
			HiddenWorldTransform.AddToTranslation(FVector(
				NoGrass::HiddenAnimatedPlantOffset,
				NoGrass::HiddenAnimatedPlantOffset,
				-NoGrass::HiddenAnimatedPlantOffset));
			HiddenWorldTransform.SetScale3D(
				HiddenWorldTransform.GetScale3D() * NoGrass::HiddenInstanceScale);
			const FTransform HiddenTransform =
				HiddenWorldTransform.GetRelativeTransform(ComponentToWorld);
			InstanceData->SetInstance(
				InstanceIndex,
				FMatrix44f(HiddenTransform.ToMatrixWithScale()));
		}
	}

	if (HiddenCount > 0)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Filtered %d of %d generated grass instances for %s (%s)"),
			HiddenCount,
			InstanceData->GetNumInstances(),
			*GetPathNameSafe(bLandscapeGrass
				? static_cast<const UObject*>(LandscapeProxy)
				: static_cast<const UObject*>(CliffActor)),
			*GetPathNameSafe(GrassMesh));
	}
	return HiddenCount;
}

// ── Regrowth and refresh scheduling ──────────────────────────────────────────

void UNoGrassWorldSubsystem::ScheduleGeneratedGrassRefresh(
	const TArray<FNoGrassCoverageVolume>& Volumes,
	double DelaySeconds)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	bool bAddedBounds = false;
	for (const FNoGrassCoverageVolume& Volume : Volumes)
	{
		if (!Volume.bLandscapeOnly && Volume.WorldBounds.IsValid)
		{
			PendingGeneratedGrassRefreshBounds += Volume.WorldBounds;
			bAddedBounds = true;
		}
	}

	if (bAddedBounds)
	{
		// Debounce the initial streamed-building scan so a cliff is rebuilt once
		// after all nearby coverage volumes have been registered.
		const double RequestedAt =
			World->GetTimeSeconds() + FMath::Max(DelaySeconds, 0.0);
		NextGeneratedGrassRefreshAt = NextGeneratedGrassRefreshAt == DBL_MAX
			? RequestedAt
			: FMath::Max(NextGeneratedGrassRefreshAt, RequestedAt);
	}
}

void UNoGrassWorldSubsystem::ProcessGeneratedGrassRegrowthAndRefresh(
	double Deadline)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	bool bNeedsVisualRefresh = false;
	for (int32 Index = CliffRegrowthVolumes.Num() - 1; Index >= 0; --Index)
	{
		FNoGrassCliffRegrowthVolume& Regrowth = CliffRegrowthVolumes[Index];
		if (Regrowth.RestoreEndAt <= Now)
		{
			PendingGeneratedGrassRefreshBounds += Regrowth.Volume.WorldBounds;
			CliffRegrowthVolumes.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			bNeedsVisualRefresh = true;
			continue;
		}

		if (Regrowth.NextVisualRefreshAt <= Now)
		{
			PendingGeneratedGrassRefreshBounds += Regrowth.Volume.WorldBounds;
			const double Duration = FMath::Max(
				Regrowth.RestoreEndAt - Regrowth.RestoreStartAt,
				0.0);
			const double StepSeconds = FMath::Clamp(
				Duration / NoGrass::GeneratedGrassRegrowthVisualSteps,
				0.5,
				10.0);
			Regrowth.NextVisualRefreshAt = Now + StepSeconds;
			bNeedsVisualRefresh = true;
		}
	}
	if (bNeedsVisualRefresh)
	{
		const double RequestedAt = Now + 0.05;
		NextGeneratedGrassRefreshAt = NextGeneratedGrassRefreshAt == DBL_MAX
			? RequestedAt
			: FMath::Min(NextGeneratedGrassRefreshAt, RequestedAt);
	}

	if (!PendingGeneratedGrassRefreshBounds.IsValid ||
		Now < NextGeneratedGrassRefreshAt ||
		FPlatformTime::Seconds() >= Deadline)
	{
		return;
	}

	const FBox RefreshBounds =
		PendingGeneratedGrassRefreshBounds.ExpandBy(NoGrass::GetCandidateQueryMargin());
	PendingGeneratedGrassRefreshBounds.Init();
	NextGeneratedGrassRefreshAt = DBL_MAX;

	int32 RefreshedComponents = 0;
	TArray<TWeakObjectPtr<UGrassInstancedStaticMeshComponent>> StaleSnapshots;

	for (const TPair<
		TWeakObjectPtr<UGrassInstancedStaticMeshComponent>,
		TSharedPtr<FNoGrassGeneratedGrassBufferSnapshot>>& Pair :
		GeneratedGrassBufferSnapshots)
	{
		UGrassInstancedStaticMeshComponent* Component = Pair.Key.Get();
		const TSharedPtr<FNoGrassGeneratedGrassBufferSnapshot>& Snapshot =
			Pair.Value;
		if (!IsValid(Component) || !Component->IsRegistered() ||
			!Snapshot.IsValid() || !Snapshot->InstanceData)
		{
			StaleSnapshots.Add(Pair.Key);
			continue;
		}

		AFGCliffActor* CliffActor = Component->GetTypedOuter<AFGCliffActor>();
		ALandscapeProxy* LandscapeProxy =
			Component->GetTypedOuter<ALandscapeProxy>();
		if (!IsValid(CliffActor) && !IsValid(LandscapeProxy))
		{
			StaleSnapshots.Add(Pair.Key);
			continue;
		}
		if (IsValid(CliffActor) && !CliffActor->IsSignificant())
		{
			continue;
		}

		FBox ActorBounds = Component->Bounds.GetBox();
		if (IsValid(CliffActor))
		{
			UStaticMeshComponent* CliffMesh = CliffActor->GetMeshComponent();
			ActorBounds = IsValid(CliffMesh)
				? CliffMesh->Bounds.GetBox()
				: CliffActor->GetComponentsBoundingBox(true);
		}
		if (!ActorBounds.IsValid || !ActorBounds.Intersect(RefreshBounds))
		{
			continue;
		}

		TUniquePtr<FStaticMeshInstanceData> RefreshedData =
			CloneInstanceData(*Snapshot->InstanceData);
		if (!RefreshedData)
		{
			continue;
		}
		TArray<FClusterNode> RefreshedClusterTree = Snapshot->ClusterTree;
		int32 RefreshedOcclusionLayerNum = Snapshot->OcclusionLayerNum;
		int32 RefreshedInstanceCount = Snapshot->NumBuiltRenderInstances;
		FilterGeneratedGrassBuffer(
			Component,
			RefreshedData.Get(),
			RefreshedClusterTree,
			RefreshedOcclusionLayerNum,
			RefreshedInstanceCount);

		TGuardValue<bool> ApplyingSnapshotGuard(
			bApplyingGeneratedGrassBufferSnapshot,
			true);
		Component->AcceptPrebuiltTree(
			RefreshedClusterTree,
			RefreshedOcclusionLayerNum,
			RefreshedInstanceCount,
			RefreshedData.Get());
		++RefreshedComponents;
	}

	for (const TWeakObjectPtr<UGrassInstancedStaticMeshComponent>& Stale :
		StaleSnapshots)
	{
		GeneratedGrassBufferSnapshots.Remove(Stale);
	}

	if (RefreshedComponents > 0)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Updated %d generated grass render buffers in place after coverage changed"),
			RefreshedComponents);
	}
}

void UNoGrassWorldSubsystem::RefreshLandscapeGrassReach()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	float MaximumObservedReach = 0.0f;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		for (UHierarchicalInstancedStaticMeshComponent* Component :
			It->FoliageComponents)
		{
			if (!IsValid(Component) || !Component->GetStaticMesh())
			{
				continue;
			}

			const FBox MeshBounds =
				Component->GetStaticMesh()->GetBoundingBox();
			const float LocalReach = FMath::Max(
				FMath::Max(
					FMath::Abs(MeshBounds.Min.X),
					FMath::Abs(MeshBounds.Max.X)),
				FMath::Max(
					FMath::Abs(MeshBounds.Min.Y),
					FMath::Abs(MeshBounds.Max.Y)));
			const FVector ComponentScale =
				Component->GetComponentScale().GetAbs();
			const float HorizontalComponentScale =
				FMath::Max(ComponentScale.X, ComponentScale.Y);
			MaximumObservedReach = FMath::Max(
				MaximumObservedReach,
				LocalReach *
					FMath::Max(HorizontalComponentScale, 1.0f) *
					NoGrass::LandscapeInstanceScaleSafety);
		}
	}

	const float ConfiguredMinimum = FMath::Max(
		NoGrass::CVarLandscapePadding.GetValueOnGameThread(),
		0.0f);
	const float RequiredPadding = FMath::Max(
		ConfiguredMinimum,
		FMath::Min(MaximumObservedReach, NoGrass::MaxAutomaticLandscapePadding));
	const float RequiredVerticalPadding = FMath::Max(
		NoGrass::CVarLandscapeVerticalPadding.GetValueOnGameThread(),
		0.0f);
	const bool bPaddingChanged = !FMath::IsNearlyEqual(
		RequiredPadding, EffectiveLandscapePadding, 1.0f);
	const bool bVerticalPaddingChanged = !FMath::IsNearlyEqual(
		RequiredVerticalPadding, EffectiveLandscapeVerticalPadding, 1.0f);
	if (!bPaddingChanged && !bVerticalPaddingChanged)
	{
		return;
	}

	EffectiveLandscapePadding = RequiredPadding;
	EffectiveLandscapeVerticalPadding = RequiredVerticalPadding;

	TArray<FNoGrassCoverageVolume> RefreshVolumes;
	for (const TPair<uint64, FNoGrassOwnerState>& OwnerPair : OwnerStates)
	{
		RefreshVolumes.Append(OwnerPair.Value.CoverageVolumes);
	}
	for (const FNoGrassPendingOwnerRelease& Pending : PendingOwnerReleases)
	{
		RefreshVolumes.Append(Pending.OwnerState.CoverageVolumes);
	}
	for (const FNoGrassCliffRegrowthVolume& Regrowth : CliffRegrowthVolumes)
	{
		RefreshVolumes.Add(Regrowth.Volume);
	}
	RefreshVolumes.Append(PermanentCoverageVolumes);
	ScheduleGeneratedGrassRefresh(RefreshVolumes, 0.05);

	if (NoGrass::CVarDebug.GetValueOnGameThread() != 0)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Adjusted Landscape Grass padding to %.0f cm horizontal and %.0f cm vertical"),
			EffectiveLandscapePadding,
			EffectiveLandscapeVerticalPadding);
	}
}
