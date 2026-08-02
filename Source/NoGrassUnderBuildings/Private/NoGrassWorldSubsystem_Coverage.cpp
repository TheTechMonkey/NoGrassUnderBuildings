// Building-to-coverage-volume geometry and foliage candidate processing.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"

#include "Buildables/FGBuildable.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "InstanceData.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"

// ── Coverage geometry ─────────────────────────────────────────────────────────

void UNoGrassWorldSubsystem::BuildCoverageForActor(
	AFGBuildable* Buildable,
	TArray<FNoGrassCoverageVolume>& OutVolumes) const
{
	TInlineComponentArray<UStaticMeshComponent*> StaticMeshComponents;
	Buildable->GetComponents(StaticMeshComponents);

	for (UStaticMeshComponent* Component : StaticMeshComponents)
	{
		if (!IsValid(Component) || !Component->GetStaticMesh())
		{
			continue;
		}

		if (Cast<USplineMeshComponent>(Component))
		{
			AddCoverageVolume(
				FTransform::Identity,
				Component->Bounds.GetBox(),
				OutVolumes);
		}
		else
		{
			AppendMeshCoverage(
				Component->GetStaticMesh(),
				Component->GetComponentTransform(),
				OutVolumes);
		}
	}

	TInlineComponentArray<USkeletalMeshComponent*> SkeletalMeshComponents;
	Buildable->GetComponents(SkeletalMeshComponents);
	for (USkeletalMeshComponent* Component : SkeletalMeshComponents)
	{
		if (IsValid(Component) && Component->GetSkeletalMeshAsset())
		{
			AddCoverageVolume(
				Component->GetComponentTransform(),
				Component->GetSkeletalMeshAsset()->GetBounds().GetBox(),
				OutVolumes);
		}
	}

	if (OutVolumes.IsEmpty())
	{
		TArray<FInstanceData> InstanceData;
		Buildable->CreateLightweightBuildableInstanceData(
			Buildable->GetLightweightTypeSpecificData(),
			InstanceData);

		for (const FInstanceData& Instance : InstanceData)
		{
			if (Instance.StaticMesh)
			{
				AppendMeshCoverage(
					Instance.StaticMesh,
					Instance.RelativeTransform * Buildable->GetActorTransform(),
					OutVolumes);
			}
		}
	}

	if (OutVolumes.IsEmpty())
	{
		const FBox WorldBounds = Buildable->GetComponentsBoundingBox(true);
		if (WorldBounds.IsValid)
		{
			AddCoverageVolume(FTransform::Identity, WorldBounds, OutVolumes);
		}
	}
}

void UNoGrassWorldSubsystem::BuildCoverageForLightweight(
	AFGLightweightBuildableSubsystem* InLightweightSubsystem,
	TSubclassOf<AFGBuildable> BuildableClass,
	int32 RuntimeIndex,
	TArray<FNoGrassCoverageVolume>& OutVolumes) const
{
	const FRuntimeBuildableInstanceData* RuntimeData =
		InLightweightSubsystem->GetRuntimeDataForBuildableClassAndIndex(
			BuildableClass,
			RuntimeIndex);
	if (!RuntimeData || !RuntimeData->IsValid())
	{
		return;
	}

	const AFGBuildable* BuildableCDO = BuildableClass->GetDefaultObject<AFGBuildable>();
	if (!BuildableCDO)
	{
		return;
	}

	TArray<FInstanceData> InstanceData;
	BuildableCDO->CreateLightweightBuildableInstanceData(
		RuntimeData->TypeSpecificData,
		InstanceData);

	for (const FInstanceData& Instance : InstanceData)
	{
		if (Instance.StaticMesh)
		{
			AppendMeshCoverage(
				Instance.StaticMesh,
				Instance.RelativeTransform * RuntimeData->Transform,
				OutVolumes);
		}
	}

	if (OutVolumes.IsEmpty() && RuntimeData->BoundingBox.IsValid)
	{
		AddCoverageVolume(
			RuntimeData->Transform,
			RuntimeData->BoundingBox,
			OutVolumes);
	}
}

void UNoGrassWorldSubsystem::AppendMeshCoverage(
	UStaticMesh* StaticMesh,
	const FTransform& MeshToWorld,
	TArray<FNoGrassCoverageVolume>& OutVolumes) const
{
	if (!StaticMesh)
	{
		return;
	}

	const UBodySetup* BodySetup = StaticMesh->GetBodySetup();
	const int32 InitialCount = OutVolumes.Num();

	if (BodySetup)
	{
		const FKAggregateGeom& Aggregate = BodySetup->AggGeom;

		for (const FKBoxElem& Box : Aggregate.BoxElems)
		{
			AddCoverageVolume(
				Box.GetTransform() * MeshToWorld,
				FBox::BuildAABB(
					FVector::ZeroVector,
					FVector(Box.X, Box.Y, Box.Z) * 0.5),
				OutVolumes);
		}

		for (const FKSphereElem& Sphere : Aggregate.SphereElems)
		{
			AddCoverageVolume(
				Sphere.GetTransform() * MeshToWorld,
				FBox::BuildAABB(FVector::ZeroVector, FVector(Sphere.Radius)),
				OutVolumes);
		}

		for (const FKSphylElem& Capsule : Aggregate.SphylElems)
		{
			AddCoverageVolume(
				Capsule.GetTransform() * MeshToWorld,
				FBox::BuildAABB(
					FVector::ZeroVector,
					FVector(
						Capsule.Radius,
						Capsule.Radius,
						Capsule.Length * 0.5 + Capsule.Radius)),
				OutVolumes);
		}

		for (const FKConvexElem& Convex : Aggregate.ConvexElems)
		{
			if (Convex.ElemBox.IsValid)
			{
				AddCoverageVolume(
					Convex.GetTransform() * MeshToWorld,
					Convex.ElemBox,
					OutVolumes);
			}
		}
	}

	if (OutVolumes.Num() == InitialCount)
	{
		AddCoverageVolume(MeshToWorld, StaticMesh->GetBoundingBox(), OutVolumes);
	}
	else
	{
		const FBox MeshBounds = StaticMesh->GetBoundingBox();
		const FVector MeshSize = MeshBounds.GetSize().GetAbs();
		const float MinimumHorizontalSize = FMath::Min(MeshSize.X, MeshSize.Y);
		const float MaximumHorizontalSize = FMath::Max(MeshSize.X, MeshSize.Y);
		const bool bFlatSolidPart = MeshSize.Z <= MinimumHorizontalSize * 0.35f;
		const bool bSlenderVerticalPart =
			MaximumHorizontalSize <= MeshSize.Z * 0.5f;

		if (bFlatSolidPart || bSlenderVerticalPart)
		{
			// Some lightweight solid pieces have simplified collision that leaves
			// visible panel or pillar areas uncovered. Use the visible bounds for
			// both ordinary foliage and Landscape Grass on these shapes. Open
			// frames still keep their precise collision-only coverage.
			AddCoverageVolume(MeshToWorld, MeshBounds, OutVolumes);
		}
	}
}

void UNoGrassWorldSubsystem::AddCoverageVolume(
	const FTransform& LocalToWorld,
	const FBox& LocalBounds,
	TArray<FNoGrassCoverageVolume>& OutVolumes,
	bool bLandscapeOnly) const
{
	if (!LocalBounds.IsValid || !LocalToWorld.IsValid())
	{
		return;
	}

	FNoGrassCoverageVolume Volume;
	Volume.LocalToWorld = LocalToWorld;
	Volume.LocalBounds = LocalBounds;
	Volume.WorldBounds = LocalBounds.TransformBy(LocalToWorld);
	Volume.bLandscapeOnly = bLandscapeOnly;

	if (Volume.WorldBounds.IsValid &&
		Volume.WorldBounds.GetExtent().GetMax() > UE_SMALL_NUMBER)
	{
		OutVolumes.Add(MoveTemp(Volume));
	}
}

void UNoGrassWorldSubsystem::QueueCoverageVolume(
	uint64 OwnerId,
	const FNoGrassCoverageVolume& Volume)
{
	const FVector Min = Volume.WorldBounds.Min;
	const FVector Max = Volume.WorldBounds.Max;
	const float Width = FMath::Max(Max.X - Min.X, 1.0f);
	const float Depth = FMath::Max(Max.Y - Min.Y, 1.0f);
	const int32 TilesX =
		FMath::Max(1, FMath::CeilToInt(Width / NoGrass::CoverageTileSize));
	const int32 TilesY =
		FMath::Max(1, FMath::CeilToInt(Depth / NoGrass::CoverageTileSize));

	if (TilesX * TilesY > NoGrass::MaxCoverageTilesPerVolume)
	{
		UE_LOG(
			LogNoGrassUnderBuildings,
			Warning,
			TEXT("Skipped an unusually large coverage volume (%d tiles)"),
			TilesX * TilesY);
		return;
	}

	if (Volume.bLandscapeOnly)
	{
		return;
	}

	for (int32 X = 0; X < TilesX; ++X)
	{
		const float TileMinX =
			FMath::Lerp(Min.X, Max.X, static_cast<float>(X) / TilesX);
		const float TileMaxX =
			FMath::Lerp(Min.X, Max.X, static_cast<float>(X + 1) / TilesX);

		for (int32 Y = 0; Y < TilesY; ++Y)
		{
			const float TileMinY =
				FMath::Lerp(Min.Y, Max.Y, static_cast<float>(Y) / TilesY);
			const float TileMaxY =
				FMath::Lerp(Min.Y, Max.Y, static_cast<float>(Y + 1) / TilesY);
			const FVector2D HalfSize(
				(TileMaxX - TileMinX) * 0.5f,
				(TileMaxY - TileMinY) * 0.5f);

			TSharedPtr<FNoGrassCoverageTask, ESPMode::NotThreadSafe> Task =
				MakeShared<FNoGrassCoverageTask, ESPMode::NotThreadSafe>();
			Task->OwnerId = OwnerId;
			Task->Volume = Volume;
			Task->QueryCenter = FVector(
				(TileMinX + TileMaxX) * 0.5f,
				(TileMinY + TileMaxY) * 0.5f,
				Volume.WorldBounds.GetCenter().Z);
			// The foliage subsystem searches by instance origin. Candidate reach
			// therefore needs only the player-configured root-point buffer.
			const float CandidateQueryMargin = NoGrass::GetCandidateQueryMargin();
			Task->QueryRadius = HalfSize.Size() + CandidateQueryMargin;
			CoverageQueue.Enqueue(MoveTemp(Task));
		}
	}
}

// ── Coverage task processing ──────────────────────────────────────────────────

void UNoGrassWorldSubsystem::ProcessCoverageTasks(double Deadline)
{
	AFGFoliageRemovalSubsystem* FoliageSubsystem =
		AFGFoliageRemovalSubsystem::Get(GetWorld());
	if (!FoliageSubsystem)
	{
		return;
	}

	int32 InstanceTests = 0;
	const int32 MaxTests = FMath::Max(
		NoGrass::CVarMaxInstanceTestsPerFrame.GetValueOnGameThread(),
		1);

	while (FPlatformTime::Seconds() < Deadline && InstanceTests < MaxTests)
	{
		if (!ActiveCoverageTask.IsValid())
		{
			if (!CoverageQueue.Dequeue(ActiveCoverageTask))
			{
				return;
			}
		}

		if (!OwnerStates.Contains(ActiveCoverageTask->OwnerId))
		{
			ActiveCoverageTask.Reset();
			continue;
		}

		if (!ActiveCoverageTask->bCandidatesGathered)
		{
			GatherSuppressedCandidates(*ActiveCoverageTask);

			TMap<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>
				Candidates;
			FoliageSubsystem->GetFoliageWithinRadius(
				ActiveCoverageTask->QueryCenter,
				ActiveCoverageTask->QueryRadius,
				Candidates);

			for (TPair<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>&
				Pair : Candidates)
			{
				if (IsDecorativeGrass(Pair.Key))
				{
					FNoGrassCandidateBatch Batch;
					Batch.Component = Pair.Key;
					Batch.InstanceIndices = MoveTemp(Pair.Value);
					ActiveCoverageTask->CandidateBatches.Add(MoveTemp(Batch));
				}
			}

			ActiveCoverageTask->bCandidatesGathered = true;
		}

		if (ActiveCoverageTask->SuppressedCandidateCursor <
			ActiveCoverageTask->SuppressedCandidates.Num())
		{
			const FNoGrassInstanceKey& Key =
				ActiveCoverageTask->SuppressedCandidates[
					ActiveCoverageTask->SuppressedCandidateCursor++];
			++InstanceTests;

			FNoGrassSuppressionState* State = SuppressedGrass.Find(Key);
			UHierarchicalInstancedStaticMeshComponent* Component =
				Key.Component.Get();
			if (State &&
				Component &&
				!State->Owners.Contains(ActiveCoverageTask->OwnerId) &&
				GrassIntersectsVolume(
					State->OriginalWorldTransform,
					Component->GetStaticMesh(),
					ActiveCoverageTask->Volume))
			{
				State->Owners.Add(ActiveCoverageTask->OwnerId);
				OwnerStates.FindChecked(
					ActiveCoverageTask->OwnerId).GrassInstances.Add(Key);
			}

			continue;
		}

		while (ActiveCoverageTask->BatchCursor <
			ActiveCoverageTask->CandidateBatches.Num())
		{
			FNoGrassCandidateBatch& Batch =
				ActiveCoverageTask->CandidateBatches[
					ActiveCoverageTask->BatchCursor];
			UHierarchicalInstancedStaticMeshComponent* Component =
				Batch.Component.Get();

			if (!Component || Batch.Cursor >= Batch.InstanceIndices.Num())
			{
				++ActiveCoverageTask->BatchCursor;
				continue;
			}

			const int32 InstanceIndex = Batch.InstanceIndices[Batch.Cursor++];
			++InstanceTests;

			if (!Component->IsValidInstance(InstanceIndex))
			{
				break;
			}

			FTransform GrassTransform;
			if (!Component->GetInstanceTransform(InstanceIndex, GrassTransform, true))
			{
				break;
			}

			const FNoGrassInstanceKey ExistingKey{
				Component,
				QuantizeLocation(GrassTransform.GetLocation())};
			if (const FNoGrassSuppressionState* Existing =
				SuppressedGrass.Find(ExistingKey))
			{
				GrassTransform = Existing->OriginalWorldTransform;
			}

			if (GrassIntersectsVolume(
					GrassTransform,
					Component->GetStaticMesh(),
					ActiveCoverageTask->Volume))
			{
				SuppressGrass(
					ActiveCoverageTask->OwnerId,
					Component,
					InstanceIndex);
			}

			break;
		}

		if (ActiveCoverageTask->BatchCursor >=
			ActiveCoverageTask->CandidateBatches.Num())
		{
			ActiveCoverageTask.Reset();
		}
	}
}

void UNoGrassWorldSubsystem::GatherSuppressedCandidates(
	FNoGrassCoverageTask& Task) const
{
	const FIntPoint MinCell =
		SpatialCellForLocation(Task.Volume.WorldBounds.Min);
	const FIntPoint MaxCell =
		SpatialCellForLocation(Task.Volume.WorldBounds.Max);
	TSet<FNoGrassInstanceKey> UniqueCandidates;

	for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
	{
		for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
		{
			if (const TSet<FNoGrassInstanceKey>* Cell =
				SuppressedSpatialGrid.Find(FIntPoint(CellX, CellY)))
			{
				UniqueCandidates.Append(*Cell);
			}
		}
	}

	Task.SuppressedCandidates.Reserve(UniqueCandidates.Num());
	for (const FNoGrassInstanceKey& Key : UniqueCandidates)
	{
		Task.SuppressedCandidates.Add(Key);
	}
}
