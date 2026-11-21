#pragma once
#include <cstdint>

namespace SDK { class UObject; class UWorld; }
namespace SDK { class UCanvasPanel; class UWidgetTree; }
namespace MainMapBuildingMap { class BuildingMapRemote; }

namespace MainMapBuildingLayer
{
    struct WorldRect final { double minX, minY, maxX, maxY; };
    // Owned terrain-session correlation; never compared to provider generations.
    struct ConsumerContext final
    {
        SDK::UWorld* world=nullptr;
        uint64_t epoch=0;
        double originX=0,originY=0;
    };
    bool TryGetConsumerContext(ConsumerContext& out);
    // Borrowed from the active terrain session; callers retain every UObject
    // they keep beyond this call. The tree is the established SpawnObject Outer.
    bool TryGetTilePresentationContext(SDK::UCanvasPanel*& canvas, SDK::UWidgetTree*& tree);

    void TerrainConstruct(SDK::UObject* terrain);
    // If not admitted, this only requests cleanup; it never touches UObjects.
    bool TerrainDestruct(SDK::UObject* terrain, bool admitted);
    void Tick();
    // ChimeraMain callback identity; does not dereference or retain the world.
    void WorldBeginPlay(SDK::UWorld* world);
    bool BeforeWorldEnd(SDK::UWorld* world);
    bool HasActiveTerrainSession();
    // Reads the active map menu's activation and visibility state through the
    // validated terrain context; a terrain session alone is not visibility.
    bool IsMainMapVisible();
    bool TryGetVisibleWorldRect(WorldRect& out);
    // Stage 3 presentation reads the currently copied images from this owner;
    // provider operations remain driven by Tick and the terrain lifecycle.
    MainMapBuildingMap::BuildingMapRemote& GetBuildingMapRemote() noexcept;
    // Device-pixel length of a 500 m tile under the current terrain geometry.
    bool TryGetGroupedTilePixelSpan(double& out);
    void Shutdown();
}
