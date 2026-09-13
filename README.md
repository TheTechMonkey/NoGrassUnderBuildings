# No Grass Under Buildings

Stops grass and small decorative ground vegetation from poking through foundations, ramps, platforms, and other buildings.

## Features

- Works in multiplayer when installed on both the server and client.
- Hides landscape grass, cliff grass, flowers, and other small decorative foliage.
- Supports individual, zooped, and blueprint building placement.
- Restores vegetation when the covering buildable is dismantled.
- Scans existing buildings once when a save loads, then tracks changes through events.
- Reapplies suppression when foliage streams back in after travelling away.
- Batches related placement and dismantle events into a single targeted refresh.
- Avoids refreshing areas that remain covered by another tracked building.
- Uses a precise, precomputed physical footprint for the Space Elevator instead of its enormous orbital bounds.
- Optionally clears supported decorative foliage along saved vehicle paths, using a route width based on the assigned vehicle.
- Restores route foliage when a path is removed or vehicle-path clearing is disabled.

The mod does not automatically remove trees, bushes, berries, resource plants, or other valuable foliage. Those should still be removed using normal game tools.

## Configuration

`Vehicle paths` is enabled by default. Turn it off in the mod settings to limit foliage clearing to buildings. The setting applies live without reloading the save.

## Compatibility

- Satisfactory 1.2.4 
- SML 3.12
