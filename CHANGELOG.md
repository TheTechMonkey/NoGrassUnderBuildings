# Changelog

## 1.3.3

- Fixed foliage sometimes remaining beneath newly placed foundations when other structures were positioned far above or below the same area.
- Added support for hiding the previously missed `SM_PlantModular_D` decorative plant.
- Improved vertical coverage checks so structures at unrelated heights no longer interfere with foliage updates.

## 1.3.2

- Added a three-dimensional spatial index so grass processing checks only nearby buildings instead of every tracked structure in the save.
- Reduced the spatial grid to 20-meter cells to avoid frame pauses in large or densely built saves.
- Removed per-building UObject tracking tokens that could exhaust Unreal Engine's object limit in extremely large saves.
- Applied the optimized nearby-only filtering to both landscape grass and cliff grass as foliage streams in.
- Preserved event-driven placement and dismantle handling, vegetation restoration, and multiplayer support.

## 1.3.1

- Replaced recurring full-world building scans with event-driven placement and dismantle tracking.
- Added one initial coverage scan when a save loads.
- Added targeted reconciliation when decorative foliage streams back in after travelling away.
- Batched zooped and blueprint building changes into a single coverage refresh.
- Avoided unnecessary refreshes when an affected area remains covered by another tracked building.
- Preserved vegetation restoration when covering buildings are dismantled.
- Removed automatic diagnostic trace capture and recurring display-level diagnostic logs.
- Included Steam, Epic, Windows dedicated-server, and Linux dedicated-server builds.

## 1.3.0

- Updated the mod for Satisfactory 1.2.4 and SML 3.12.
- Replaced the previous continuous world-processing approach with targeted exclusion boxes and foliage reconciliation.
- Added support for landscape grass, cliff grass, flowers, and other small decorative ground foliage.
- Added Steam and Epic Windows client, Windows dedicated-server, and Linux dedicated-server builds.
