#include "NoGrassWorldSubsystem.h"

#include "NoGrassUnderBuildings.h"
#include "NoGrassPermanentState.h"

#include "AbstractInstanceManager.h"
#include "Buildables/FGBuildable.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGCliffActor.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FGBuildableSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "GameFramework/PlayerController.h"
#include "GrassInstancedStaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "InstanceData.h"
#include "LandscapeProxy.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderTransform.h"
#include "StaticMeshResources.h"

struct FNoGrassGeneratedGrassBufferSnapshot
{
	TArray<FClusterNode> ClusterTree;
	int32 OcclusionLayerNum = 0;
	int32 NumBuiltRenderInstances = 0;
	TUniquePtr<FStaticMeshInstanceData> InstanceData;
};

namespace NoGrass
{
	static TUniquePtr<FStaticMeshInstanceData> CloneInstanceData(
		const FStaticMeshInstanceData& Source)
	{
		TUniquePtr<FStaticMeshInstanceData> Clone =
			MakeUnique<FStaticMeshInstanceData>(
				Source.GetTranslationUsesHalfs());
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

	static TAutoConsoleVariable<int32> CVarEnabled(
		TEXT("NoGrassUnderBuildings.Enabled"),
		1,
		TEXT("Enables reversible decorative-grass suppression."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarFrameBudgetMs(
		TEXT("NoGrassUnderBuildings.FrameBudgetMs"),
		0.60f,
		TEXT("Maximum game-thread time used per frame."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarMaxInstanceTestsPerFrame(
		TEXT("NoGrassUnderBuildings.MaxInstanceTestsPerFrame"),
		192,
		TEXT("Hard cap on grass intersection tests per frame."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarRegrowthSeconds(
		TEXT("NoGrassUnderBuildings.RegrowthSeconds"),
		600.0f,
		TEXT("Minimum delay before grass starts returning after the final covering building is dismantled."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarRegrowthMaxSeconds(
		TEXT("NoGrassUnderBuildings.RegrowthMaxSeconds"),
		0.0f,
		TEXT("Maximum random regrowth delay. Zero automatically uses 1.5 times RegrowthSeconds."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarGradualRegrowth(
		TEXT("NoGrassUnderBuildings.GradualRegrowth"),
		1,
		TEXT("Restores touching dismantled areas in staged edge-to-center waves."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarScanRadius(
		TEXT("NoGrassUnderBuildings.ScanRadius"),
		30000.0f,
		TEXT("Radius around the local player processed for existing buildings, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarRescanInterval(
		TEXT("NoGrassUnderBuildings.RescanInterval"),
		15.0f,
		TEXT("Minimum seconds between streamed-area rescans."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarRescanMovement(
		TEXT("NoGrassUnderBuildings.RescanMovement"),
		5000.0f,
		TEXT("Player movement required to trigger a streamed-area rescan, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarHorizontalPadding(
		TEXT("NoGrassUnderBuildings.HorizontalPadding"),
		200.0f,
		TEXT("Horizontal ordinary-foliage coverage buffer around buildings, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarVerticalPadding(
		TEXT("NoGrassUnderBuildings.VerticalPadding"),
		200.0f,
		TEXT("Vertical ordinary-foliage coverage buffer above and below buildings, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarCliffGrassPadding(
		TEXT("NoGrassUnderBuildings.CliffGrassPadding"),
		100.0f,
		TEXT("Horizontal root-point buffer for render-only cliff grass, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarLandscapePadding(
		TEXT("NoGrassUnderBuildings.LandscapePadding"),
		200.0f,
		TEXT("Extra horizontal root-point buffer for leaning Landscape Grass, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarLandscapeVerticalPadding(
		TEXT("NoGrassUnderBuildings.LandscapeVerticalPadding"),
		200.0f,
		TEXT("Vertical Landscape Grass exclusion buffer above and below buildings, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<float> CVarMaxGrassExtent(
		TEXT("NoGrassUnderBuildings.MaxGrassExtent"),
		500.0f,
		TEXT("Maximum allowed grass-mesh extent on any axis, in centimeters."),
		ECVF_Default);

	static TAutoConsoleVariable<int32> CVarDebug(
		TEXT("NoGrassUnderBuildings.Debug"),
		0,
		TEXT("Logs accepted grass meshes and suppression activity."),
		ECVF_Default);

	constexpr float CoverageTileSize = 1200.0f;
	constexpr int32 MaxCoverageTilesPerVolume = 256;
	constexpr int32 MaxActorScanEntriesPerFrame = 64;
	constexpr int32 MaxLightweightScanEntriesPerFrame = 256;
	constexpr float MinimumCandidateQueryMargin = 100.0f;
	constexpr float InstanceResolveRadius = 10.0f;
	// Exact zero-scale transforms are not reliably consumed by every streamed
	// foliage render buffer. Some plant materials also apply world-position
	// animation after instance scaling. Keep a non-degenerate scale and move the
	// hidden instance well below the world so both render paths remain invisible.
	constexpr float HiddenInstanceScale = 0.001f;
	constexpr float HiddenInstanceDepth = 100000.0f;
	constexpr float HiddenAnimatedPlantOffset = 10000000.0f;
	constexpr double LandscapeReachScanInterval = 2.0;
	constexpr float LandscapeInstanceScaleSafety = 1.0f;
	constexpr float MaxAutomaticLandscapePadding = 200.0f;
	constexpr int32 GeneratedGrassRegrowthVisualSteps = 12;
	constexpr double OwnerReleaseBatchDelay = 0.20;
	constexpr float RegrowthGroupingGap = 100.0f;
	constexpr uint64 PermanentGrassOwnerId = MAX_uint64;

	static float GetCandidateQueryMargin()
	{
		return FMath::Max(
			FMath::Max(
				CVarHorizontalPadding.GetValueOnGameThread(),
				0.0f),
			MinimumCandidateQueryMargin);
	}

	static double GetRegrowthMaximum(double MinimumDelay)
	{
		const double ConfiguredMaximum = FMath::Max(
			static_cast<double>(
				CVarRegrowthMaxSeconds.GetValueOnGameThread()),
			0.0);
		return ConfiguredMaximum > 0.0
			? FMath::Max(ConfiguredMaximum, MinimumDelay)
			: MinimumDelay * 1.5;
	}

	static double GetRandomRestoreDelay(
		uint32 Seed,
		double MinimumDelay,
		double MaximumDelay)
	{
		const double UnitValue =
			static_cast<double>(Seed % 1000001u) / 1000000.0;
		return FMath::Lerp(MinimumDelay, MaximumDelay, UnitValue);
	}

	static float GetRegrowthAlpha(
		const FNoGrassRegrowthWave& Wave,
		const FVector& WorldLocation,
		uint32 Seed)
	{
		if (!Wave.bValid)
		{
			return -1.0f;
		}

		const FVector2D Offset(
			WorldLocation.X - Wave.Origin.X,
			WorldLocation.Y - Wave.Origin.Y);
		const FVector2D Point(
			FVector2D::DotProduct(Offset, Wave.AxisX),
			FVector2D::DotProduct(Offset, Wave.AxisY));
		const FVector2D Extent = (Wave.Max - Wave.Min) * 0.5;
		const double EdgeDepthX = FMath::Min(
			Point.X - Wave.Min.X,
			Wave.Max.X - Point.X) /
			FMath::Max(static_cast<double>(Extent.X), 1.0);
		const double EdgeDepthY = FMath::Min(
			Point.Y - Wave.Min.Y,
			Wave.Max.Y - Point.Y) /
			FMath::Max(static_cast<double>(Extent.Y), 1.0);
		const double EdgeDepth = FMath::Clamp(
			FMath::Min(EdgeDepthX, EdgeDepthY),
			0.0,
			1.0);
		const double Jitter =
			(static_cast<double>(Seed % 1001u) / 1000.0 - 0.5) * 0.07;
		return static_cast<float>(
			FMath::Clamp(EdgeDepth + Jitter, 0.0, 1.0));
	}

	static bool RequiresPhysicalRemoval(const UStaticMesh* GrassMesh)
	{
		if (!GrassMesh)
		{
			return false;
		}

		const FString MeshPath = GrassMesh->GetPathName();
		return MeshPath.Contains(
				TEXT("SM_CoverGround_01"),
				ESearchCase::IgnoreCase) ||
			MeshPath.Contains(
				TEXT("LowerVegetation_Plant_010"),
				ESearchCase::IgnoreCase) ||
			MeshPath.Contains(
				TEXT("LowerVegetation_Plant_017"),
				ESearchCase::IgnoreCase);
	}

}

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
		static_cast<double>(
			NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
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
		static_cast<double>(
			NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
		0.0);
	const double CurrentRegrowthMaxSeconds =
		NoGrass::GetRegrowthMaximum(CurrentRegrowthSeconds);
	const bool bCurrentGradualRegrowth =
		NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0;
	if (!FMath::IsNearlyEqual(
			CurrentRegrowthSeconds,
			LastObservedRegrowthSeconds) ||
		!FMath::IsNearlyEqual(
			CurrentRegrowthMaxSeconds,
			LastObservedRegrowthMaxSeconds) ||
		bCurrentGradualRegrowth != LastObservedGradualRegrowth)
	{
		RebasePendingRestores(
			Now,
			CurrentRegrowthSeconds,
			CurrentRegrowthMaxSeconds);
		LastObservedRegrowthSeconds = CurrentRegrowthSeconds;
		LastObservedRegrowthMaxSeconds = CurrentRegrowthMaxSeconds;
		LastObservedGradualRegrowth = bCurrentGradualRegrowth;
	}

	const float CurrentCliffGrassPadding = FMath::Max(
		NoGrass::CVarCliffGrassPadding.GetValueOnGameThread(),
		0.0f);
	if (!FMath::IsNearlyEqual(
		CurrentCliffGrassPadding,
		LastObservedCliffGrassPadding))
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

	if (World->GetNetMode() != NM_DedicatedServer &&
		Now >= NextLandscapeReachScanAt)
	{
		RefreshLandscapeGrassReach();
		NextLandscapeReachScanAt =
			Now + NoGrass::LandscapeReachScanInterval;
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
		HorizontalBuffer,
		ECVF_SetByGameSetting);
	NoGrass::CVarCliffGrassPadding.AsVariable()->Set(
		HorizontalBuffer,
		ECVF_SetByGameSetting);
	NoGrass::CVarLandscapePadding.AsVariable()->Set(
		HorizontalBuffer,
		ECVF_SetByGameSetting);
	NoGrass::CVarVerticalPadding.AsVariable()->Set(
		VerticalBuffer,
		ECVF_SetByGameSetting);
	NoGrass::CVarLandscapeVerticalPadding.AsVariable()->Set(
		VerticalBuffer,
		ECVF_SetByGameSetting);
}

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
		const FBox Bounds = PermanentCoverageVolumes[Index].WorldBounds.ExpandBy(
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
		if (AFGBuildableSubsystem* NewBuildableSubsystem = AFGBuildableSubsystem::Get(World))
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

void UNoGrassWorldSubsystem::StartRegionalScan(const FVector& ScanOrigin)
{
	ActiveScanOrigin = ScanOrigin;
	LastRegionalScanOrigin = ScanOrigin;
	NextRegionalScanAt =
		GetWorld()->GetTimeSeconds() +
		FMath::Max(NoGrass::CVarRescanInterval.GetValueOnGameThread(), 2.0f);

	const float Radius = FMath::Max(NoGrass::CVarScanRadius.GetValueOnGameThread(), 5000.0f);

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

	if (AFGLightweightBuildableSubsystem* BoundLightweightSubsystem = LightweightSubsystem.Get())
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
	AFGLightweightBuildableSubsystem* BoundLightweightSubsystem = LightweightSubsystem.Get();
	if (!bLightweightScanActive || !BoundLightweightSubsystem)
	{
		return;
	}

	const auto& AllInstances = BoundLightweightSubsystem->GetAllLightweightBuildableInstances();
	const float ScanRadius = FMath::Max(NoGrass::CVarScanRadius.GetValueOnGameThread(), 5000.0f);
	const float ScanRadiusSquared = FMath::Square(ScanRadius);
	int32 EntriesVisited = 0;

	while (LightweightScanClassCursor < LightweightScanClasses.Num() &&
		EntriesVisited < NoGrass::MaxLightweightScanEntriesPerFrame &&
		FPlatformTime::Seconds() < Deadline)
	{
		const TSubclassOf<AFGBuildable> BuildableClass =
			LightweightScanClasses[LightweightScanClassCursor];
		const TArray<FRuntimeBuildableInstanceData>* Instances = AllInstances.Find(BuildableClass);

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

			const FRuntimeBuildableInstanceData& RuntimeData = (*Instances)[RuntimeIndex];
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
	const bool bNeedsCliffRefresh =
		OwnerState->CoverageVolumes.IsEmpty() || bForce;
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
	if (!IsValid(InLightweightSubsystem) || !BuildableClass || RuntimeIndex == INDEX_NONE)
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
	const bool bNeedsCliffRefresh =
		OwnerState->CoverageVolumes.IsEmpty() || bForce;
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

	TArray<FNoGrassPendingOwnerRelease> Releases =
		MoveTemp(PendingOwnerReleases);
	PendingOwnerReleases.Reset();
	LastPendingOwnerReleaseAt = -DBL_MAX;

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

	// A zoop is reported as many independent buildables. Releases that arrive
	// together and physically touch are one visual wave; unrelated mass-
	// dismantled structures remain separate connected groups.
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

			const FBox Expanded = ReleaseBounds[Current].ExpandBy(
				FVector(NoGrass::RegrowthGroupingGap));
			for (int32 Candidate = 0;
				Candidate < ReleaseCount;
				++Candidate)
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
	const double MaximumDelay =
		NoGrass::GetRegrowthMaximum(MinimumDelay);
	const bool bGradual =
		NoGrass::CVarGradualRegrowth.GetValueOnGameThread() != 0 &&
		MaximumDelay > MinimumDelay + UE_SMALL_NUMBER;
	const float WavePadding = FMath::Max(
		FMath::Max(
			NoGrass::CVarHorizontalPadding.GetValueOnGameThread(),
			NoGrass::CVarCliffGrassPadding.GetValueOnGameThread()),
		EffectiveLandscapePadding);

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
					Wave.AxisX = FVector2D(Axis3D.X, Axis3D.Y)
						.GetSafeNormal();
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

				const FBox ExpandedLocal = Volume.LocalBounds.ExpandBy(
					FVector(WavePadding, WavePadding, 0.0f));
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
						Seed,
						MinimumDelay,
						MaximumDelay);
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
							Seed,
							MinimumDelay,
							MaximumDelay);
					RestoreQueue.Add(
						{Key, Now + Delay, RestoreAlpha});
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
	const AFGCliffActor* CliffActor = Component->GetTypedOuter<AFGCliffActor>();
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
	Snapshot->InstanceData = NoGrass::CloneInstanceData(*InstanceData);
	GeneratedGrassBufferSnapshots.Add(Component, MoveTemp(Snapshot));

	return FilterGeneratedGrassBuffer(
		Component,
		InstanceData,
		ClusterTree,
		OcclusionLayerNum,
		NumBuiltRenderInstances);
}

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
	const FIntPoint MinPermanentCell =
		SpatialCellForLocation(CandidateBounds.Min);
	const FIntPoint MaxPermanentCell =
		SpatialCellForLocation(CandidateBounds.Max);
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
		const FNoGrassCoverageVolume& Volume =
			PermanentCoverageVolumes[Index];
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

	const FTransform ComponentToWorld = Component->GetComponentTransform();
	UStaticMesh* GrassMesh = Component->GetStaticMesh();
	const bool bRelocateAnimatedPlant = GrassMesh->GetName().Equals(
		TEXT("SM_Plant_08"),
		ESearchCase::IgnoreCase);
	const float GeneratedGrassPadding = bLandscapeGrass
		? EffectiveLandscapePadding
		: FMath::Max(
			NoGrass::CVarCliffGrassPadding.GetValueOnGameThread(),
			0.0f);
	const float VerticalPadding = bLandscapeGrass
		? EffectiveLandscapeVerticalPadding
		: FMath::Max(
			NoGrass::CVarVerticalPadding.GetValueOnGameThread(),
			0.0f);
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

	// The close-range SM_Plant_08 material ignores transformed/zeroed retained
	// instances. Match Unreal's working exclusion-box result by omitting covered
	// records from this one local render buffer and rebuilding only its HISM
	// cluster tree. This does not invalidate Landscape Grass elsewhere.
	if (bRelocateAnimatedPlant && HiddenCount > 0 &&
		HiddenCount < InstanceData->GetNumInstances())
	{
		const int32 SourceCount = InstanceData->GetNumInstances();
		const int32 VisibleCount = SourceCount - HiddenCount;
		const int32 NumCustomDataFloats =
			InstanceData->GetNumCustomDataFloats();
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
						VisibleIndex,
						CustomIndex,
						Value);
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

	const FBox RefreshBounds = PendingGeneratedGrassRefreshBounds.ExpandBy(
		NoGrass::GetCandidateQueryMargin());
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
			NoGrass::CloneInstanceData(*Snapshot->InstanceData);
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

FString UNoGrassWorldSubsystem::GetRegrowthStatus() const
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const double MinimumDelay = FMath::Max(
		static_cast<double>(
			NoGrass::CVarRegrowthSeconds.GetValueOnGameThread()),
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
		RestoreQueue.Num() +
		CliffRegrowthVolumes.Num();
	const FString NextRestoreText = PendingTotal > 0 && NextRestoreAt != DBL_MAX
		? FString::Printf(
			TEXT("%.1f sec"),
			FMath::Max(NextRestoreAt - Now, 0.0))
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
		OutLines.Add(TEXT("[CoverageDiagnostic] Required runtime subsystem unavailable."));
		return;
	}

	const float DiagnosticReach =
		FMath::Max(ScanRadius, 200.0f) +
		NoGrass::GetCandidateQueryMargin();
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

	for (const TPair<TSubclassOf<AFGBuildable>, TArray<FRuntimeBuildableInstanceData>>&
		ClassInstances : AllInstances)
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

				for (const TPair<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>&
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
					Suppression && OwnerId && Suppression->Owners.Contains(*OwnerId);
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
		AddCoverageVolume(
			MeshToWorld,
			StaticMesh->GetBoundingBox(),
			OutVolumes);
	}
	else
	{
		const FBox MeshBounds = StaticMesh->GetBoundingBox();
		const FVector MeshSize = MeshBounds.GetSize().GetAbs();
		const float MinimumHorizontalSize =
			FMath::Min(MeshSize.X, MeshSize.Y);
		const float MaximumHorizontalSize =
			FMath::Max(MeshSize.X, MeshSize.Y);
		const bool bFlatSolidPart =
			MeshSize.Z <= MinimumHorizontalSize * 0.35f;
		const bool bSlenderVerticalPart =
			MaximumHorizontalSize <= MeshSize.Z * 0.5f;

		if (bFlatSolidPart || bSlenderVerticalPart)
		{
			// Some lightweight solid pieces have simplified collision that leaves
			// visible panel or pillar areas uncovered. Use the visible bounds for
			// both ordinary foliage and Landscape Grass on these shapes. Open
			// frames still keep their precise collision-only coverage.
			AddCoverageVolume(
				MeshToWorld,
				MeshBounds,
				OutVolumes);
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
	const int32 TilesX = FMath::Max(1, FMath::CeilToInt(Width / NoGrass::CoverageTileSize));
	const int32 TilesY = FMath::Max(1, FMath::CeilToInt(Depth / NoGrass::CoverageTileSize));

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
		const float TileMinX = FMath::Lerp(Min.X, Max.X, static_cast<float>(X) / TilesX);
		const float TileMaxX = FMath::Lerp(Min.X, Max.X, static_cast<float>(X + 1) / TilesX);

		for (int32 Y = 0; Y < TilesY; ++Y)
		{
			const float TileMinY = FMath::Lerp(Min.Y, Max.Y, static_cast<float>(Y) / TilesY);
			const float TileMaxY = FMath::Lerp(Min.Y, Max.Y, static_cast<float>(Y + 1) / TilesY);
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
			const float CandidateQueryMargin =
				NoGrass::GetCandidateQueryMargin();
			Task->QueryRadius =
				HalfSize.Size() + CandidateQueryMargin;
			CoverageQueue.Enqueue(MoveTemp(Task));
		}
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
		FMath::Min(
			MaximumObservedReach,
			NoGrass::MaxAutomaticLandscapePadding));
	const float RequiredVerticalPadding = FMath::Max(
		NoGrass::CVarLandscapeVerticalPadding.GetValueOnGameThread(),
		0.0f);
	const bool bPaddingChanged = !FMath::IsNearlyEqual(
		RequiredPadding,
		EffectiveLandscapePadding,
		1.0f);
	const bool bVerticalPaddingChanged = !FMath::IsNearlyEqual(
		RequiredVerticalPadding,
		EffectiveLandscapeVerticalPadding,
		1.0f);
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
				Seed,
				NewMinimumDelay,
				NewMaximumDelay));
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
				Seed,
				NewMinimumDelay,
				NewMaximumDelay);
			Entry.RestoreStartAt = Now + Delay;
			Entry.RestoreEndAt = Now + Delay;
		}
		Entry.NextVisualRefreshAt = Entry.RestoreStartAt;
	}
}

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

			TMap<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>> Candidates;
			FoliageSubsystem->GetFoliageWithinRadius(
				ActiveCoverageTask->QueryCenter,
				ActiveCoverageTask->QueryRadius,
				Candidates);

			for (TPair<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>& Pair : Candidates)
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
			UHierarchicalInstancedStaticMeshComponent* Component = Key.Component.Get();
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
				ActiveCoverageTask->CandidateBatches[ActiveCoverageTask->BatchCursor];
			UHierarchicalInstancedStaticMeshComponent* Component = Batch.Component.Get();

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
			if (const FNoGrassSuppressionState* Existing = SuppressedGrass.Find(ExistingKey))
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

		UHierarchicalInstancedStaticMeshComponent* Component = Entry.Key.Component.Get();
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
			const int32 InstanceIndex = ResolveInstanceIndex(Entry.Key, *State);
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
			RestoreQueue.Add(
				{Entry.Key, Now + 1.0, Entry.RestoreAlpha});
			bRestoreQueueDirty = true;
			continue;
		}
		++RestoredThisFrame;

		RemoveSuppressedFromSpatialGrid(Entry.Key);
		SuppressedGrass.Remove(Entry.Key);
	}
}

void UNoGrassWorldSubsystem::GatherSuppressedCandidates(
	FNoGrassCoverageTask& Task) const
{
	const FIntPoint MinCell = SpatialCellForLocation(Task.Volume.WorldBounds.Min);
	const FIntPoint MaxCell = SpatialCellForLocation(Task.Volume.WorldBounds.Max);
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

	TSet<TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>> RetryComponents;
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& WeakComponent :
		PendingPhysicalRemovalComponents)
	{
		UHierarchicalInstancedStaticMeshComponent* Component = WeakComponent.Get();
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
	for (const TWeakObjectPtr<UHierarchicalInstancedStaticMeshComponent>& WeakComponent :
		DirtyComponents)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component = WeakComponent.Get())
		{
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

FIntVector UNoGrassWorldSubsystem::QuantizeLocation(const FVector& Location)
{
	return FIntVector(
		FMath::RoundToInt(Location.X),
		FMath::RoundToInt(Location.Y),
		FMath::RoundToInt(Location.Z));
}

FIntPoint UNoGrassWorldSubsystem::SpatialCellForLocation(const FVector& Location)
{
	return FIntPoint(
		FMath::FloorToInt(Location.X / NoGrass::CoverageTileSize),
		FMath::FloorToInt(Location.Y / NoGrass::CoverageTileSize));
}

bool UNoGrassWorldSubsystem::SegmentIntersectsBox(
	const FVector& Start,
	const FVector& End,
	const FBox& Box)
{
	if (!Box.IsValid)
	{
		return false;
	}

	const FVector Delta = End - Start;
	double MinTime = 0.0;
	double MaxTime = 1.0;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double AxisStart = Start[Axis];
		const double AxisDelta = Delta[Axis];
		const double AxisMin = Box.Min[Axis];
		const double AxisMax = Box.Max[Axis];

		if (FMath::Abs(AxisDelta) <= UE_SMALL_NUMBER)
		{
			if (AxisStart < AxisMin || AxisStart > AxisMax)
			{
				return false;
			}
			continue;
		}

		double TimeA = (AxisMin - AxisStart) / AxisDelta;
		double TimeB = (AxisMax - AxisStart) / AxisDelta;
		if (TimeA > TimeB)
		{
			Swap(TimeA, TimeB);
		}

		MinTime = FMath::Max(MinTime, TimeA);
		MaxTime = FMath::Min(MaxTime, TimeB);
		if (MinTime > MaxTime)
		{
			return false;
		}
	}

	return true;
}
