// Owner release, grouped regrowth wave construction, and restore scheduling.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/World.h"

// ── Owner release ─────────────────────────────────────────────────────────────

void UNoGrassWorldSubsystem::ReleaseOwner(uint64 OwnerId)
{
	FNoGrassOwnerState OwnerState;
	if (!OwnerStates.RemoveAndCopyValue(OwnerId, OwnerState) || bShuttingDown)
	{
		return;
	}

	FNoGrassPendingOwnerRelease& Pending =
		PendingOwnerReleases.AddDefaulted_GetRef();
	Pending.OwnerId = OwnerId;
	Pending.OwnerState = MoveTemp(OwnerState);
	LastPendingOwnerReleaseAt = GetWorld()->GetTimeSeconds();
}

void UNoGrassWorldSubsystem::FlushPendingOwnerReleases()
{
	UWorld* World = GetWorld();
	if (!World || PendingOwnerReleases.IsEmpty() || bShuttingDown)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	if (Now - LastPendingOwnerReleaseAt < NoGrass::OwnerReleaseBatchDelay)
	{
		return;
	}

	TArray<FNoGrassPendingOwnerRelease> Releases = MoveTemp(PendingOwnerReleases);
	PendingOwnerReleases.Reset();
	LastPendingOwnerReleaseAt = -DBL_MAX;

	// ── Group touching releases into one shared regrowth wave ─────────────────
	// A zoop is reported as many independent buildables. Releases that arrive
	// together and physically touch are one visual wave; unrelated mass-
	// dismantled structures remain separate connected groups.
	const int32 ReleaseCount = Releases.Num();
	TArray<FBox> ReleaseBounds;
	ReleaseBounds.SetNum(ReleaseCount);
	for (int32 Index = 0; Index < ReleaseCount; ++Index)
	{
		FBox Bounds(ForceInit);
		for (const FNoGrassCoverageVolume& Volume :
			Releases[Index].OwnerState.CoverageVolumes)
		{
			if (!Volume.bLandscapeOnly && Volume.WorldBounds.IsValid)
			{
				Bounds += Volume.WorldBounds;
			}
		}
		ReleaseBounds[Index] = Bounds;
	}

	TArray<int32> GroupIds;
	GroupIds.Init(INDEX_NONE, ReleaseCount);
	int32 GroupCount = 0;
	for (int32 StartIndex = 0; StartIndex < ReleaseCount; ++StartIndex)
	{
		if (GroupIds[StartIndex] != INDEX_NONE)
		{
			continue;
		}

		const int32 GroupId = GroupCount++;
		GroupIds[StartIndex] = GroupId;
		TArray<int32> Open;
		Open.Add(StartIndex);
		while (!Open.IsEmpty())
		{
			const int32 Current = Open.Pop(EAllowShrinking::No);
			if (!ReleaseBounds[Current].IsValid)
			{
				continue;
			}

			const FBox Expanded =
				ReleaseBounds[Current].ExpandBy(FVector(NoGrass::RegrowthGroupingGap));
			for (int32 Candidate = 0; Candidate < ReleaseCount; ++Candidate)
			{
				if (GroupIds[Candidate] == INDEX_NONE &&
					ReleaseBounds[Candidate].IsValid &&
					Expanded.Intersect(ReleaseBounds[Candidate]))
				{
					GroupIds[Candidate] = GroupId;
					Open.Add(Candidate);
				}
			}
		}
	}

	const double MinimumDelay = FMath::Max(
		static_cast<double>(NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
		0.0);
	const double MaximumDelay = NoGrass::GetRegrowthMaximum(MinimumDelay);
	const bool bGradual =
		NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0 &&
		MaximumDelay > MinimumDelay + UE_SMALL_NUMBER;
	const float WavePadding = FMath::Max(
		FMath::Max(
			NoGrass::CVarHorizontalPadding.GetValueOnGameThread(),
			NoGrass::CVarCliffGrassPadding.GetValueOnGameThread()),
		EffectiveLandscapePadding);

	// ── Build per-group wave and schedule restores ────────────────────────────
	for (int32 GroupId = 0; GroupId < GroupCount; ++GroupId)
	{
		FNoGrassRegrowthWave Wave;
		for (int32 Index = 0; Index < ReleaseCount; ++Index)
		{
			if (GroupIds[Index] != GroupId)
			{
				continue;
			}

			for (const FNoGrassCoverageVolume& Volume :
				Releases[Index].OwnerState.CoverageVolumes)
			{
				if (Volume.bLandscapeOnly || !Volume.WorldBounds.IsValid)
				{
					continue;
				}

				if (!Wave.bValid)
				{
					const FVector Axis3D =
						Volume.LocalToWorld.TransformVectorNoScale(
							FVector::ForwardVector);
					Wave.AxisX = FVector2D(Axis3D.X, Axis3D.Y).GetSafeNormal();
					if (Wave.AxisX.IsNearlyZero())
					{
						Wave.AxisX = FVector2D(1.0, 0.0);
					}
					Wave.AxisY = FVector2D(-Wave.AxisX.Y, Wave.AxisX.X);
					Wave.Origin = FVector2D(
						Volume.WorldBounds.GetCenter().X,
						Volume.WorldBounds.GetCenter().Y);
					Wave.Min = FVector2D(DBL_MAX, DBL_MAX);
					Wave.Max = FVector2D(-DBL_MAX, -DBL_MAX);
					Wave.bValid = true;
				}

				const FBox ExpandedLocal =
					Volume.LocalBounds.ExpandBy(FVector(WavePadding, WavePadding, 0.0f));
				for (int32 Corner = 0; Corner < 8; ++Corner)
				{
					const FVector LocalCorner(
						(Corner & 1) ? ExpandedLocal.Max.X : ExpandedLocal.Min.X,
						(Corner & 2) ? ExpandedLocal.Max.Y : ExpandedLocal.Min.Y,
						(Corner & 4) ? ExpandedLocal.Max.Z : ExpandedLocal.Min.Z);
					const FVector WorldCorner =
						Volume.LocalToWorld.TransformPosition(LocalCorner);
					const FVector2D Offset(
						WorldCorner.X - Wave.Origin.X,
						WorldCorner.Y - Wave.Origin.Y);
					const FVector2D Projected(
						FVector2D::DotProduct(Offset, Wave.AxisX),
						FVector2D::DotProduct(Offset, Wave.AxisY));
					Wave.Min.X = FMath::Min(Wave.Min.X, Projected.X);
					Wave.Min.Y = FMath::Min(Wave.Min.Y, Projected.Y);
					Wave.Max.X = FMath::Max(Wave.Max.X, Projected.X);
					Wave.Max.Y = FMath::Max(Wave.Max.Y, Projected.Y);
				}
			}
		}

		int32 GroupOwnerCount = 0;
		int32 GroupOrdinaryCount = 0;
		int32 GroupGeneratedCount = 0;
		for (int32 Index = 0; Index < ReleaseCount; ++Index)
		{
			if (GroupIds[Index] != GroupId)
			{
				continue;
			}

			++GroupOwnerCount;
			FNoGrassPendingOwnerRelease& Release = Releases[Index];

			for (const FNoGrassCoverageVolume& Volume :
				Release.OwnerState.CoverageVolumes)
			{
				if (Volume.bLandscapeOnly || !Volume.WorldBounds.IsValid)
				{
					continue;
				}

				const uint32 Seed = HashCombineFast(
					GetTypeHash(Release.OwnerId),
					GetTypeHash(QuantizeLocation(Volume.WorldBounds.GetCenter())));
				FNoGrassCliffRegrowthVolume Regrowth;
				Regrowth.Volume = Volume;
				Regrowth.Wave = Wave;
				if (bGradual)
				{
					Regrowth.RestoreStartAt = Now + MinimumDelay;
					Regrowth.RestoreEndAt = Now + MaximumDelay;
				}
				else
				{
					const double Delay = NoGrass::GetRandomRestoreDelay(
						Seed, MinimumDelay, MaximumDelay);
					Regrowth.RestoreStartAt = Now + Delay;
					Regrowth.RestoreEndAt = Now + Delay;
				}
				Regrowth.NextVisualRefreshAt = Regrowth.RestoreStartAt;
				CliffRegrowthVolumes.Add(MoveTemp(Regrowth));
				++GroupGeneratedCount;
			}

			for (const FNoGrassInstanceKey& Key :
				Release.OwnerState.GrassInstances)
			{
				FNoGrassSuppressionState* GrassState = SuppressedGrass.Find(Key);
				if (!GrassState)
				{
					continue;
				}

				GrassState->Owners.Remove(Release.OwnerId);
				if (GrassState->Owners.IsEmpty())
				{
					const uint32 Seed = GetTypeHash(Key);
					const float RestoreAlpha = bGradual
						? NoGrass::GetRegrowthAlpha(
							Wave,
							GrassState->OriginalWorldTransform.GetLocation(),
							Seed)
						: -1.0f;
					const double Delay = RestoreAlpha >= 0.0f
						? FMath::Lerp(
							MinimumDelay,
							MaximumDelay,
							static_cast<double>(RestoreAlpha))
						: NoGrass::GetRandomRestoreDelay(
							Seed, MinimumDelay, MaximumDelay);
					RestoreQueue.Add({Key, Now + Delay, RestoreAlpha});
					bRestoreQueueDirty = true;
					++GroupOrdinaryCount;
				}
			}
		}

		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Queued grouped regrowth: buildings=%d ordinary=%d generated-patches=%d delay=%.0f-%.0f sec"),
			GroupOwnerCount,
			GroupOrdinaryCount,
			GroupGeneratedCount,
			MinimumDelay,
			MaximumDelay);
	}
}

// ── Restore processing ────────────────────────────────────────────────────────

void UNoGrassWorldSubsystem::ProcessRestores(double Deadline)
{
	if (RestoreQueue.IsEmpty())
	{
		return;
	}

	if (bRestoreQueueDirty)
	{
		RestoreQueue.Sort(
			[](const FNoGrassRestoreEntry& A, const FNoGrassRestoreEntry& B)
			{
				return A.RestoreAt > B.RestoreAt;
			});
		bRestoreQueueDirty = false;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	int32 RestoredThisFrame = 0;

	while (!RestoreQueue.IsEmpty() &&
		RestoreQueue.Last().RestoreAt <= Now &&
		RestoredThisFrame < 32 &&
		FPlatformTime::Seconds() < Deadline)
	{
		const FNoGrassRestoreEntry Entry = RestoreQueue.Pop(EAllowShrinking::No);
		FNoGrassSuppressionState* State = SuppressedGrass.Find(Entry.Key);
		if (!State || !State->Owners.IsEmpty())
		{
			continue;
		}

		UHierarchicalInstancedStaticMeshComponent* Component =
			Entry.Key.Component.Get();
		if (!Component)
		{
			RemoveSuppressedFromSpatialGrid(Entry.Key);
			SuppressedGrass.Remove(Entry.Key);
			continue;
		}

		bool bRestored = false;
		if (State->bUsePhysicalRemoval)
		{
			if (State->bPhysicallyRemoved)
			{
				const int32 AddedIndex = Component->AddInstance(
					State->OriginalWorldTransform,
					true);
				bRestored = AddedIndex != INDEX_NONE;
				if (bRestored)
				{
					DirtyComponents.Add(Component);
				}
			}
			else
			{
				// The building was dismantled before the deferred physical
				// removal ran, so the original instance never left the component.
				bRestored = true;
			}
		}
		else
		{
			const int32 InstanceIndex =
				ResolveInstanceIndex(Entry.Key, *State);
			if (InstanceIndex != INDEX_NONE)
			{
				bRestored = Component->UpdateInstanceTransform(
					InstanceIndex,
					State->OriginalWorldTransform,
					true,
					false,
					true);
				if (bRestored)
				{
					DirtyComponents.Add(Component);
				}
			}
		}

		if (!bRestored)
		{
			RestoreQueue.Add({Entry.Key, Now + 1.0, Entry.RestoreAlpha});
			bRestoreQueueDirty = true;
			continue;
		}
		++RestoredThisFrame;

		RemoveSuppressedFromSpatialGrid(Entry.Key);
		SuppressedGrass.Remove(Entry.Key);
	}
}

void UNoGrassWorldSubsystem::RebasePendingRestores(
	double Now,
	double NewMinimumDelay,
	double NewMaximumDelay)
{
	for (FNoGrassRestoreEntry& Entry : RestoreQueue)
	{
		const uint32 Seed = GetTypeHash(Entry.Key);
		const bool bGradual =
			NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0 &&
			Entry.RestoreAlpha >= 0.0f;
		Entry.RestoreAt = Now + (bGradual
			? FMath::Lerp(
				NewMinimumDelay,
				NewMaximumDelay,
				static_cast<double>(Entry.RestoreAlpha))
			: NoGrass::GetRandomRestoreDelay(
				Seed, NewMinimumDelay, NewMaximumDelay));
	}
	bRestoreQueueDirty |= !RestoreQueue.IsEmpty();

	for (FNoGrassCliffRegrowthVolume& Entry : CliffRegrowthVolumes)
	{
		const uint32 Seed = HashCombineFast(
			GetTypeHash(QuantizeLocation(Entry.Volume.WorldBounds.GetCenter())),
			GetTypeHash(QuantizeLocation(Entry.Volume.WorldBounds.GetExtent())));
		if (NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0 &&
			NewMaximumDelay > NewMinimumDelay + UE_SMALL_NUMBER)
		{
			Entry.RestoreStartAt = Now + NewMinimumDelay;
			Entry.RestoreEndAt = Now + NewMaximumDelay;
		}
		else
		{
			const double Delay = NoGrass::GetRandomRestoreDelay(
				Seed, NewMinimumDelay, NewMaximumDelay);
			Entry.RestoreStartAt = Now + Delay;
			Entry.RestoreEndAt = Now + Delay;
		}
		Entry.NextVisualRefreshAt = Entry.RestoreStartAt;
	}
}
