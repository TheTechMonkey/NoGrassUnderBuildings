#include "NoGrassUnderBuildings.h"

#include "NoGrassWorldSubsystem.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "FGCliffActor.h"
#include "FGFoliageRemovalSubsystem.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FoliageType.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GrassInstancedStaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "LandscapeProxy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Patching/NativeHookManager.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogNoGrassUnderBuildings);

namespace NoGrassDiagnostics
{
	constexpr float DefaultScanRadiusMeters = 20.0f;
	constexpr float MinimumScanRadiusMeters = 2.0f;
	constexpr float MaximumScanRadiusMeters = 100.0f;

	static bool LooksLikeVegetation(const FString& ObjectPath)
	{
		const FString LowerPath = ObjectPath.ToLower();
		return LowerPath.Contains(TEXT("grass")) ||
			LowerPath.Contains(TEXT("foliage")) ||
			LowerPath.Contains(TEXT("vegetation")) ||
			LowerPath.Contains(TEXT("groundcover")) ||
			LowerPath.Contains(TEXT("ground_cover")) ||
			LowerPath.Contains(TEXT("weed")) ||
			LowerPath.Contains(TEXT("clover")) ||
			LowerPath.Contains(TEXT("fern")) ||
			LowerPath.Contains(TEXT("moss")) ||
			LowerPath.Contains(TEXT("lichen")) ||
			LowerPath.Contains(TEXT("flower")) ||
			LowerPath.Contains(TEXT("plant")) ||
			LowerPath.Contains(TEXT("shrub")) ||
			LowerPath.Contains(TEXT("bush"));
	}

	static FString FormatMaterials(const UMeshComponent* Component)
	{
		if (!IsValid(Component))
		{
			return TEXT("-");
		}

		FString Result;
		for (int32 MaterialIndex = 0;
			MaterialIndex < Component->GetNumMaterials();
			++MaterialIndex)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT("; ");
			}
			Result += FString::Printf(
				TEXT("%d=%s"),
				MaterialIndex,
				*GetPathNameSafe(Component->GetMaterial(MaterialIndex)));
		}
		return Result.IsEmpty() ? TEXT("-") : Result;
	}

	static FString FormatSampleTransforms(
		UInstancedStaticMeshComponent* Component,
		const TArray<int32>& InstanceIndices,
		const FVector& ScanCenter)
	{
		struct FSample
		{
			float DistanceSquared = 0.0f;
			FString Text;
		};

		TArray<FSample> Samples;
		Samples.Reserve(InstanceIndices.Num());
		for (const int32 InstanceIndex : InstanceIndices)
		{
			FTransform InstanceTransform;
			if (!Component->GetInstanceTransform(
				InstanceIndex,
				InstanceTransform,
				true))
			{
				continue;
			}

			FSample Sample;
			Sample.DistanceSquared = FVector::DistSquared(
				InstanceTransform.GetLocation(),
				ScanCenter);
			Sample.Text = FString::Printf(
				TEXT("Location=%s Scale=%s Distance=%.2fm Index=%d"),
				*InstanceTransform.GetLocation().ToCompactString(),
				*InstanceTransform.GetScale3D().ToCompactString(),
				FMath::Sqrt(Sample.DistanceSquared) / 100.0f,
				InstanceIndex);
			Samples.Add(MoveTemp(Sample));
		}

		Samples.Sort([](const FSample& Left, const FSample& Right)
		{
			return Left.DistanceSquared < Right.DistanceSquared;
		});

		FString Result;
		const int32 SampleCount = FMath::Min(Samples.Num(), 6);
		for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT("; ");
			}
			Result += Samples[SampleIndex].Text;
		}
		return Result;
	}

	static void ScanNearby(const TArray<FString>& Args, UWorld* World)
	{
		if (!IsValid(World) || !World->IsGameWorld())
		{
			UE_LOG(
				LogNoGrassUnderBuildings,
				Warning,
				TEXT("Nearby scan requires a loaded game world."));
			return;
		}

		float RadiusMeters = DefaultScanRadiusMeters;
		if (!Args.IsEmpty())
		{
			RadiusMeters = FCString::Atof(*Args[0]);
		}
		RadiusMeters = FMath::Clamp(
			RadiusMeters,
			MinimumScanRadiusMeters,
			MaximumScanRadiusMeters);
		const float RadiusCentimeters = RadiusMeters * 100.0f;

		APlayerController* PlayerController = World->GetFirstPlayerController();
		APawn* PlayerPawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		if (!IsValid(PlayerPawn))
		{
			UE_LOG(
				LogNoGrassUnderBuildings,
				Warning,
				TEXT("Nearby scan could not find the local player."));
			return;
		}

		FVector ScanCenter = PlayerPawn->GetActorLocation();
		FString ScanCenterSource = TEXT("player location");
		if (PlayerController)
		{
			FVector ViewLocation = FVector::ZeroVector;
			FRotator ViewRotation = FRotator::ZeroRotator;
			PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
			FHitResult HitResult;
			FCollisionQueryParams QueryParams(
				SCENE_QUERY_STAT(NoGrassNearbyScan),
				true,
				PlayerPawn);
			if (World->LineTraceSingleByChannel(
					HitResult,
					ViewLocation,
					ViewLocation + ViewRotation.Vector() * 100000.0f,
					ECC_Visibility,
					QueryParams))
			{
				ScanCenter = HitResult.ImpactPoint;
				ScanCenterSource = FString::Printf(
					TEXT("crosshair hit: component=%s class=%s item=%d"),
					*GetPathNameSafe(HitResult.GetComponent()),
					*GetPathNameSafe(
						HitResult.GetComponent()
							? HitResult.GetComponent()->GetClass()
							: nullptr),
					HitResult.Item);
			}
		}
		AFGFoliageRemovalSubsystem* FoliageSubsystem =
			AFGFoliageRemovalSubsystem::Get(World);
		TMap<UHierarchicalInstancedStaticMeshComponent*, TArray<int32>>
			RemovableFoliage;
		if (IsValid(FoliageSubsystem))
		{
			FoliageSubsystem->GetFoliageWithinRadius(
				ScanCenter,
				RadiusCentimeters,
				RemovableFoliage);
		}

		TArray<FString> Entries;
		int32 RemovableComponentCount = 0;
		int32 LandscapeComponentCount = 0;
		int32 ExternalComponentCount = 0;
		int32 CliffGeneratedComponentCount = 0;
		int32 StandaloneVegetationCount = 0;

		for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
		{
			UInstancedStaticMeshComponent* Component = *It;
			if (!IsValid(Component) ||
				Component->HasAnyFlags(RF_ClassDefaultObject) ||
				Component->GetWorld() != World ||
				!Component->IsRegistered() ||
				!IsValid(Component->GetStaticMesh()))
			{
				continue;
			}

			UGrassInstancedStaticMeshComponent* GrassComponent =
				Cast<UGrassInstancedStaticMeshComponent>(Component);
			AFGCliffActor* CliffOwner = GrassComponent
				? GrassComponent->GetTypedOuter<AFGCliffActor>()
				: nullptr;
			const bool bGeneratedCliffGrass =
				IsValid(CliffOwner) && Component->GetNumRenderInstances() > 0;
			if (Component->GetInstanceCount() <= 0 && !bGeneratedCliffGrass)
			{
				continue;
			}

			if (bGeneratedCliffGrass)
			{
				if (Component->Bounds.GetBox().ComputeSquaredDistanceToPoint(
						ScanCenter) <= FMath::Square(RadiusCentimeters))
				{
					++CliffGeneratedComponentCount;
					Entries.Add(FString::Printf(
						TEXT("[CliffGeneratedGrass] Mesh=%s | CPUInstances=%d | RenderInstances=%d | ComponentBoundsOrigin=%s | ComponentBoundsExtent=%s | Visible=%s | Owner=%s"),
						*GetPathNameSafe(Component->GetStaticMesh()),
						Component->GetInstanceCount(),
						Component->GetNumRenderInstances(),
						*Component->Bounds.Origin.ToCompactString(),
						*Component->Bounds.BoxExtent.ToCompactString(),
						Component->IsVisible() ? TEXT("yes") : TEXT("no"),
						*GetPathNameSafe(CliffOwner)));
				}
				continue;
			}

			const TArray<int32> NearbyInstances =
				Component->GetInstancesOverlappingSphere(
					ScanCenter,
					RadiusCentimeters,
					true);
			if (NearbyInstances.IsEmpty())
			{
				continue;
			}

			UHierarchicalInstancedStaticMeshComponent* HierarchicalComponent =
				Cast<UHierarchicalInstancedStaticMeshComponent>(Component);
			const bool bRemovable =
				HierarchicalComponent &&
				RemovableFoliage.Contains(HierarchicalComponent);
			const bool bLandscapeOwned =
				Component->GetTypedOuter<ALandscapeProxy>() != nullptr;

			FString Source;
			FString FoliageTypePath = TEXT("-");
			if (bRemovable)
			{
				Source = TEXT("SatisfactoryRemovableFoliage");
				++RemovableComponentCount;
				if (IsValid(FoliageSubsystem))
				{
					FoliageTypePath = GetPathNameSafe(
						FoliageSubsystem->GetFoliageType(
							HierarchicalComponent));
				}
			}
			else if (bLandscapeOwned)
			{
				Source = TEXT("LandscapeGrass");
				++LandscapeComponentCount;
			}
			else
			{
				Source = TEXT("ExternalInstancedMesh");
				++ExternalComponentCount;
			}

			const AActor* Owner = Component->GetOwner();
			const FBoxSphereBounds MeshBounds =
				Component->GetStaticMesh()->GetBounds();
			Entries.Add(FString::Printf(
				TEXT("[%s] Mesh=%s | Materials=%s | Nearby=%d | Total=%d | MeshBoundsOrigin=%s | MeshBoundsExtent=%s | MeshSphereRadius=%.2f | ComponentBoundsOrigin=%s | ComponentBoundsExtent=%s | ComponentScale=%s | Visible=%s | HiddenInGame=%s | ComponentClass=%s | OwnerClass=%s | Owner=%s | FoliageType=%s | ClosestSamples=%s"),
				*Source,
				*GetPathNameSafe(Component->GetStaticMesh()),
				*FormatMaterials(Component),
				NearbyInstances.Num(),
				Component->GetInstanceCount(),
				*MeshBounds.Origin.ToCompactString(),
				*MeshBounds.BoxExtent.ToCompactString(),
				MeshBounds.SphereRadius,
				*Component->Bounds.Origin.ToCompactString(),
				*Component->Bounds.BoxExtent.ToCompactString(),
				*Component->GetComponentScale().ToCompactString(),
				Component->IsVisible() ? TEXT("yes") : TEXT("no"),
				Component->bHiddenInGame ? TEXT("yes") : TEXT("no"),
				*GetPathNameSafe(Component->GetClass()),
				*GetPathNameSafe(Owner ? Owner->GetClass() : nullptr),
				*GetPathNameSafe(Owner),
				*FoliageTypePath,
				*FormatSampleTransforms(
					Component,
					NearbyInstances,
					ScanCenter)));
		}

		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!IsValid(Component) ||
				Component->IsA<UInstancedStaticMeshComponent>() ||
				Component->HasAnyFlags(RF_ClassDefaultObject) ||
				Component->GetWorld() != World ||
				!Component->IsRegistered() ||
				!IsValid(Component->GetStaticMesh()))
			{
				continue;
			}

			const FString MeshPath =
				GetPathNameSafe(Component->GetStaticMesh());
			const FString Materials = FormatMaterials(Component);
			const bool bCliffOrVegetation =
				LooksLikeVegetation(MeshPath) ||
				LooksLikeVegetation(Materials) ||
				MeshPath.Contains(
					TEXT("/World/Environment/Rock/Cliff/"),
					ESearchCase::IgnoreCase);
			if (!bCliffOrVegetation ||
				Component->Bounds.GetBox().ComputeSquaredDistanceToPoint(ScanCenter) >
					FMath::Square(RadiusCentimeters))
			{
				continue;
			}

			const AActor* Owner = Component->GetOwner();
			++StandaloneVegetationCount;
			Entries.Add(FString::Printf(
				TEXT("[StandaloneCliffOrVegetationMesh] Mesh=%s | Materials=%s | ComponentBoundsOrigin=%s | ComponentBoundsExtent=%s | ComponentScale=%s | Visible=%s | HiddenInGame=%s | ComponentClass=%s | OwnerClass=%s | Owner=%s | Location=%s"),
				*MeshPath,
				*Materials,
				*Component->Bounds.Origin.ToCompactString(),
				*Component->Bounds.BoxExtent.ToCompactString(),
				*Component->GetComponentScale().ToCompactString(),
				Component->IsVisible() ? TEXT("yes") : TEXT("no"),
				Component->bHiddenInGame ? TEXT("yes") : TEXT("no"),
				*GetPathNameSafe(Component->GetClass()),
				*GetPathNameSafe(Owner ? Owner->GetClass() : nullptr),
				*GetPathNameSafe(Owner),
				*Component->GetComponentLocation().ToCompactString()));
		}

		const int32 RelevantComponentCount = Entries.Num();
		if (UNoGrassWorldSubsystem* NoGrassSubsystem =
			World->GetSubsystem<UNoGrassWorldSubsystem>())
		{
			NoGrassSubsystem->AppendCoverageDiagnostics(
				ScanCenter,
				RadiusCentimeters,
				Entries);
		}
		Entries.Sort();

		FString Report = FString::Printf(
			TEXT("No Grass Under Buildings nearby component scan\n")
			TEXT("Center: %s\nCenter source: %s\nRadius: %.1f meters\n")
			TEXT("Satisfactory removable foliage components: %d\n")
			TEXT("Landscape Grass components: %d\n")
			TEXT("External instanced-mesh components: %d\n")
			TEXT("Generated cliff-grass components: %d\n")
			TEXT("Standalone vegetation/cliff meshes: %d\n\n")
			TEXT("SatisfactoryRemovableFoliage = already visible to the mod's exact-instance system.\n")
			TEXT("LandscapeGrass = handled through local generated-instance buffers.\n")
			TEXT("ExternalInstancedMesh = outside both known grass systems and the main diagnostic target.\n\n"),
			*ScanCenter.ToCompactString(),
			*ScanCenterSource,
			RadiusMeters,
			RemovableComponentCount,
			LandscapeComponentCount,
			ExternalComponentCount,
			CliffGeneratedComponentCount,
			StandaloneVegetationCount);
		Report += FString::Join(Entries, TEXT("\n"));
		Report += TEXT("\n");

		const FString ReportPath =
			FPaths::Combine(
				FPaths::ProjectLogDir(),
				TEXT("NoGrassUnderBuildings-Scan.txt"));
		const bool bSaved = FFileHelper::SaveStringToFile(
			Report,
			*ReportPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

		for (const FString& Entry : Entries)
		{
			UE_LOG(
				LogNoGrassUnderBuildings,
				Display,
				TEXT("Nearby scan: %s"),
				*Entry);
		}
		UE_LOG(
			LogNoGrassUnderBuildings,
			Display,
			TEXT("Nearby scan found %d relevant components. Report %s: %s"),
			RelevantComponentCount,
			bSaved ? TEXT("saved") : TEXT("could not be saved"),
			*ReportPath);

		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				-1,
				8.0f,
				bSaved ? FColor::Green : FColor::Red,
				FString::Printf(
					TEXT("No Grass scan: %d nearby components. %s"),
					RelevantComponentCount,
					bSaved
						? TEXT("Report saved in FactoryGame/Saved/Logs.")
						: TEXT("Report could not be saved; check FactoryGame.log.")));
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs ScanNearbyCommand(
		TEXT("NoGrassUnderBuildings.ScanNearby"),
		TEXT("Scans nearby grass and instanced meshes. Optional argument: radius in meters (default 20)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ScanNearby));

	static void ShowRegrowthStatus(const TArray<FString>&, UWorld* World)
	{
		if (!IsValid(World) || !World->IsGameWorld())
		{
			return;
		}

		UNoGrassWorldSubsystem* Subsystem =
			World->GetSubsystem<UNoGrassWorldSubsystem>();
		if (!Subsystem)
		{
			return;
		}

		const FString Status = Subsystem->GetRegrowthStatus();
		UE_LOG(LogNoGrassUnderBuildings, Display, TEXT("%s"), *Status);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				-1,
				12.0f,
				FColor::Green,
				Status);
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs RegrowthStatusCommand(
		TEXT("NoGrassUnderBuildings.RegrowthStatus"),
		TEXT("Shows current regrowth delays, pending work, and the next return time."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ShowRegrowthStatus));
}

void FNoGrassUnderBuildingsModule::StartupModule()
{
#if !WITH_EDITOR
	SUBSCRIBE_METHOD(
		UGrassInstancedStaticMeshComponent::AcceptPrebuiltTree,
		[](auto& Scope,
			UGrassInstancedStaticMeshComponent* Component,
			TArray<FClusterNode>& ClusterTree,
			int32 OcclusionLayerNum,
			int32 NumBuiltRenderInstances,
			FStaticMeshInstanceData* InstanceData)
		{
			if (!IsValid(Component) || !InstanceData)
			{
				return;
			}

			if (UWorld* World = Component->GetWorld())
			{
				if (UNoGrassWorldSubsystem* Subsystem =
					World->GetSubsystem<UNoGrassWorldSubsystem>())
				{
					Subsystem->FilterGeneratedGrass(
						Component,
						ClusterTree,
						OcclusionLayerNum,
						NumBuiltRenderInstances,
						InstanceData);
					Scope(
						Component,
						ClusterTree,
						OcclusionLayerNum,
						NumBuiltRenderInstances,
						InstanceData);
				}
			}
		});

	SUBSCRIBE_METHOD_AFTER(
		AFGLightweightBuildableSubsystem::AddFromBuildableInstanceData,
		[](int32 RuntimeIndex,
			AFGLightweightBuildableSubsystem* LightweightSubsystem,
			TSubclassOf<AFGBuildable> BuildableClass,
			FRuntimeBuildableInstanceData&,
			bool bFromSaveData,
			int32,
			uint16,
			AActor* BuildEffectInstigator,
			int32)
		{
			if (!IsValid(LightweightSubsystem) || RuntimeIndex == INDEX_NONE)
			{
				return;
			}

			if (UWorld* World = LightweightSubsystem->GetWorld())
			{
				if (UNoGrassWorldSubsystem* Subsystem = World->GetSubsystem<UNoGrassWorldSubsystem>())
				{
					Subsystem->HandleLightweightAdded(
						LightweightSubsystem,
						BuildableClass,
						RuntimeIndex,
						bFromSaveData,
						BuildEffectInstigator);
				}
			}
		});

	SUBSCRIBE_UOBJECT_METHOD_AFTER(
		AFGBuildable,
		PlayBuildEffects,
		[](AFGBuildable* Buildable, AActor* BuildEffectInstigator)
		{
			if (!IsValid(Buildable))
			{
				return;
			}
			if (UWorld* World = Buildable->GetWorld())
			{
				if (UNoGrassWorldSubsystem* Subsystem =
					World->GetSubsystem<UNoGrassWorldSubsystem>())
				{
					Subsystem->HandleActorPlaced(
						Buildable,
						BuildEffectInstigator);
				}
			}
		});

	SUBSCRIBE_METHOD(
		AFGLightweightBuildableSubsystem::InvalidateRuntimeInstanceDataForIndex,
		[](auto&,
			AFGLightweightBuildableSubsystem* LightweightSubsystem,
			TSubclassOf<AFGBuildable> BuildableClass,
			int32 RuntimeIndex)
		{
			if (!IsValid(LightweightSubsystem) || RuntimeIndex == INDEX_NONE)
			{
				return;
			}

			if (UWorld* World = LightweightSubsystem->GetWorld())
			{
				if (UNoGrassWorldSubsystem* Subsystem = World->GetSubsystem<UNoGrassWorldSubsystem>())
				{
					Subsystem->HandleLightweightRemoved(
						LightweightSubsystem,
						BuildableClass,
						RuntimeIndex);
				}
			}
		});
#endif

	UE_LOG(
		LogNoGrassUnderBuildings,
		Display,
		TEXT("No Grass Under Buildings 0.3.7 initialized (clean configuration UI and exact orange-flower handling)"));
}

void FNoGrassUnderBuildingsModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FNoGrassUnderBuildingsModule, NoGrassUnderBuildings)
