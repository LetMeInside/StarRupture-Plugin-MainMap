# StarRupture MainMap

## Overview

MainMap enhances StarRupture's existing in-game world map by displaying player-built structures using building-map imagery supplied by MiniMap. It integrates with the native map and preserves its terrain, markers, navigation, panning, zooming, and clipping.

## Features

- Player-built structures displayed on the world map using MiniMap's building imagery.
- Optional expanded native world map layout.
- Optional hiding of Fog of War presentation.
- Independent control over building-overlay visibility.
- Native map navigation, terrain, and markers retained.

Building visualization depends on the imagery available from MiniMap; this is not a guarantee that every building type is represented.

## Requirements and installation

- StarRupture.
- [AlienX Mod Loader](https://github.com/AlienXAXS/StarRupture-ModLoader).
- [MiniMap](https://github.com/LetMeInside/StarRupture-Plugin-MiniMap), required for building visualization.

Download the latest compatible releases of [MainMap](https://github.com/LetMeInside/StarRupture-Plugin-MainMap/releases), MiniMap, and AlienX Mod Loader. Follow the [Mod Loader installation and plugin instructions](https://github.com/AlienXAXS/StarRupture-ModLoader#installation) and the instructions supplied with the MainMap and MiniMap releases.

## Configuration

MainMap registers these Boolean entries with ModLoader's configuration system:

| Entry | Default | Effect |
| --- | --- | --- |
| `Map.ExpandedMap` | `true` | Enables the expanded native world map layout. Set to `false` for the game's original layout. |
| `Map.HideFoW` | `false` | Hides native Fog of War presentation when enabled. |
| `Buildings.ShowLayer` | `true` | Shows the building overlay; disable it independently of the layout and fog preferences. |

`HideFoW` affects visual presentation only. It does not modify the game's exploration data. MainMap reads saved settings at startup and observes live configuration changes through ModLoader.

## Architecture

MiniMap owns building discovery, geometry generation, and Building Map Service image production and publication. MainMap consumes those completed images and integrates them into the native world map; it does not independently discover buildings or generate their geometry.

MainMap owns native map integration, terrain-session and viewport management, composite-grid preparation and rendering, presentation beneath native Fog of War, and native map layout and visibility preferences.

The active pipeline is:

1. [BuildingLayer](Src/Map/BuildingLayer.cpp) observes native terrain lifecycle and map visibility, retains session objects, derives the clipped visible world rectangle, and coordinates viewport updates, rendering, layout, and cleanup.
2. [BuildingMapRemote](Src/Map/BuildingMapRemote.h) discovers `MiniMap_GetBuildingMapServiceABI`, validates its service table, and manages opaque collector and viewport handles. It polls completed Section images and copies their RGBA8 pixels into MainMap-owned buffers; provider objects and scheduling remain in MiniMap.
3. [BuildingMapTarget](Src/Map/BuildingMapTarget.h), using [BuildingMapGridPlanner](Src/Map/BuildingMapGridPlanner.h), imports source images and prepares a bounded composite grid with six physical target slots. MainMap owns these render targets and the source texture/material resources. Changed assignments and source observations drive recomposition; physical targets can be reused within the terrain session.
4. [BuildingMapPresentation](Src/Map/BuildingMapPresentation.h) binds eligible composites to presentation materials and creates segment-local cropped images where composite bounds overlap native terrain segments. The images sit above terrain and beneath native Fog of War. Crops follow each composite's world bounds while native map transforms and clipping remain in control.

Presentation is withdrawn before dirty or reassigned targets are redrawn. Session changes and shutdown retire presentation, target resources, and provider viewport ownership through the existing lifecycle and UObject lease handling.

## Materials

Runtime material paths and texture parameters are:

| Role | Material path | Texture parameter |
| --- | --- | --- |
| Composite rendering | `/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose` | `SpriteAtlas` |
| Native map presentation | `/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent` | `BuildingTarget` |

The composition material draws imported Section images into MainMap-owned composite render targets. Presentation uses those targets, with segment-local cropping supplied through image-brush UV regions. The materials are loaded from the mounted material content; cook/package history is separate from the active rendering architecture.

## Development documentation

The linked source headers above describe the current service consumer, grid, target ownership, and presentation responsibilities. Configuration is declared in [Config.h](Src/Config/Config.h) and [Config.cpp](Src/Config/Config.cpp); plugin initialization and shutdown are in [plugin.cpp](Src/plugin.cpp).

The [material authoring and cook guide](Src/Content/Cook/README.md) preserves historical BCT1 authoring, package inventory, and verification evidence. Its historical runtime descriptions do not describe the current Building Map Service pipeline.
