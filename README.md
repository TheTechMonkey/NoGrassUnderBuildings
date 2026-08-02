# No Grass Under Buildings 0.3.7

For Satisfactory 1.2 build 495413, Unreal Engine 5.6.1, and SML 3.12.

The mod hides grass beneath buildings and foundations. Ordinary grass, Unreal
Landscape Grass, and the separate render-only grass used by cliffs and pillars
are handled. By default, grass returns after the final covering building is
dismantled. Regrowth can be spread between a minimum and maximum delay. Grass
returns in staged edge-to-center waves across ordinary, landscape, and cliff
rendering. Touching pieces dismantled together, including zooped foundations,
share one continuous regrowth footprint.

Landscape and cliff grass are restored by updating only their nearby generated
instance buffers. The mod does not change Unreal's global Landscape Grass
exclusion list during regrowth, avoiding the map-wide redraw/flicker that could
be visible with short regrowth timers.

Hidden generated vegetation is also displaced safely below the world. This
keeps animated flower and plant materials invisible even when their shader
does not fully respect an instance's tiny hidden scale.

The short orange `SM_Plant_07` ground flowers use Satisfactory's ordinary
removable-foliage system rather than Landscape Grass. They are handled as an
exact exception, using the same root-position buffer test as ordinary grass so
uncovered flowers and bushes outside the configured buffer remain visible.

The mod does not permanently remove trees, bushes, flowers, resource plants,
or other foliage. Satisfactory's normal building-placement and chainsaw rules
remain responsible for those objects. No chainsaw or fuel check is performed
by this mod.

## In-game configuration

Use SML's **Mod Configuration** screen and open **No Grass Under Buildings**.
The page provides:

- minimum and maximum grass-regrowth times
- gradual edge-to-center regrowth on/off
- horizontal and vertical grass-clearing buffers
- **Regrow grass after dismantling** on/off

The obsolete permanent tree, bush, plant, resource, other-foliage, and
chainsaw controls from early development builds are not part of this page.

When **Regrow grass after dismantling** is enabled, hidden grass returns after
the configured delay. Disable it before placing a building to save that grass
coverage as indefinitely cleared. This is non-destructive suppression: the mod
does not invoke the game's permanent foliage-removal or reward paths.

## Performance and multiplayer

The host records non-regrowing grass coverage in the save and replicates it to
clients. Clients perform local rendering work for ordinary, landscape, and
generated cliff grass. Large placement bursts are processed within a small
per-frame budget, persistent coverage lookups use a spatial index, and stale
generated-grass snapshots are released when their streamed components unload.

The mod is required on the host/server and connecting clients.

## Diagnostic console controls

- `NoGrassUnderBuildings.RegrowthStatus`
- `NoGrassUnderBuildings.ScanNearby 10`
- `NoGrassUnderBuildings.Debug 0|1`
- `NoGrassUnderBuildings.FrameBudgetMs 0.60`

`ScanNearby` writes
`FactoryGame/Saved/Logs/NoGrassUnderBuildings-Scan.txt`. Normal settings should
be changed through the configuration page.

## Manual packaging note

`Binaries/Win64/FactoryGameSteam-Win64-Shipping.modules` must use game build ID
`495413`.
