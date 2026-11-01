#pragma once
#include <cstdint>
#ifdef MODLOADER_CLIENT_BUILD
#include "..\..\..\..\MiniMap\Solution\Src\Map\Buildings\ExternalMapBuildingSessionAbi.h"
#endif

namespace SDK { class UObject; class UWorld; }

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

    void TerrainConstruct(SDK::UObject* terrain);
    // If not admitted, this only requests cleanup; it never touches UObjects.
    bool TerrainDestruct(SDK::UObject* terrain, bool admitted);
    void Tick(bool materialsReady);
    bool BeforeWorldEnd(SDK::UWorld* world);
    bool HasActiveTerrainSession();
    bool TryGetVisibleWorldRect(WorldRect& out);
#ifdef MODLOADER_CLIENT_BUILD
    MiniMapBuildingAbiResult RenderBuildings(MiniMapBuildingRenderFn render,float delta,MiniMapBuildingRenderStatus& status);
#endif
    void Shutdown();
}
