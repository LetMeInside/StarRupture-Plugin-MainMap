# StarRupture MainMap

**A building-overlay plugin for the StarRupture world map.**

MainMap is a native C++ plugin for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/), built using the [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader).

The project aims to display player-built structures directly on the game's existing world map, using the building sprites and rendering infrastructure developed for [MiniMap](https://github.com/LetMeInside/StarRupture-Plugin-MiniMap).

> **Development status: Work in progress**
>
> MainMap is not yet ready for general use. The rendering foundation has been implemented and partially verified, but the actual map overlay, lifecycle management and MiniMap integration are still under development. No public release is available.

## Project Goals

MainMap is designed to extend the existing world map rather than replace it.

The intended functionality includes:

- Displaying player-built structures at their correct world positions.
- Reusing MiniMap's building-sprite cache, atlas and rendering algorithms.
- Integrating with the existing map's zooming, panning and clipping.
- Rendering buildings beneath the game's existing map markers.
- Supporting large building areas through a dedicated, temporary building collector.
- Keeping MainMap and MiniMap as separate plugins with clearly defined responsibilities.

The plugin is intended to work alongside MiniMap, not replace it.

## Architecture

### MainMap

MainMap is responsible for:

- Detecting the lifecycle of the existing world-map interface.
- Attaching a passive building-overlay image to the game's terrain canvas.
- Managing its own rendering targets and presentation resources.
- Mapping building geometry into world-map coordinates.
- Managing the lifetime of its UI and rendering resources.

### MiniMap integration

The planned integration will reuse MiniMap's existing:

- Native building capture infrastructure.
- Persistent building-sprite cache.
- CPU texture atlas.
- Building geometry and rendering algorithms.

MainMap will eventually use a separate, temporary building collector implemented within MiniMap.

This collector will operate only while an actual MainMap session is active. It will be independent of MiniMap's persistent collector and will be destroyed when the world map closes.

The plugins will communicate through a versioned native interface. This integration has not yet been implemented.

## Rendering Design

The rendering system uses two Unreal Engine materials:

**M_BuildingCompose**

Composites textured building geometry into a render target, supporting correct alpha blending, tinting and overlapping geometry.

**M_BuildingPresent**

Presents the completed render target through Unreal's UMG interface.

Both materials are cooked for the Windows client and embedded in `MainMap.dll` as an IoStore resource triplet.

This permits DLL-only distribution without requiring users to install separate material files.

The rendering design uses an RGBA16f target with associated RGB and transmittance alpha. This preserves the required transparency behavior when compositing multiple building sprites.

## Current Implementation Status

The following foundations have been implemented:

- Native UObject lifetime management with move-only strong-object leases.
- Typed native bindings and ModLoader-managed fingerprint resolution.
- Embedded IoStore material resources and runtime mounting.
- Loading and verification of both cooked materials in the shipping game.
- Render-target creation and textured-triangle submission.
- Numerical RGBA rendering validation.
- Native world-map terrain construction/destruction observation.
- Reusable frame-preparation helpers.

**Verified results:**

- 18 numerical rendering checks passed in the shipping client.
- Both cooked materials were successfully loaded.
- Two actual world-map terrain construction/destruction cycles were observed.
- Client Debug, Client Release and Server Release configurations compile successfully.

**Remaining work:**

- Attach and display the overlay through UMG/Slate.
- Implement persistent rendering-resource ownership.
- Verify safe resource replacement, garbage collection and shutdown.
- Integrate MiniMap's building renderer.
- Implement the dedicated MainMap building collector.
- Complete the production rendering and visibility behavior.

Some of the latest rendering-frame helpers compile successfully but have not yet undergone runtime verification.

## Requirements

### For development

- Windows
- Visual Studio 2022 with MSVC C++20 support
- Windows SDK
- [StarRupture Plugin SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK)
- Compatible StarRupture ModLoader

The project currently targets:

| Component | Version |
|---|---|
| StarRupture | CL-127004 |
| Unreal Engine | 5.6.1 |
| ModLoader | 1.22.1 |
| Plugin interface | v70 |
| C++ | C++20 |

The Plugin SDK is an **external development dependency** and is not copied into this repository.

The Visual Studio project expects the shared SDK at:

```text
StarRupture/
├── StarRupture-Plugin-SDK/
└── MainMap/
    └── Solution/
```

The SDK path is resolved relative to `MainMap.sln` using the project's MSBuild properties.

### For cooking materials

Regenerating the Unreal materials additionally requires:

- Unreal Engine 5.6.1
- Python 3
- Unreal Editor command-line cooking tools
- UnrealPak and IoStore packaging tools

The accepted cooked material resources are already included in the repository. **Cooking is not required for a normal DLL build.**

## Building

Open:

```text
MainMap.sln
```

Available configurations include:

- Client Debug | x64
- Client Release | x64
- Server Release | x64

For a release build, select `Client Release | x64` and build the solution.

The output DLL is written to:

```text
bin\x64\Client Release\plugins\MainMap.dll
```

The build validates the accepted cooked material files before resource compilation and checks that the resulting DLL contains the correct embedded resources.

The materials are embedded automatically during the normal build.

No additional material files need to be deployed alongside the DLL.

## Material Authoring

The material-authoring project is maintained inside the repository:

```text
Src/
├── Content/
│   ├── StarRupture.uproject
│   ├── Config/
│   ├── Content/
│   └── Cook/
├── Resources/
│   └── Materials/
│       ├── MainMapMaterials.pak
│       ├── MainMapMaterials.utoc
│       └── MainMapMaterials.ucas
└── Build/
```

The source material assets and cooking scripts are preserved so the embedded materials can be regenerated without relying on external project-specific files.

See `Src/Content/Cook/README.md` for the authoring and cooking procedure.

Material regeneration is an explicit operation, separate from the normal C++ build.

## Installation

**No public installation instructions are available yet.**

MainMap is still under active development. The current DLL contains diagnostic rendering functionality and is not intended for general gameplay use.

## Related Projects

- [StarRupture MiniMap](https://github.com/LetMeInside/StarRupture-Plugin-MiniMap)
- [StarRupture ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)
- [StarRupture Plugin SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK)

## Author

Developed by **Kian369 / LetMeInside**.

## License

Source Available License

See [LICENSE](LICENSE) for the full license text.