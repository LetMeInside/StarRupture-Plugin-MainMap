#pragma once

namespace SDK { class UObject; class UWorld; }

namespace MainMapBuildingLayer
{
    void TerrainConstruct(SDK::UObject* terrain);
    // If not admitted, this only requests cleanup; it never touches UObjects.
    void TerrainDestruct(SDK::UObject* terrain, bool admitted);
    void Tick(bool materialsReady);
    void BeforeWorldEnd(SDK::UWorld* world);
    void Shutdown();
}
