// Regrowth status query and per-building coverage diagnostic output.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"

#include "Buildables/FGBuildable.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"

FString UNoGrassWorldSubsystem::GetRegrowthStatus() const
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const double MinimumDelay = FMath::Max(
		static_cast<double>(NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
		0.0);
	const double MaximumDelay = NoGrass::GetRegrowthMaximum(MinimumDelay);

	double NextRestoreAt = DBL_MAX;
	for (const FNoGrassRestoreEntry& Entry : RestoreQueue)
	{
		NextRestoreAt = FMath::Min(NextRestoreAt, Entry.RestoreAt);
	}
	for (const FNoGrassCliffRegrowthVolume& Entry : CliffRegrowthVolumes)
	{
		NextRestoreAt = FMath::Min(NextRestoreAt, Entry.RestoreStartAt);
	}

	const int32 PendingTotal =
		RestoreQueue.Num() + CliffRegrowthVolumes.Num();
	const FString NextRestoreText =
		PendingTotal > 0 && NextRestoreAt != DBL_MAX
		? FString::Printf(TEXT("%.1f sec"), FMath::Max(NextRestoreAt - Now, 0.0))
		: TEXT("none");

	return FString::Printf(
		TEXT("NoGrass regrowth: timer %.0f-%.0f sec | next %s | pending ordinary %d, generated patches %d | active buildings %d"),
		MinimumDelay,
		MaximumDelay,
		*NextRestoreText,
		RestoreQueue.Num(),
		CliffRegrowthVolumes.Num(),
		OwnerStates.Num());
}

void UNoGrassWorldSubsystem::AppendCoverageDiagnostics(
	const FVector& ScanCenter,
	float ScanRadius,
	TArray<FString>& OutLines) const
{
	AFGLightweightBuildableSubsystem* BoundLightweightSubsystem =
		LightweightSubsystem.Get();
	AFGFoliageRemovalSubsystem* FoliageSubsystem =
		AFGFoliageRemovalSubsystem::Get(GetWorld());
	if (!BoundLightweightSubsystem || !FoliageSubsystem)
	{
		OutLines.Add(
			TEXT("[CoverageDiagnostic] Required runtime subsystem unavailable."));
		return;
	}

	const float DiagnosticReach =
		FMath::Max(ScanRadius, 200.0f) + NoGrass::GetCandidateQueryMargin();
	const float DiagnosticReachSquared = FMath::Square(DiagnosticReach);

	TMap<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>> Candidates;
	FoliageSubsystem->GetFoliageWithinRadius(
		ScanCenter,
		DiagnosticReach,
		Candidates);

	const auto& AllInstances =
		BoundLightweightSubsystem->GetAllLightweightBuildableInstances();
	int32 NearbyBuildableCount = 0;
	int32 MatchingGrassCount = 0;

	for (const TPair<
		TSubclassOf<AFGBuildable>,
		TArray<FRuntimeBuildableInstanceData>>& ClassInstances : AllInstances)
	{
		for (int32 RuntimeIndex = 0;
			RuntimeIndex < ClassInstances.Value.Num();
			++RuntimeIndex)
		{
			const FRuntimeBuildableInstanceData& RuntimeData =
				ClassInstances.Value[RuntimeIndex];
			if (!RuntimeData.IsValid() ||
				FVector::DistSquared(
					RuntimeData.Transform.GetLocation(),
					ScanCenter) > DiagnosticReachSquared)
			{
				continue;
			}

			TArray<FNoGrassCoverageVolume> Volumes;
			BuildCoverageForLightweight(
				BoundLightweightSubsystem,
				ClassInstances.Key,
				RuntimeIndex,
				Volumes);
			if (Volumes.IsEmpty())
			{
				continue;
			}

			++NearbyBuildableCount;
			const FNoGrassLightweightOwnerKey OwnerKey{
				BoundLightweightSubsystem,
				ClassInstances.Key.Get(),
				RuntimeIndex};
			const uint64* OwnerId = LightweightOwners.Find(OwnerKey);
			const FNoGrassOwnerState* OwnerState =
				OwnerId ? OwnerStates.Find(*OwnerId) : nullptr;

			TSet<FNoGrassInstanceKey> MatchingInstances;
			int32 OrdinaryVolumeCount = 0;
			for (const FNoGrassCoverageVolume& Volume : Volumes)
			{
				if (Volume.bLandscapeOnly)
				{
					continue;
				}
				++OrdinaryVolumeCount;

				for (const TPair<
					UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>&
					CandidateBatch : Candidates)
				{
					UHierarchicalInstancedStaticMeshComponent* Component =
						CandidateBatch.Key;
					if (!IsDecorativeGrass(Component))
					{
						continue;
					}

					for (int32 InstanceIndex : CandidateBatch.Value)
					{
						FTransform CurrentTransform;
						if (!Component->GetInstanceTransform(
								InstanceIndex,
								CurrentTransform,
								true))
						{
							continue;
						}

						const FNoGrassInstanceKey InstanceKey{
							Component,
							QuantizeLocation(CurrentTransform.GetLocation())};
						const FNoGrassSuppressionState* Suppression =
							SuppressedGrass.Find(InstanceKey);
						const FTransform& TestTransform = Suppression
							? Suppression->OriginalWorldTransform
							: CurrentTransform;
						if (GrassIntersectsVolume(
								TestTransform,
								Component->GetStaticMesh(),
								Volume))
						{
							MatchingInstances.Add(InstanceKey);
						}
					}
				}
			}

			OutLines.Add(FString::Printf(
				TEXT("[CoverageDiagnostic] BuildableClass=%s | RuntimeIndex=%d | Location=%s | TrackedOwner=%s | OwnerGrass=%d | CoverageVolumes=%d | OrdinaryVolumes=%d | ExactGrassMatches=%d"),
				*GetPathNameSafe(ClassInstances.Key.Get()),
				RuntimeIndex,
				*RuntimeData.Transform.GetLocation().ToCompactString(),
				OwnerId ? TEXT("yes") : TEXT("no"),
				OwnerState ? OwnerState->GrassInstances.Num() : 0,
				Volumes.Num(),
				OrdinaryVolumeCount,
				MatchingInstances.Num()));

			for (const FNoGrassInstanceKey& InstanceKey : MatchingInstances)
			{
				UHierarchicalInstancedStaticMeshComponent* Component =
					InstanceKey.Component.Get();
				const FNoGrassSuppressionState* Suppression =
					SuppressedGrass.Find(InstanceKey);
				const bool bAttachedToOwner =
					Suppression && OwnerId &&
					Suppression->Owners.Contains(*OwnerId);
				const int32 ResolvedIndex = Suppression
					? ResolveInstanceIndex(InstanceKey, *Suppression)
					: INDEX_NONE;
				FTransform CurrentTransform;
				const bool bHasCurrentTransform =
					Component && ResolvedIndex != INDEX_NONE &&
					Component->GetInstanceTransform(
						ResolvedIndex,
						CurrentTransform,
						true);

				++MatchingGrassCount;
				OutLines.Add(FString::Printf(
					TEXT("[CoverageDiagnosticMatch] Mesh=%s | QuantizedLocation=%s | SuppressionState=%s | AttachedToOwner=%s | ResolvedIndex=%d | OriginalScale=%s | CurrentScale=%s"),
					*GetPathNameSafe(Component ? Component->GetStaticMesh() : nullptr),
					*InstanceKey.QuantizedLocation.ToString(),
					Suppression ? TEXT("yes") : TEXT("no"),
					bAttachedToOwner ? TEXT("yes") : TEXT("no"),
					ResolvedIndex,
					Suppression
						? *Suppression->OriginalWorldTransform.GetScale3D().ToCompactString()
						: TEXT("unavailable"),
					bHasCurrentTransform
						? *CurrentTransform.GetScale3D().ToCompactString()
						: TEXT("unavailable")));
			}
		}
	}

	OutLines.Add(FString::Printf(
		TEXT("[CoverageDiagnosticSummary] NearbyBuildables=%d | ExactGrassMatches=%d | CandidateComponents=%d"),
		NearbyBuildableCount,
		MatchingGrassCount,
		Candidates.Num()));
}
