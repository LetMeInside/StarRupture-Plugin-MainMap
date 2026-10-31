#include "RenderingProof.h"
#include "Native/NativeApi.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#include "MaterialProof.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "..\..\..\..\MiniMap\Solution\Src\Map\Buildings\ExternalMapBuildingSessionAbi.h"
#endif
#ifdef MODLOADER_CLIENT_BUILD
#include "BuildingLayer.h"
#endif

namespace
{
    bool g_registered = false;
    bool g_attempted = false;
    bool g_materialsReady = false;
    PluginPakHandle g_pakHandle = 0;
#ifdef MODLOADER_CLIENT_BUILD
    HMODULE g_miniMapModule=nullptr;
    MiniMapBuildingSessionApiV1 g_miniMapBuildingApi{};
    bool g_miniMapSessionActive=false;

    void DiscoverMiniMapBuildingApi()
    {
        HMODULE module=nullptr;
        if(!GetModuleHandleExW(0,L"MiniMap.dll",&module))return;
        auto entry=reinterpret_cast<decltype(&MiniMap_GetBuildingSessionApi)>(
            GetProcAddress(module,"MiniMap_GetBuildingSessionApi"));
        MiniMapBuildingSessionApiV1 api{};
        api.StructSize=sizeof(api);
        if(!entry || entry(MINIMAP_BUILDING_SESSION_ABI_VERSION,&api)!=MINIMAP_BUILDING_ABI_OK ||
            api.Version!=MINIMAP_BUILDING_SESSION_ABI_VERSION ||
            (api.Capabilities&(MINIMAP_BUILDING_ABI_CAP_SESSION|MINIMAP_BUILDING_ABI_CAP_DIAGNOSTICS))!=
                (MINIMAP_BUILDING_ABI_CAP_SESSION|MINIMAP_BUILDING_ABI_CAP_DIAGNOSTICS) ||
            !api.OpenSession || !api.UpdateWorldRectangle || !api.Service || !api.ReadStatus || !api.CloseSession)
        {
            FreeLibrary(module);
            return;
        }
        g_miniMapModule=module; // Owns a module reference for every cached callable pointer.
        g_miniMapBuildingApi=api;
    }

    bool CloseMiniMapBuildingSession()
    {
        if(!g_miniMapSessionActive)return true;
        const auto result=g_miniMapBuildingApi.CloseSession();
        if(result==MINIMAP_BUILDING_ABI_OK)g_miniMapSessionActive=false;
        return !g_miniMapSessionActive;
    }

    void ReleaseMiniMapBuildingApi()
    {
        if(!CloseMiniMapBuildingSession())
        {
            LOG_ERROR("ExternalMap: MiniMap session close was not admitted; retaining its module reference");
            return;
        }
        g_miniMapBuildingApi={};
        if(g_miniMapModule)FreeLibrary(g_miniMapModule);
        g_miniMapModule=nullptr;
    }
#endif
    void ObserveTerrainConstruct(SDK::UObject* terrain)
    {
#ifdef MODLOADER_CLIENT_BUILD
        ExternalMapBuildingLayer::TerrainConstruct(terrain);
#endif
    }
    void ObserveTerrainDestruct(SDK::UObject* terrain,bool admitted)
    {
#ifdef MODLOADER_CLIENT_BUILD
        if(admitted)CloseMiniMapBuildingSession();
        ExternalMapBuildingLayer::TerrainDestruct(terrain,admitted);
#endif
    }
    void InspectExistingTerrain(const char* boundary)
    {
        if(!ExternalMapNative::CanOperate())
        {
            LOG_WARN("ExternalMap A2.3: terrain lookup at %s rejected by admission",boundary);
            return;
        }
        auto* walker=g_self->hooks->ObjectWalker;
        if(!walker->IsReady()) return;
        for(const char* cls : {"WBP_MapMenuTerrain_C","CrUW_MapMenuTerrain","WBP_MapMenuMapArea_C","WBP_MapMenu_C"})
        {
            PluginObjectInfo objects[4]{};
            const int total=walker->FindObjectsByClassNameInto(cls,PluginObjectLookup_InstanceOnly,objects,4);
            LOG_INFO("ExternalMap A2.3: terrain lookup boundary=%s class=%s instances=%d",boundary,cls,total);
            for(int i=0;i<total && i<4;++i)
                LOG_INFO("ExternalMap A2.3: terrain lookup object=%p index=%d name=%s",objects[i].object,
                    objects[i].objectIndex,objects[i].objectName);
        }
    }
    void BeforeWorldEnd(SDK::UWorld* world,const char*)
    {
#ifdef MODLOADER_CLIENT_BUILD
        CloseMiniMapBuildingSession();
        ExternalMapBuildingLayer::BeforeWorldEnd(world);
#endif
        InspectExistingTerrain("before-world-end");
    }

    void OnTick(float delta)
    {
#ifdef MODLOADER_CLIENT_BUILD
        ExternalMapBuildingLayer::Tick(g_materialsReady);
        if(g_miniMapSessionActive && ExternalMapNative::CanOperate() && g_miniMapBuildingApi.Service(delta)!=MINIMAP_BUILDING_ABI_OK)
            g_miniMapSessionActive=false;
#endif
        if (g_attempted || !ExternalMapNative::CanOperate()) return;
        g_attempted = true; // No retry after timeout or uncertain engine acceptance.
        InspectExistingTerrain("initial-admitted-tick");
        auto* pak = g_self->hooks->Pak;
        if (!pak || !pak->IsAvailable || !pak->IsAvailable() || !pak->MountResource ||
            !pak->LoadObject || !pak->ResultToString)
        {
            LOG_ERROR("ExternalMap A2.3: pak API unavailable; compatibility gate CLOSED");
            return;
        }
        HMODULE module = nullptr;
        // Resolve the DLL containing this function without acquiring another module reference.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&OnTick), &module))
        {
            LOG_ERROR("ExternalMap A2.3: actual ExternalMap module lookup failed");
            return;
        }
        PluginPakMountOptions options{};
        options.order = PLUGIN_PAK_ORDER_DEFAULT;
        const auto result = pak->MountResource(g_self, module, "30101", "30102", "30103",
            "ExternalMapMaterials", &options, &g_pakHandle);
        LOG_INFO("ExternalMap A2.3: MountResource result=%s (%d) handle=%p module=%p",
            pak->ResultToString(result), static_cast<int>(result),
            g_pakHandle, module);
        if (result == PLUGIN_PAK_ALREADY_MOUNTED)
        {
            LOG_ERROR("ExternalMap A2.3: ALREADY_MOUNTED does not prove freshness; full restart required; gate CLOSED");
            return;
        }
        if (result != PLUGIN_PAK_OK || !g_pakHandle) return;
        ExternalMapNative::StrongObjectLease composeLease,presentLease;
        auto* compose=ExternalMapProof::LoadMaterial("/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose",
            SDK::EMaterialDomain::MD_Surface,composeLease);
        if (!ExternalMapNative::CanOperate()) return;
        auto* present=ExternalMapProof::LoadMaterial("/Game/Mods/ExternalMap/Materials/M_BuildingPresent.M_BuildingPresent",
            SDK::EMaterialDomain::MD_UI,presentLease);
        if(compose && present)
            g_materialsReady=ExternalMapProof::ValidateAndMeasure(SDK::UWorld::GetWorld(),compose,present);
        LOG_WARN("ExternalMap A2.3: diagnostic operation finished; actual Slate/presentation/lifetime remain UNVERIFIED");
    }
}

namespace ExternalMapRenderingProof
{
    bool Initialize()
    {
        auto* engine = g_self->hooks->Engine;
        auto* world=g_self->hooks->World;
        auto* walker=g_self->hooks->ObjectWalker;
        if (!engine || !engine->RegisterOnTick || !engine->UnregisterOnTick || !world ||
            !world->RegisterOnBeforeWorldEndPlay || !world->UnregisterOnBeforeWorldEndPlay ||
            !walker || !walker->IsReady || !walker->FindObjectsByClassNameInto) return false;
        if(!ExternalMapNative::InstallTerrainHooks(g_self,ObserveTerrainConstruct,ObserveTerrainDestruct)) return false;
        DiscoverMiniMapBuildingApi(); // Optional dependency; ExternalMap initialization remains independent.
        LOG_INFO("ExternalMap A2.3B: early terrain lifecycle ready; BuildingLayer publication enabled after material proof");
        world->RegisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        g_attempted = false;
        g_materialsReady = false;
        g_pakHandle = 0;
        engine->RegisterOnTick(OnTick);
        g_registered = true;
        return true;
    }

    void Shutdown()
    {
#ifdef MODLOADER_CLIENT_BUILD
        ReleaseMiniMapBuildingApi();
        ExternalMapBuildingLayer::Shutdown();
#endif
        ExternalMapNative::RemoveTerrainHooks();
        LOG_INFO("ExternalMap A2.3: proof cleanup leases=%zu",ExternalMapNative::OutstandingLeaseCount());
        if (g_registered && g_self && g_self->hooks && g_self->hooks->Engine)
        {
            g_self->hooks->Engine->UnregisterOnTick(OnTick);
            g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        }
        g_registered = false;
        // Registry owns mounted content for process lifetime. Never unmount to evade restart.
    }
}
