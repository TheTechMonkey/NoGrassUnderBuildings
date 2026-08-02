#include "NoGrassPermanentState.h"

#include "Net/UnrealNetwork.h"

namespace
{
	uint32 MakePermanentVolumeHash(const FTransform& Transform, const FBox& Bounds)
	{
		const FBox WorldBounds = Bounds.TransformBy(Transform);
		const FVector Center = WorldBounds.GetCenter();
		const FVector Extent = WorldBounds.GetExtent();
		const FRotator Rotation = Transform.Rotator();
		uint32 Hash = GetTypeHash(FIntVector(
			FMath::RoundToInt(Center.X),
			FMath::RoundToInt(Center.Y),
			FMath::RoundToInt(Center.Z)));
		Hash = HashCombineFast(Hash, GetTypeHash(FIntVector(
			FMath::RoundToInt(Extent.X),
			FMath::RoundToInt(Extent.Y),
			FMath::RoundToInt(Extent.Z))));
		return HashCombineFast(Hash, GetTypeHash(FIntVector(
			FMath::RoundToInt(Rotation.Roll * 10.0),
			FMath::RoundToInt(Rotation.Pitch * 10.0),
			FMath::RoundToInt(Rotation.Yaw * 10.0))));
	}
}

ANoGrassPermanentState::ANoGrassPermanentState()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(1.0f);
	SetReplicatingMovement(false);
}

void ANoGrassPermanentState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANoGrassPermanentState, PermanentVolumes);
}

void ANoGrassPermanentState::AddVolumes(
	const TArray<TPair<FTransform, FBox>>& Volumes)
{
	if (!HasAuthority())
	{
		return;
	}
	if (KnownVolumeHashes.Num() != PermanentVolumes.Items.Num())
	{
		KnownVolumeHashes.Empty(PermanentVolumes.Items.Num());
		for (const FNoGrassPermanentVolumeItem& Existing : PermanentVolumes.Items)
		{
			KnownVolumeHashes.Add(MakePermanentVolumeHash(
				Existing.LocalToWorld,
				Existing.LocalBounds));
		}
	}

	bool bAddedAny = false;
	for (const TPair<FTransform, FBox>& Volume : Volumes)
	{
		if (!Volume.Key.IsValid() || !Volume.Value.IsValid)
		{
			continue;
		}
		const uint32 VolumeHash = MakePermanentVolumeHash(Volume.Key, Volume.Value);
		if (KnownVolumeHashes.Contains(VolumeHash))
		{
			continue;
		}
		KnownVolumeHashes.Add(VolumeHash);
		FNoGrassPermanentVolumeItem& Item = PermanentVolumes.Items.AddDefaulted_GetRef();
		Item.LocalToWorld = Volume.Key;
		Item.LocalBounds = Volume.Value;
		PermanentVolumes.MarkItemDirty(Item);
		bAddedAny = true;
	}
	if (bAddedAny)
	{
		ForceNetUpdate();
	}
}

void ANoGrassPermanentState::PreSaveGame_Implementation(int32, int32) {}
void ANoGrassPermanentState::PostSaveGame_Implementation(int32, int32) {}
void ANoGrassPermanentState::PreLoadGame_Implementation(int32, int32) {}
void ANoGrassPermanentState::PostLoadGame_Implementation(int32, int32)
{
	KnownVolumeHashes.Reset();
	PermanentVolumes.MarkArrayDirty();
}
void ANoGrassPermanentState::GatherDependencies_Implementation(TArray<UObject*>&) {}
bool ANoGrassPermanentState::NeedTransform_Implementation() { return false; }
bool ANoGrassPermanentState::ShouldSave_Implementation() const { return true; }
