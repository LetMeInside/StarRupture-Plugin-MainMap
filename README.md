# StarRupture MainMap

MainMap adds validated MiniMap BuildingComplex (BCT1) publications to the existing StarRupture world map. It retains the game's native terrain, markers, fog, map transform, panning, zooming, and clipping.

## Architecture

MainMap owns terrain construction/destruction observation, conservative world/menu/widget admission, the terrain session's strong UObject references, and geometry queries for the clipped visible world rectangle and grouped tile pixel span. `BuildingComplexConsumer` is the only external building-image consumer.

MiniMap owns building discovery and capture, sprite caching, BuildingComplex cells, composition, and texture publication. MainMap discovers `MiniMap_GetBuildingComplexApi` and uses the frozen BCT1 ABI. It demands, inspects, acquires, and presents each current visible tile independently. A tile is shown only after its BCT1 publication and texture lease pass validation and its own UImage/MID presentation is ready. Empty, pending, failed, and unavailable tiles are not fabricated. Pan/zoom placement uses the publication's world bounds and UV crop values. MainMap removes the UI and MID references before releasing each provider lease.

There is no legacy external-map renderer or fallback. Terrain session diagnostics remain enabled at `LOG_INFO` to investigate admission and closure, including distinct terrain-session epochs.

## Materials

The runtime requires `M_BuildingComplexPresent`, mounted from the embedded `MainMapMaterials` resource container. This material uses the BCT1 `TileTexture`, `UVScale`, and `UVBias` parameters. MainMap no longer loads the legacy compose/presentation materials or owns a render target.

The currently embedded resource triplet and accepted cook manifest still contain the four historical legacy packages alongside `M_BuildingComplexPresent`. Producing a BCT1-only package requires the Unreal cook and IoStore packaging workflow; that package has not been regenerated in this source-only change. Do not treat the current container as a BCT1-only production package until that separately authorized packaging work is complete. See `Src/Content/Cook/README.md`.

## Lifecycle and verification boundaries

The BCT1 session closes before the terrain session releases its world, canvas, and terrain leases. UI and MID references are detached before BCT1 lease release. Shutdown preserves unacknowledged ownership rather than calling a potentially unloaded provider late.

This source change was inspected without building, testing, launching the game, or deploying. Runtime admission, publication presentation, and shutdown behavior still require their normal build and playtest gates.
