// Instance identification, suppression, restoration, and spatial grid management.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

// ── Instance identification ───────────────────────────────────────────────────

bool UNoGrassWorldSubsystem::IsDecorativeGrass(
	UHierarchicalInstancedStaticMeshComponent* Component) const
{
	if (!IsValid(Component) || !Component->GetStaticMesh())
	{
		return false;
	}

	const FString MeshPath = Component->GetStaticMesh()->GetPathName().ToLower();
	const bool bGroundVegetationFamily =
		MeshPath.Contains(TEXT("/world/environment/foliage/grass/")) ||
		MeshPath.Contains(TEXT("/world/environment/grass/"));
	const bool bGrassName =
		MeshPath.Contains(TEXT("grass")) ||
		MeshPath.Contains(TEXT("groundcover")) ||
		MeshPath.Contains(TEXT("ground_cover")) ||
		MeshPath.Contains(TEXT("coverground")) ||
		MeshPath.Contains(TEXT("clover")) ||
		MeshPath.Contains(TEXT("weed")) ||
		MeshPath.Contains(TEXT("moss")) ||
		MeshPath.Contains(TEXT("lichen")) ||
		// SM_Plant_07 is the short orange ground flower used around the
		// Grass Fields. Earlier builds caught it through the entire
		// SmallFoliage family; keep the useful exception without accepting
		// nearby bushes and other broad lower-vegetation assets again.
		MeshPath.Contains(TEXT(
			"/foliage/smallfoliage/lowervegatation/sm_plant_07.")) ||
		MeshPath.Contains(TEXT("lowervegetation_plant_010")) ||
		MeshPath.Contains(TEXT("lowervegetation_plant_017"));
	if (!bGroundVegetationFamily && !bGrassName)
	{
		return false;
	}

	const FBox MeshBounds = Component->GetStaticMesh()->GetBoundingBox();
	const float MaxAllowedExtent = FMath::Max(
		NoGrass::CVarMaxGrassExtent.GetValueOnGameThread(),
		50.0f);
	if (MeshBounds.GetExtent().GetMax() > MaxAllowedExtent)
	{
		return false;
	}

	if (NoGrass::CVarDebug.GetValueOnGameThread() != 0)
	{
		const FName MeshName = Component->GetStaticMesh()->GetFName();
		if (!LoggedGrassMeshes.Contains(MeshName))
		{
			LoggedGrassMeshes.Add(MeshName);
			UE_LOG(
				LogNoGrassUnderBuildings,
				Display,
				TEXT("Accepted decorative grass mesh: %s"),
				*Component->GetStaticMesh()->GetPathName());
		}
	}

	return true;
}

bool UNoGrassWorldSubsystem::GrassIntersectsVolume(
	const FTransform& GrassWorldTransform,
	UStaticMesh* GrassMesh,
	const FNoGrassCoverageVolume& Volume) const
{
	if (!GrassMesh)
	{
		return false;
	}

	const float HorizontalPadding = FMath::Max(
		NoGrass::CVarHorizontalPadding.GetValueOnGameThread(),
		0.0f);
	const float VerticalPadding = FMath::Max(
		NoGrass::CVarVerticalPadding.GetValueOnGameThread(),
		0.0f);
	const FVector CoveragePadding(
		HorizontalPadding,
		HorizontalPadding,
		VerticalPadding);

	const FBox LocalCoverage = Volume.LocalBounds.ExpandBy(CoveragePadding);

	// Ordinary decorative foliage is planted at the instance transform origin.
	// Testing the full mesh caused six-to-eight-meter clearings whenever wide
	// low-vegetation assets were also used as bushes. Root-point coverage makes
	// the settings-page buffer the single source of truth. Generated cliff grass
	// remains handled by FilterGeneratedGrassBuffer, which uses the same
	// root-point rule without relying on these ordinary foliage instances.
	const FVector RootInCoverage =
		Volume.LocalToWorld.InverseTransformPosition(
			GrassWorldTransform.GetLocation());
	return LocalCoverage.IsInsideOrOn(RootInCoverage);
}

// ── Suppression ───────────────────────────────────────────────────────────────

void UNoGrassWorldSubsystem::SuppressGrass(
	uint64 OwnerId,
	UHierarchicalInstancedStaticMeshComponent* Component,
	int32 InstanceIndex)
{
	if (!OwnerStates.Contains(OwnerId) ||
		!IsValid(Component) ||
		!Component->IsValidInstance(InstanceIndex))
	{
		return;
	}

	FTransform CurrentTransform;
	if (!Component->GetInstanceTransform(InstanceIndex, CurrentTransform, true))
	{
		return;
	}

	const FNoGrassInstanceKey Key{
		Component,
		QuantizeLocation(CurrentTransform.GetLocation())};
	FNoGrassSuppressionState* ExistingState = SuppressedGrass.Find(Key);

	if (!ExistingState)
	{
		FNoGrassSuppressionState NewState;
		NewState.OriginalWorldTransform = CurrentTransform;
		NewState.LastKnownInstanceIndex = InstanceIndex;
		NewState.Owners.Add(OwnerId);
		NewState.bUsePhysicalRemoval =
			NoGrass::RequiresPhysicalRemoval(Component->GetStaticMesh());
		NewState.bPendingPhysicalRemoval = NewState.bUsePhysicalRemoval;
		ExistingState = &SuppressedGrass.Add(Key, MoveTemp(NewState));
		AddSuppressedToSpatialGrid(Key);

		if (ExistingState->bUsePhysicalRemoval)
		{
			PendingPhysicalRemovalComponents.Add(Component);
		}
		else
		{
			FTransform HiddenTransform = CurrentTransform;
			HiddenTransform.AddToTranslation(
				FVector(0.0, 0.0, -NoGrass::HiddenInstanceDepth));
			HiddenTransform.SetScale3D(
				CurrentTransform.GetScale3D() * NoGrass::HiddenInstanceScale);
			Component->UpdateInstanceTransform(
				InstanceIndex,
				HiddenTransform,
				true,
				false,
				true);
			DirtyComponents.Add(Component);
		}
	}
	else
	{
		ExistingState->LastKnownInstanceIndex = InstanceIndex;
		ExistingState->Owners.Add(OwnerId);
		if (ExistingState->bUsePhysicalRemoval &&
			!ExistingState->bPhysicallyRemoved)
		{
			ExistingState->bPendingPhysicalRemoval = true;
			PendingPhysicalRemovalComponents.Add(Component);
		}
	}

	OwnerStates.FindChecked(OwnerId).GrassInstances.Add(Key);
}

// ── Index resolution and spatial grid ────────────────────────────────────────

int32 UNoGrassWorldSubsystem::ResolveInstanceIndex(
	const FNoGrassInstanceKey& Key,
	const FNoGrassSuppressionState& State) const
{
	if (State.bPhysicallyRemoved)
	{
		return INDEX_NONE;
	}

	UHierarchicalInstancedStaticMeshComponent* Component = Key.Component.Get();
	if (!Component)
	{
		return INDEX_NONE;
	}

	auto MatchesLocation = [&State, Component](int32 Index)
	{
		if (!Component->IsValidInstance(Index))
		{
			return false;
		}
		FTransform Transform;
		return Component->GetInstanceTransform(Index, Transform, true) &&
			FVector::DistSquared(
				Transform.GetLocation(),
				State.OriginalWorldTransform.GetLocation()) <=
			FMath::Square(NoGrass::InstanceResolveRadius);
	};

	if (MatchesLocation(State.LastKnownInstanceIndex))
	{
		return State.LastKnownInstanceIndex;
	}

	const TArray<int32> Nearby = Component->GetInstancesOverlappingSphere(
		State.OriginalWorldTransform.GetLocation(),
		NoGrass::InstanceResolveRadius,
		true);
	for (const int32 Index : Nearby)
	{
		if (MatchesLocation(Index))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

void UNoGrassWorldSubsystem::AddSuppressedToSpatialGrid(
	const FNoGrassInstanceKey& Key)
{
	if (const FNoGrassSuppressionState* State = SuppressedGrass.Find(Key))
	{
		SuppressedSpatialGrid.FindOrAdd(
			SpatialCellForLocation(
				State->OriginalWorldTransform.GetLocation())).Add(Key);
	}
}

void UNoGrassWorldSubsystem::RemoveSuppressedFromSpatialGrid(
	const FNoGrassInstanceKey& Key)
{
	const FNoGrassSuppressionState* State = SuppressedGrass.Find(Key);
	if (!State)
	{
		return;
	}

	const FIntPoint CellKey =
		SpatialCellForLocation(State->OriginalWorldTransform.GetLocation());
	if (TSet<FNoGrassInstanceKey>* Cell = SuppressedSpatialGrid.Find(CellKey))
	{
		Cell->Remove(Key);
		if (Cell->IsEmpty())
		{
			SuppressedSpatialGrid.Remove(CellKey);
		}
	}
}

// ── Deferred physical removal ─────────────────────────────────────────────────

void UNoGrassWorldSubsystem::FlushPendingPhysicalRemovals()
{
	// Candidate indices are cached while a coverage task is time-sliced. Wait
	// until that task finishes before removing instances, because removal shifts
	// every later index in the same HISM component.
	if (ActiveCoverageTask.IsValid() ||
		PendingPhysicalRemovalComponents.IsEmpty())
	{
		return;
	}

	TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>>
		RetryComponents;
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>&
		WeakComponent : PendingPhysicalRemovalComponents)
	{
		UHierarchicalInstancedStaticMeshComponent* Component =
			WeakComponent.Get();
		if (!Component)
		{
			continue;
		}

		struct FPendingRemoval
		{
			FNoGrassInstanceKey Key;
			int32 InstanceIndex = INDEX_NONE;
		};

		TArray<FPendingRemoval> Pending;
		for (TPair<FNoGrassInstanceKey, FNoGrassSuppressionState>& Pair :
			SuppressedGrass)
		{
			FNoGrassSuppressionState& State = Pair.Value;
			if (Pair.Key.Component.Get() != Component ||
				!State.bUsePhysicalRemoval ||
				!State.bPendingPhysicalRemoval ||
				State.bPhysicallyRemoved ||
				State.Owners.IsEmpty())
			{
				continue;
			}

			const int32 InstanceIndex = ResolveInstanceIndex(Pair.Key, State);
			if (InstanceIndex == INDEX_NONE)
			{
				RetryComponents.Add(Component);
				continue;
			}
			Pending.Add({Pair.Key, InstanceIndex});
		}

		Pending.Sort(
			[](const FPendingRemoval& A, const FPendingRemoval& B)
			{
				return A.InstanceIndex > B.InstanceIndex;
			});

		TArray<int32> InstanceIndices;
		InstanceIndices.Reserve(Pending.Num());
		for (const FPendingRemoval& Entry : Pending)
		{
			InstanceIndices.Add(Entry.InstanceIndex);
		}

		if (!InstanceIndices.IsEmpty() &&
			Component->RemoveInstances(InstanceIndices, true))
		{
			for (const FPendingRemoval& Entry : Pending)
			{
				if (FNoGrassSuppressionState* State =
					SuppressedGrass.Find(Entry.Key))
				{
					State->bPendingPhysicalRemoval = false;
					State->bPhysicallyRemoved = true;
					State->LastKnownInstanceIndex = INDEX_NONE;
				}
			}
			DirtyComponents.Add(Component);
		}
		else if (!InstanceIndices.IsEmpty())
		{
			RetryComponents.Add(Component);
		}
	}

	PendingPhysicalRemovalComponents = MoveTemp(RetryComponents);
}

void UNoGrassWorldSubsystem::FlushDirtyComponents()
{
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>&
		WeakComponent : DirtyComponents)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component =
			WeakComponent.Get())
		{
			const UStaticMesh* Mesh = Component->GetStaticMesh();
			const bool bPreserveOriginalFoliageTree =
				Mesh && Mesh->GetPathName().Contains(
					TEXT("SM_Plant_07"),
					ESearchCase::IgnoreCase);
			if (bPreserveOriginalFoliageTree)
			{
				// This orange ground flower is ordinary removable foliage. A
				// forced HISM tree rebuild changes the distance behavior of the
				// entire streamed component, making every flower pop in only when
				// the player is close. Its instance transform update needs only a
				// scene-proxy refresh.
				Component->MarkRenderStateDirty();
				continue;
			}

			// Some streamed FGFoliageInstancedSMC cells keep drawing their old
			// hierarchical instance buffer even after the CPU transform and scene
			// proxy are marked dirty. Rebuild only components changed by this mod;
			// this is the targeted equivalent of foliage.RebuildFoliageTrees.
			if (!Component->BuildTreeIfOutdated(false, true))
			{
				Component->MarkRenderStateDirty();
			}
		}
	}
	DirtyComponents.Reset();
}
