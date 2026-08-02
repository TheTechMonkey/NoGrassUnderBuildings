// Building event handlers, regional scan pipeline, and owner management.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"

#include "Buildables/FGBuildable.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "Engine/World.h"

// ── Event handlers ────────────────────────────────────────────────────────────

void UNoGrassWorldSubsystem::HandleBuildableAdded(AFGBuildable* Buildable)
{
	if (GetWorld()->GetNetMode() != NM_DedicatedServer)
	{
		QueueActorCoverage(Buildable, true);
	}
}

void UNoGrassWorldSubsystem::HandleActorPlaced(
	AFGBuildable* Buildable,
	AActor* BuildEffectInstigator)
{
	(void)BuildEffectInstigator;
	if (!IsValid(Buildable) || bShuttingDown ||
		GetWorld()->GetNetMode() == NM_Client)
	{
		return;
	}

	const FNoGrassConfigurationStruct Config =
		FNoGrassConfigurationStruct::GetActiveConfig(this);
	if (Config.RegrowGrass)
	{
		return;
	}

	TArray<FNoGrassCoverageVolume> Volumes;
	BuildCoverageForActor(Buildable, Volumes);
	AddPermanentGrassCoverage(Volumes);
}

void UNoGrassWorldSubsystem::HandleBuildableRemoved(AFGBuildable* Buildable)
{
	if (!Buildable)
	{
		return;
	}

	const TWeakObjectPtr<AFGBuildable> Key(Buildable);
	if (const uint64* OwnerId = ActorOwners.Find(Key))
	{
		const uint64 OwnerIdCopy = *OwnerId;
		ActorOwners.Remove(Key);
		ReleaseOwner(OwnerIdCopy);
	}
}

void UNoGrassWorldSubsystem::HandleLightweightAdded(
	AFGLightweightBuildableSubsystem* InLightweightSubsystem,
	TSubclassOf<AFGBuildable> BuildableClass,
	int32 RuntimeIndex,
	bool bFromSaveData,
	AActor* BuildEffectInstigator)
{
	(void)BuildEffectInstigator;
	if (!bShuttingDown)
	{
		if (GetWorld()->GetNetMode() != NM_DedicatedServer)
		{
			QueueLightweightCoverage(
				InLightweightSubsystem,
				BuildableClass,
				RuntimeIndex,
				true);
		}

		if (!bFromSaveData && GetWorld()->GetNetMode() != NM_Client)
		{
			const FNoGrassConfigurationStruct Config =
				FNoGrassConfigurationStruct::GetActiveConfig(this);
			if (!Config.RegrowGrass)
			{
				TArray<FNoGrassCoverageVolume> Volumes;
				BuildCoverageForLightweight(
					InLightweightSubsystem,
					BuildableClass,
					RuntimeIndex,
					Volumes);
				AddPermanentGrassCoverage(Volumes);
			}
		}
	}
}

void UNoGrassWorldSubsystem::HandleLightweightRemoved(
	AFGLightweightBuildableSubsystem* InLightweightSubsystem,
	TSubclassOf<AFGBuildable> BuildableClass,
	int32 RuntimeIndex)
{
	const FNoGrassLightweightOwnerKey Key{
		InLightweightSubsystem,
		BuildableClass.Get(),
		RuntimeIndex};

	if (const uint64* OwnerId = LightweightOwners.Find(Key))
	{
		const uint64 OwnerIdCopy = *OwnerId;
		LightweightOwners.Remove(Key);
		ReleaseOwner(OwnerIdCopy);
	}
}

// ── Regional scan ─────────────────────────────────────────────────────────────

void UNoGrassWorldSubsystem::StartRegionalScan(const FVector& ScanOrigin)
{
	ActiveScanOrigin = ScanOrigin;
	LastRegionalScanOrigin = ScanOrigin;
	NextRegionalScanAt =
		GetWorld()->GetTimeSeconds() +
		FMath::Max(NoGrass::CVarRescanInterval.GetValueOnGameThread(), 2.0f);

	const float Radius =
		FMath::Max(NoGrass::CVarScanRadius.GetValueOnGameThread(), 5000.0f);

	if (AFGBuildableSubsystem* BoundBuildableSubsystem = BuildableSubsystem.Get())
	{
		TArray<AFGBuildable*> NearbyBuildables;
		BoundBuildableSubsystem->GetCollidingBuildablesInBoundingBox(
			NearbyBuildables,
			FBox::BuildAABB(ScanOrigin, FVector(Radius)));

		ActorScanBuildables.Reserve(NearbyBuildables.Num());
		for (AFGBuildable* Buildable : NearbyBuildables)
		{
			ActorScanBuildables.Add(Buildable);
		}
		bActorScanActive = !ActorScanBuildables.IsEmpty();
	}

	LightweightScanClasses.Reset();
	LightweightScanClassCursor = 0;
	LightweightScanInstanceCursor = 0;
	bLightweightScanActive = false;

	if (AFGLightweightBuildableSubsystem* BoundLightweightSubsystem =
		LightweightSubsystem.Get())
	{
		BoundLightweightSubsystem->GetAllLightweightBuildableInstances().GetKeys(
			LightweightScanClasses);
		bLightweightScanActive = LightweightScanClasses.Num() > 0;
	}
}

void UNoGrassWorldSubsystem::ProcessActorScan(double Deadline)
{
	if (!bActorScanActive)
	{
		return;
	}

	int32 EntriesVisited = 0;
	while (ActorScanCursor < ActorScanBuildables.Num() &&
		EntriesVisited < NoGrass::MaxActorScanEntriesPerFrame &&
		FPlatformTime::Seconds() < Deadline)
	{
		QueueActorCoverage(ActorScanBuildables[ActorScanCursor++].Get());
		++EntriesVisited;
	}

	if (ActorScanCursor >= ActorScanBuildables.Num())
	{
		ActorScanBuildables.Reset();
		ActorScanCursor = 0;
		bActorScanActive = false;
	}
}

void UNoGrassWorldSubsystem::ProcessLightweightScan(double Deadline)
{
	AFGLightweightBuildableSubsystem* BoundLightweightSubsystem =
		LightweightSubsystem.Get();
	if (!bLightweightScanActive || !BoundLightweightSubsystem)
	{
		return;
	}

	const auto& AllInstances =
		BoundLightweightSubsystem->GetAllLightweightBuildableInstances();
	const float ScanRadius =
		FMath::Max(NoGrass::CVarScanRadius.GetValueOnGameThread(), 5000.0f);
	const float ScanRadiusSquared = FMath::Square(ScanRadius);
	int32 EntriesVisited = 0;

	while (LightweightScanClassCursor < LightweightScanClasses.Num() &&
		EntriesVisited < NoGrass::MaxLightweightScanEntriesPerFrame &&
		FPlatformTime::Seconds() < Deadline)
	{
		const TSubclassOf<AFGBuildable> BuildableClass =
			LightweightScanClasses[LightweightScanClassCursor];
		const TArray<FRuntimeBuildableInstanceData>* Instances =
			AllInstances.Find(BuildableClass);

		if (!Instances)
		{
			++LightweightScanClassCursor;
			LightweightScanInstanceCursor = 0;
			continue;
		}

		while (LightweightScanInstanceCursor < Instances->Num() &&
			EntriesVisited < NoGrass::MaxLightweightScanEntriesPerFrame &&
			FPlatformTime::Seconds() < Deadline)
		{
			const int32 RuntimeIndex = LightweightScanInstanceCursor++;
			++EntriesVisited;

			const FRuntimeBuildableInstanceData& RuntimeData =
				(*Instances)[RuntimeIndex];
			if (!RuntimeData.IsValid())
			{
				continue;
			}

			if (FVector::DistSquared2D(
					RuntimeData.Transform.GetLocation(),
					ActiveScanOrigin) <= ScanRadiusSquared)
			{
				QueueLightweightCoverage(
					BoundLightweightSubsystem,
					BuildableClass,
					RuntimeIndex);
			}
		}

		if (LightweightScanInstanceCursor >= Instances->Num())
		{
			++LightweightScanClassCursor;
			LightweightScanInstanceCursor = 0;
		}
	}

	if (LightweightScanClassCursor >= LightweightScanClasses.Num())
	{
		bLightweightScanActive = false;
		LightweightScanClasses.Reset();
	}
}

// ── Owner management ──────────────────────────────────────────────────────────

uint64 UNoGrassWorldSubsystem::FindOrAddActorOwner(AFGBuildable* Buildable)
{
	const TWeakObjectPtr<AFGBuildable> Key(Buildable);
	if (const uint64* Existing = ActorOwners.Find(Key))
	{
		return *Existing;
	}

	const uint64 OwnerId = NextOwnerId++;
	ActorOwners.Add(Key, OwnerId);
	OwnerStates.Add(OwnerId);
	return OwnerId;
}

uint64 UNoGrassWorldSubsystem::FindOrAddLightweightOwner(
	const FNoGrassLightweightOwnerKey& Key)
{
	if (const uint64* Existing = LightweightOwners.Find(Key))
	{
		return *Existing;
	}

	const uint64 OwnerId = NextOwnerId++;
	LightweightOwners.Add(Key, OwnerId);
	OwnerStates.Add(OwnerId);
	return OwnerId;
}

void UNoGrassWorldSubsystem::QueueActorCoverage(AFGBuildable* Buildable, bool bForce)
{
	if (!IsValid(Buildable) || Buildable->GetWorld() != GetWorld())
	{
		return;
	}

	const uint64 OwnerId = FindOrAddActorOwner(Buildable);
	FNoGrassOwnerState* OwnerState = OwnerStates.Find(OwnerId);
	if (!OwnerState)
	{
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	if (!bForce && Now - OwnerState->LastCoverageQueuedAt < 8.0)
	{
		return;
	}
	OwnerState->LastCoverageQueuedAt = Now;

	TArray<FNoGrassCoverageVolume> Volumes;
	BuildCoverageForActor(Buildable, Volumes);
	const bool bNeedsCliffRefresh = OwnerState->CoverageVolumes.IsEmpty() || bForce;
	OwnerState->CoverageVolumes = Volumes;
	if (bNeedsCliffRefresh)
	{
		ScheduleGeneratedGrassRefresh(Volumes);
	}
	for (const FNoGrassCoverageVolume& Volume : Volumes)
	{
		QueueCoverageVolume(OwnerId, Volume);
	}
}

void UNoGrassWorldSubsystem::QueueLightweightCoverage(
	AFGLightweightBuildableSubsystem* InLightweightSubsystem,
	TSubclassOf<AFGBuildable> BuildableClass,
	int32 RuntimeIndex,
	bool bForce)
{
	if (!IsValid(InLightweightSubsystem) || !BuildableClass ||
		RuntimeIndex == INDEX_NONE)
	{
		return;
	}

	const FNoGrassLightweightOwnerKey Key{
		InLightweightSubsystem,
		BuildableClass.Get(),
		RuntimeIndex};
	const uint64 OwnerId = FindOrAddLightweightOwner(Key);
	FNoGrassOwnerState* OwnerState = OwnerStates.Find(OwnerId);
	if (!OwnerState)
	{
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	if (!bForce && Now - OwnerState->LastCoverageQueuedAt < 8.0)
	{
		return;
	}
	OwnerState->LastCoverageQueuedAt = Now;

	TArray<FNoGrassCoverageVolume> Volumes;
	BuildCoverageForLightweight(
		InLightweightSubsystem,
		BuildableClass,
		RuntimeIndex,
		Volumes);
	const bool bNeedsCliffRefresh = OwnerState->CoverageVolumes.IsEmpty() || bForce;
	OwnerState->CoverageVolumes = Volumes;
	if (bNeedsCliffRefresh)
	{
		ScheduleGeneratedGrassRefresh(Volumes);
	}
	for (const FNoGrassCoverageVolume& Volume : Volumes)
	{
		QueueCoverageVolume(OwnerId, Volume);
	}
}
