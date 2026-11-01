#pragma once
#ifdef MODLOADER_CLIENT_BUILD
#include "..\..\..\..\MiniMap\Solution\Src\Map\Buildings\ExternalMapBuildingSessionAbi.h"
#endif

namespace SDK { class UObject; class UWorld; }

namespace MainMapBuildingLayer
{
    struct WorldRect final { double minX, minY, maxX, maxY; };

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
