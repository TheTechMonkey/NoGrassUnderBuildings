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

The mod does not automatically remove trees, bushes, berries, resource plants, or other valuable foliage. Those should still be removed using normal game tools.

No configuration is required or provided.

## Compatibility

- Satisfactory 1.2.4 (CL 502094)
- SML 3.12
- Steam and Epic Windows clients
- Windows dedicated servers
- Linux dedicated servers

## To-Do

- Improve how the footprint of each placed building or item is calculated.
- Maintain a consistent approximately 1-meter exclusion buffer around placed items so edge grass cannot protrude through them.
