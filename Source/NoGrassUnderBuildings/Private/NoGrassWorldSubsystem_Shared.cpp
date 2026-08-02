// Console variable definitions for the NoGrass namespace.
// All other NoGrassWorldSubsystem_*.cpp files access these as extern via NoGrassInternal.h.

#include "NoGrassInternal.h"

namespace NoGrass
{
	TAutoConsoleVariable<int32> CVarEnabled(
		TEXT("NoGrassUnderBuildings.Enabled"),
		1,
		TEXT("Enables reversible decorative-grass suppression."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarFrameBudgetMs(
		TEXT("NoGrassUnderBuildings.FrameBudgetMs"),
		0.60f,
		TEXT("Maximum game-thread time used per frame."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarMaxInstanceTestsPerFrame(
		TEXT("NoGrassUnderBuildings.MaxInstanceTestsPerFrame"),
		192,
		TEXT("Hard cap on grass intersection tests per frame."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarRegrowthSeconds(
		TEXT("NoGrassUnderBuildings.RegrowthSeconds"),
		600.0f,
		TEXT("Minimum delay before grass starts returning after the final covering building is dismantled."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarRegrowthMaxSeconds(
		TEXT("NoGrassUnderBuildings.RegrowthMaxSeconds"),
		0.0f,
		TEXT("Maximum random regrowth delay. Zero automatically uses 1.5 times RegrowthSeconds."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarGradualRegrowth(
		TEXT("NoGrassUnderBuildings.GradualRegrowth"),
		1,
		TEXT("Restores touching dismantled areas in staged edge-to-center waves."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarScanRadius(
		TEXT("NoGrassUnderBuildings.ScanRadius"),
		30000.0f,
		TEXT("Radius around the local player processed for existing buildings, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarRescanInterval(
		TEXT("NoGrassUnderBuildings.RescanInterval"),
		15.0f,
		TEXT("Minimum seconds between streamed-area rescans."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarRescanMovement(
		TEXT("NoGrassUnderBuildings.RescanMovement"),
		5000.0f,
		TEXT("Player movement required to trigger a streamed-area rescan, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarHorizontalPadding(
		TEXT("NoGrassUnderBuildings.HorizontalPadding"),
		200.0f,
		TEXT("Horizontal ordinary-foliage coverage buffer around buildings, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarVerticalPadding(
		TEXT("NoGrassUnderBuildings.VerticalPadding"),
		200.0f,
		TEXT("Vertical ordinary-foliage coverage buffer above and below buildings, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarCliffGrassPadding(
		TEXT("NoGrassUnderBuildings.CliffGrassPadding"),
		100.0f,
		TEXT("Horizontal root-point buffer for render-only cliff grass, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarLandscapePadding(
		TEXT("NoGrassUnderBuildings.LandscapePadding"),
		200.0f,
		TEXT("Extra horizontal root-point buffer for leaning Landscape Grass, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarLandscapeVerticalPadding(
		TEXT("NoGrassUnderBuildings.LandscapeVerticalPadding"),
		200.0f,
		TEXT("Vertical Landscape Grass exclusion buffer above and below buildings, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarMaxGrassExtent(
		TEXT("NoGrassUnderBuildings.MaxGrassExtent"),
		500.0f,
		TEXT("Maximum allowed grass-mesh extent on any axis, in centimeters."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarDebug(
		TEXT("NoGrassUnderBuildings.Debug"),
		0,
		TEXT("Logs accepted grass meshes and suppression activity."),
		ECVF_Default);

} // namespace NoGrass
