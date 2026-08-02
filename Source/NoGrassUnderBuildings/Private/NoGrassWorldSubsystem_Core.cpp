// Subsystem lifecycle, tick orchestration, and configuration.

#include "NoGrassInternal.h"
#include "NoGrassUnderBuildings.h"
#include "NoGrassPermanentState.h"

#include "Buildables/FGBuildable.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

bool UNoGrassWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}
	return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
}

void UNoGrassWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bShuttingDown = false;
	NextRegionalScanAt = 0.0;
	NextLandscapeReachScanAt = 0.0;
	NextConfigurationRefreshAt = 0.0;
	PermanentStateSpawnAfter = 1.0;
	LastPermanentVolumeCount = INDEX_NONE;
	PermanentCoverageRefreshAt = DBL_MAX;
	ApplyConfiguration();
	LastObservedRegrowthSeconds = FMath::Max(
		static_cast<double>(NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
		0.0);
	LastObservedRegrowthMaxSeconds =
		NoGrass::GetRegrowthMaximum(LastObservedRegrowthSeconds);
	LastObservedGradualRegrowth =
		NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0;
	EffectiveLandscapePadding = FMath::Max(
		NoGrass::CVarLandscapePadding.GetValueOnGameThread(),
		0.0f);
	EffectiveLandscapeVerticalPadding = FMath::Max(
		NoGrass::CVarLandscapeVerticalPadding.GetValueOnGameThread(),
		0.0f);
	LastObservedCliffGrassPadding = FMath::Max(
		NoGrass::CVarCliffGrassPadding.GetValueOnGameThread(),
		0.0f);
}

void UNoGrassWorldSubsystem::Deinitialize()
{
	bShuttingDown = true;

	if (AFGBuildableSubsystem* BoundSubsystem = BuildableSubsystem.Get())
	{
		BoundSubsystem->mBuildableAddedDelegate.RemoveDynamic(
			this,
			&UNoGrassWorldSubsystem::HandleBuildableAdded);
		BoundSubsystem->mBuildableRemovedDelegate.RemoveDynamic(
			this,
			&UNoGrassWorldSubsystem::HandleBuildableRemoved);
	}

	BuildableSubsystem.Reset();
	LightweightSubsystem.Reset();
	ActorOwners.Empty();
	LightweightOwners.Empty();
	OwnerStates.Empty();
	SuppressedGrass.Empty();
	SuppressedSpatialGrid.Empty();
	PendingPhysicalRemovalComponents.Empty();
	DirtyComponents.Empty();
	PermanentState.Reset();
	PermanentCoverageVolumes.Empty();
	PermanentCoverageSpatialGrid.Empty();
	RestoreQueue.Empty();
	PendingOwnerReleases.Empty();
	CliffRegrowthVolumes.Empty();
	GeneratedGrassBufferSnapshots.Empty();
	PendingGeneratedGrassRefreshBounds.Init();
	NextGeneratedGrassRefreshAt = DBL_MAX;
	ActorScanBuildables.Empty();
	bActorScanActive = false;
	ActiveCoverageTask.Reset();
	TSharedPtr<FNoGrassCoverageTask, ESPMode::NotThreadSafe> DiscardedTask;
	while (CoverageQueue.Dequeue(DiscardedTask))
	{
	}

	Super::Deinitialize();
}

TStatId UNoGrassWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UNoGrassWorldSubsystem, STATGROUP_Tickables);
}

bool UNoGrassWorldSubsystem::IsTickable() const
{
	return !bShuttingDown && NoGrass::CVarEnabled.GetValueOnGameThread() != 0;
}

void UNoGrassWorldSubsystem::Tick(float)
{
	UWorld* World = GetWorld();
	if (!World || bShuttingDown)
	{
		return;
	}

	TryBindBuildableSubsystem();

	const double Now = World->GetTimeSeconds();
	if (Now >= NextConfigurationRefreshAt)
	{
		ApplyConfiguration();
		NextConfigurationRefreshAt = Now + 1.0;
	}
	TryBindPermanentState();
	if (ANoGrassPermanentState* State = PermanentState.Get())
	{
		if (State->GetVolumes().Num() != LastPermanentVolumeCount)
		{
			if (PermanentCoverageRefreshAt == DBL_MAX)
			{
				PermanentCoverageRefreshAt = Now + 0.5;
			}
			if (Now >= PermanentCoverageRefreshAt)
			{
				RefreshPermanentCoverage();
				PermanentCoverageRefreshAt = DBL_MAX;
			}
		}
	}

	const double CurrentRegrowthSeconds = FMath::Max(
		static_cast<double>(NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
		0.0);
	const double CurrentRegrowthMaxSeconds =
		NoGrass::GetRegrowthMaximum(CurrentRegrowthSeconds);
	const bool bCurrentGradualRegrowth =
		NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0;
	if (!FMath::IsNearlyEqual(CurrentRegrowthSeconds, LastObservedRegrowthSeconds) ||
		!FMath::IsNearlyEqual(CurrentRegrowthMaxSeconds, LastObservedRegrowthMaxSeconds) ||
		bCurrentGradualRegrowth != LastObservedGradualRegrowth)
	{
		RebasePendingRestores(Now, CurrentRegrowthSeconds, CurrentRegrowthMaxSeconds);
		LastObservedRegrowthSeconds = CurrentRegrowthSeconds;
		LastObservedRegrowthMaxSeconds = CurrentRegrowthMaxSeconds;
		LastObservedGradualRegrowth = bCurrentGradualRegrowth;
	}

	const float CurrentCliffGrassPadding = FMath::Max(
		NoGrass::CVarCliffGrassPadding.GetValueOnGameThread(),
		0.0f);
	if (!FMath::IsNearlyEqual(CurrentCliffGrassPadding, LastObservedCliffGrassPadding))
	{
		TArray<FNoGrassCoverageVolume> RefreshVolumes;
		for (const TPair<uint64, FNoGrassOwnerState>& Pair : OwnerStates)
		{
			RefreshVolumes.Append(Pair.Value.CoverageVolumes);
		}
		for (const FNoGrassCliffRegrowthVolume& Regrowth : CliffRegrowthVolumes)
		{
			RefreshVolumes.Add(Regrowth.Volume);
		}
		ScheduleGeneratedGrassRefresh(RefreshVolumes, 0.05);
		LastObservedCliffGrassPadding = CurrentCliffGrassPadding;
	}

	if (World->GetNetMode() != NM_DedicatedServer && Now >= NextLandscapeReachScanAt)
	{
		RefreshLandscapeGrassReach();
		NextLandscapeReachScanAt = Now + NoGrass::LandscapeReachScanInterval;
	}

	if (World->GetNetMode() != NM_DedicatedServer)
	{
		if (APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);

			const float MovementThreshold = FMath::Max(
				NoGrass::CVarRescanMovement.GetValueOnGameThread(),
				1000.0f);
			const bool bMovedFarEnough =
				FVector::DistSquared2D(ViewLocation, LastRegionalScanOrigin) >=
				FMath::Square(MovementThreshold);

			if (!bActorScanActive &&
				!bLightweightScanActive &&
				Now >= NextRegionalScanAt &&
				(bMovedFarEnough || LastRegionalScanOrigin.X == UE_BIG_NUMBER))
			{
				StartRegionalScan(ViewLocation);
			}
		}
	}

	const float BudgetMs = FMath::Clamp(
		NoGrass::CVarFrameBudgetMs.GetValueOnGameThread(),
		0.05f,
		5.0f);
	const double Deadline = FPlatformTime::Seconds() + BudgetMs / 1000.0;

	ProcessActorScan(Deadline);
	ProcessLightweightScan(Deadline);
	ProcessCoverageTasks(Deadline);
	FlushPendingOwnerReleases();
	ProcessRestores(Deadline);
	ProcessGeneratedGrassRegrowthAndRefresh(Deadline);
	FlushPendingPhysicalRemovals();
	FlushDirtyComponents();
}

void UNoGrassWorldSubsystem::TryBindBuildableSubsystem()
{
	if (BuildableSubsystem.IsValid() && LightweightSubsystem.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (!BuildableSubsystem.IsValid())
	{
		if (AFGBuildableSubsystem* NewBuildableSubsystem =
			AFGBuildableSubsystem::Get(World))
		{
			BuildableSubsystem = NewBuildableSubsystem;
			NewBuildableSubsystem->mBuildableAddedDelegate.AddUniqueDynamic(
				this,
				&UNoGrassWorldSubsystem::HandleBuildableAdded);
			NewBuildableSubsystem->mBuildableRemovedDelegate.AddUniqueDynamic(
				this,
				&UNoGrassWorldSubsystem::HandleBuildableRemoved);
			NextRegionalScanAt = 0.0;
		}
	}

	if (!LightweightSubsystem.IsValid())
	{
		LightweightSubsystem = AFGLightweightBuildableSubsystem::Get(World);
	}
}

void UNoGrassWorldSubsystem::ApplyConfiguration()
{
	const FNoGrassConfigurationStruct Config =
		FNoGrassConfigurationStruct::GetActiveConfig(this);
	NoGrass::CVarRegrowthSeconds.AsVariable()->Set(
		FMath::Clamp(Config.RegrowthMinSeconds, 0.0f, 86400.0f),
		ECVF_SetByGameSetting);
	NoGrass::CVarRegrowthMaxSeconds.AsVariable()->Set(
		FMath::Clamp(Config.RegrowthMaxSeconds, 0.0f, 86400.0f),
		ECVF_SetByGameSetting);
	NoGrass::CVarGradualRegrowth.AsVariable()->Set(
		Config.GradualRegrowth ? 1 : 0,
		ECVF_SetByGameSetting);

	const float HorizontalBuffer =
		FMath::Clamp(Config.HorizontalBufferMeters, 0.0f, 20.0f) * 100.0f;
	const float VerticalBuffer =
		FMath::Clamp(Config.VerticalBufferMeters, 0.0f, 20.0f) * 100.0f;
	NoGrass::CVarHorizontalPadding.AsVariable()->Set(
		HorizontalBuffer, ECVF_SetByGameSetting);
	NoGrass::CVarCliffGrassPadding.AsVariable()->Set(
		HorizontalBuffer, ECVF_SetByGameSetting);
	NoGrass::CVarLandscapePadding.AsVariable()->Set(
		HorizontalBuffer, ECVF_SetByGameSetting);
	NoGrass::CVarVerticalPadding.AsVariable()->Set(
		VerticalBuffer, ECVF_SetByGameSetting);
	NoGrass::CVarLandscapeVerticalPadding.AsVariable()->Set(
		VerticalBuffer, ECVF_SetByGameSetting);
}
