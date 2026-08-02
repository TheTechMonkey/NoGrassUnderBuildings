#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "GameFramework/Info.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "NoGrassPermanentState.generated.h"

USTRUCT()
struct FNoGrassPermanentVolumeItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FTransform LocalToWorld = FTransform::Identity;

	UPROPERTY(SaveGame)
	FBox LocalBounds = FBox(ForceInit);
};

USTRUCT()
struct FNoGrassPermanentVolumeArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<FNoGrassPermanentVolumeItem> Items;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParams)
	{
		return FastArrayDeltaSerialize<
			FNoGrassPermanentVolumeItem,
			FNoGrassPermanentVolumeArray>(Items, DeltaParams, *this);
	}
};

template<>
struct TStructOpsTypeTraits<FNoGrassPermanentVolumeArray> :
	public TStructOpsTypeTraitsBase2<FNoGrassPermanentVolumeArray>
{
	enum
	{
		WithNetDeltaSerializer = true
	};
};

UCLASS()
class NOGRASSUNDERBUILDINGS_API ANoGrassPermanentState final :
	public AInfo,
	public IFGSaveInterface
{
	GENERATED_BODY()

public:
	ANoGrassPermanentState();

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	virtual void PreSaveGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	virtual void PostSaveGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	virtual void PreLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	virtual void PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion) override;
	virtual void GatherDependencies_Implementation(TArray<UObject*>& OutDependentObjects) override;
	virtual bool NeedTransform_Implementation() override;
	virtual bool ShouldSave_Implementation() const override;

	void AddVolumes(const TArray<TPair<FTransform, FBox>>& Volumes);
	const TArray<FNoGrassPermanentVolumeItem>& GetVolumes() const
	{
		return PermanentVolumes.Items;
	}

private:
	UPROPERTY(SaveGame, Replicated)
	FNoGrassPermanentVolumeArray PermanentVolumes;

	// Prevent duplicate placement callbacks and rebuilding in the same location
	// from growing the permanent save record forever.
	TSet<uint32> KnownVolumeHashes;
};
