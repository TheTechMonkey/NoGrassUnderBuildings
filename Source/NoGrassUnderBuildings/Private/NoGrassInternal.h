#pragma once

// Private header shared by all NoGrassWorldSubsystem_*.cpp translation units.
// Not part of the module's public API — do not include from Public/ headers.

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "NoGrassWorldSubsystem.h"

namespace NoGrass
{
	// ── Console variables ─────────────────────────────────────────────────────
	// Defined in NoGrassWorldSubsystem_Shared.cpp. All other translation units
	// access them as extern via this header.
	extern TAutoConsoleVariable<int32>  CVarEnabled;
	extern TAutoConsoleVariable<float>  CVarFrameBudgetMs;
	extern TAutoConsoleVariable<int32>  CVarMaxInstanceTestsPerFrame;
	extern TAutoConsoleVariable<float>  CVarRegrowthSeconds;
	extern TAutoConsoleVariable<float>  CVarRegrowthMaxSeconds;
	extern TAutoConsoleVariable<int32>  CVarGradualRegrowth;
	extern TAutoConsoleVariable<float>  CVarScanRadius;
	extern TAutoConsoleVariable<float>  CVarRescanInterval;
	extern TAutoConsoleVariable<float>  CVarRescanMovement;
	extern TAutoConsoleVariable<float>  CVarHorizontalPadding;
	extern TAutoConsoleVariable<float>  CVarVerticalPadding;
	extern TAutoConsoleVariable<float>  CVarCliffGrassPadding;
	extern TAutoConsoleVariable<float>  CVarLandscapePadding;
	extern TAutoConsoleVariable<float>  CVarLandscapeVerticalPadding;
	extern TAutoConsoleVariable<float>  CVarMaxGrassExtent;
	extern TAutoConsoleVariable<int32>  CVarDebug;

	// ── Constants ─────────────────────────────────────────────────────────────
	constexpr float  CoverageTileSize                  = 1200.0f;
	constexpr int32  MaxCoverageTilesPerVolume         = 256;
	constexpr int32  MaxActorScanEntriesPerFrame       = 64;
	constexpr int32  MaxLightweightScanEntriesPerFrame = 256;
	constexpr float  MinimumCandidateQueryMargin       = 100.0f;
	constexpr float  InstanceResolveRadius             = 10.0f;
	// Exact zero-scale transforms are not reliably consumed by every streamed
	// foliage render buffer. Some plant materials also apply world-position
	// animation after instance scaling. Keep a non-degenerate scale and move the
	// hidden instance well below the world so both render paths remain invisible.
	constexpr float  HiddenInstanceScale               = 0.001f;
	constexpr float  HiddenInstanceDepth               = 100000.0f;
	constexpr float  HiddenAnimatedPlantOffset         = 10000000.0f;
	constexpr double LandscapeReachScanInterval        = 2.0;
	constexpr float  LandscapeInstanceScaleSafety      = 1.0f;
	constexpr float  MaxAutomaticLandscapePadding      = 200.0f;
	constexpr int32  GeneratedGrassRegrowthVisualSteps = 12;
	constexpr double OwnerReleaseBatchDelay            = 0.20;
	constexpr float  RegrowthGroupingGap               = 100.0f;
	constexpr uint64 PermanentGrassOwnerId             = MAX_uint64;

	// ── Inline helpers ────────────────────────────────────────────────────────
	FORCEINLINE float GetCandidateQueryMargin()
	{
		return FMath::Max(
			FMath::Max(CVarHorizontalPadding.GetValueOnGameThread(), 0.0f),
			MinimumCandidateQueryMargin);
	}

	FORCEINLINE double GetRegrowthMaximum(double MinimumDelay)
	{
		const double Configured = FMath::Max(
			static_cast<double>(CVarRegrowthMaxSeconds.GetValueOnGameThread()),
			0.0);
		return Configured > 0.0
			? FMath::Max(Configured, MinimumDelay)
			: MinimumDelay * 1.5;
	}

	FORCEINLINE double GetRandomRestoreDelay(
		uint32 Seed, double MinimumDelay, double MaximumDelay)
	{
		const double UnitValue =
			static_cast<double>(Seed % 1000001u) / 1000000.0;
		return FMath::Lerp(MinimumDelay, MaximumDelay, UnitValue);
	}

	inline float GetRegrowthAlpha(
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
		const double EdgeDepthX =
			FMath::Min(Point.X - Wave.Min.X, Wave.Max.X - Point.X) /
			FMath::Max(static_cast<double>(Extent.X), 1.0);
		const double EdgeDepthY =
			FMath::Min(Point.Y - Wave.Min.Y, Wave.Max.Y - Point.Y) /
			FMath::Max(static_cast<double>(Extent.Y), 1.0);
		const double EdgeDepth =
			FMath::Clamp(FMath::Min(EdgeDepthX, EdgeDepthY), 0.0, 1.0);
		const double Jitter =
			(static_cast<double>(Seed % 1001u) / 1000.0 - 0.5) * 0.07;
		return static_cast<float>(FMath::Clamp(EdgeDepth + Jitter, 0.0, 1.0));
	}

	FORCEINLINE bool RequiresPhysicalRemoval(const UStaticMesh* GrassMesh)
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

} // namespace NoGrass
