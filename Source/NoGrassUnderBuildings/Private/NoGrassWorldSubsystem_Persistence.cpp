// Permanent (non-regrowing) grass coverage — save, replicate, and rebuild.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"
#include "NoGrassPermanentState.h"

#include "EngineUtils.h"
#include "Engine/World.h"

void UNoGrassWorldSubsystem::TryBindPermanentState()
{
	if (PermanentState.IsValid())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<ANoGrassPermanentState> It(World); It; ++It)
	{
		PermanentState = *It;
		LastPermanentVolumeCount = INDEX_NONE;
		return;
	}

	if (World->GetNetMode() != NM_Client &&
		World->GetTimeSeconds() >= PermanentStateSpawnAfter)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Name = TEXT("NoGrassUnderBuildings_PermanentState");
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		PermanentState = World->SpawnActor<ANoGrassPermanentState>(
			ANoGrassPermanentState::StaticClass(),
			FTransform::Identity,
			SpawnParameters);
		LastPermanentVolumeCount = INDEX_NONE;
	}
}

void UNoGrassWorldSubsystem::AddPermanentGrassCoverage(
	const TArray<FNoGrassCoverageVolume>& Volumes)
{
	TryBindPermanentState();
	ANoGrassPermanentState* State = PermanentState.Get();
	if (!State || !State->HasAuthority())
	{
		return;
	}

	TArray<TPair<FTransform, FBox>> SavedVolumes;
	for (const FNoGrassCoverageVolume& Volume : Volumes)
	{
		if (!Volume.bLandscapeOnly && Volume.LocalBounds.IsValid)
		{
			SavedVolumes.Emplace(Volume.LocalToWorld, Volume.LocalBounds);
		}
	}
	State->AddVolumes(SavedVolumes);
	// Debounce the more expensive persistent-volume rebuild while a player is
	// rapidly laying a large field of foundations.
	PermanentCoverageRefreshAt = GetWorld()->GetTimeSeconds() + 0.5;
}

void UNoGrassWorldSubsystem::RefreshPermanentCoverage()
{
	ANoGrassPermanentState* State = PermanentState.Get();
	if (!State)
	{
		return;
	}

	PermanentCoverageVolumes.Reset();
	PermanentCoverageSpatialGrid.Reset();
	for (const FNoGrassPermanentVolumeItem& Item : State->GetVolumes())
	{
		if (!Item.LocalToWorld.IsValid() || !Item.LocalBounds.IsValid)
		{
			continue;
		}
		FNoGrassCoverageVolume& Volume =
			PermanentCoverageVolumes.AddDefaulted_GetRef();
		Volume.LocalToWorld = Item.LocalToWorld;
		Volume.LocalBounds = Item.LocalBounds;
		Volume.WorldBounds = Item.LocalBounds.TransformBy(Item.LocalToWorld);
	}

	for (int32 Index = 0; Index < PermanentCoverageVolumes.Num(); ++Index)
	{
		const FBox Bounds =
			PermanentCoverageVolumes[Index].WorldBounds.ExpandBy(
				NoGrass::GetCandidateQueryMargin());
		const FIntPoint MinCell = SpatialCellForLocation(Bounds.Min);
		const FIntPoint MaxCell = SpatialCellForLocation(Bounds.Max);
		for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
		{
			for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
			{
				PermanentCoverageSpatialGrid.FindOrAdd(FIntPoint(X, Y)).Add(Index);
			}
		}
	}
	LastPermanentVolumeCount = State->GetVolumes().Num();

	FNoGrassOwnerState& PermanentOwner =
		OwnerStates.FindOrAdd(NoGrass::PermanentGrassOwnerId);
	PermanentOwner.CoverageVolumes = PermanentCoverageVolumes;
	for (const FNoGrassCoverageVolume& Volume : PermanentCoverageVolumes)
	{
		QueueCoverageVolume(NoGrass::PermanentGrassOwnerId, Volume);
	}
	ScheduleGeneratedGrassRefresh(PermanentCoverageVolumes, 0.05);
}
