#include "RenderingProof.h"
#include "Native/NativeApi.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#include "MaterialProof.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "BuildingLayer.h"
#endif

namespace
{
    bool g_registered = false;
    bool g_attempted = false;
    bool g_materialsReady = false;
    PluginPakHandle g_pakHandle = 0;
    void ObserveTerrainConstruct(SDK::UObject* terrain)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::TerrainConstruct(terrain);
#endif
    }
    void ObserveTerrainDestruct(SDK::UObject* terrain,bool admitted)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::TerrainDestruct(terrain,admitted);
#endif
    }
    void InspectExistingTerrain(const char* boundary)
    {
        if(!MainMapNative::CanOperate())
        {
            LOG_WARN("MainMap A2.3: terrain lookup at %s rejected by admission",boundary);
            return;
        }
        auto* walker=g_self->hooks->ObjectWalker;
        if(!walker->IsReady()) return;
        for(const char* cls : {"WBP_MapMenuTerrain_C","CrUW_MapMenuTerrain","WBP_MapMenuMapArea_C","WBP_MapMenu_C"})
        {
            PluginObjectInfo objects[4]{};
            const int total=walker->FindObjectsByClassNameInto(cls,PluginObjectLookup_InstanceOnly,objects,4);
            LOG_INFO("MainMap A2.3: terrain lookup boundary=%s class=%s instances=%d",boundary,cls,total);
            for(int i=0;i<total && i<4;++i)
                LOG_INFO("MainMap A2.3: terrain lookup object=%p index=%d name=%s",objects[i].object,
                    objects[i].objectIndex,objects[i].objectName);
        }
    }
    void BeforeWorldEnd(SDK::UWorld* world,const char*)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::BeforeWorldEnd(world);
#endif
        InspectExistingTerrain("before-world-end");
    }

    void OnTick(float)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::Tick(g_materialsReady);
#endif
        if (g_attempted || !MainMapNative::CanOperate()) return;
        g_attempted = true; // No retry after timeout or uncertain engine acceptance.
        InspectExistingTerrain("initial-admitted-tick");
        auto* pak = g_self->hooks->Pak;
        if (!pak || !pak->IsAvailable || !pak->IsAvailable() || !pak->MountResource ||
            !pak->LoadObject || !pak->ResultToString)
        {
            LOG_ERROR("MainMap A2.3: pak API unavailable; compatibility gate CLOSED");
            return;
        }
        HMODULE module = nullptr;
        // Resolve the DLL containing this function without acquiring another module reference.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&OnTick), &module))
        {
            LOG_ERROR("MainMap A2.3: actual MainMap module lookup failed");
            return;
        }
        PluginPakMountOptions options{};
        options.order = PLUGIN_PAK_ORDER_DEFAULT;
        const auto result = pak->MountResource(g_self, module, "30101", "30102", "30103",
            "MainMapMaterials", &options, &g_pakHandle);
        LOG_INFO("MainMap A2.3: MountResource result=%s (%d) handle=%p module=%p",
            pak->ResultToString(result), static_cast<int>(result),
            g_pakHandle, module);
        if (result == PLUGIN_PAK_ALREADY_MOUNTED)
        {
            LOG_ERROR("MainMap A2.3: ALREADY_MOUNTED does not prove freshness; full restart required; gate CLOSED");
            return;
        }
        if (result != PLUGIN_PAK_OK || !g_pakHandle) return;
        MainMapNative::StrongObjectLease composeLease,presentLease;
        auto* compose=MainMapProof::LoadMaterial("/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose",
            SDK::EMaterialDomain::MD_Surface,composeLease);
        if (!MainMapNative::CanOperate()) return;
        auto* present=MainMapProof::LoadMaterial("/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
            SDK::EMaterialDomain::MD_UI,presentLease);
        if(compose && present)
            g_materialsReady=MainMapProof::ValidateAndMeasure(SDK::UWorld::GetWorld(),compose,present);
        LOG_WARN("MainMap A2.3: diagnostic operation finished; actual Slate/presentation/lifetime remain UNVERIFIED");
    }
}

namespace MainMapRenderingProof
{
    bool Initialize()
    {
        auto* engine = g_self->hooks->Engine;
        auto* world=g_self->hooks->World;
        auto* walker=g_self->hooks->ObjectWalker;
        if (!engine || !engine->RegisterOnTick || !engine->UnregisterOnTick || !world ||
            !world->RegisterOnBeforeWorldEndPlay || !world->UnregisterOnBeforeWorldEndPlay ||
            !walker || !walker->IsReady || !walker->FindObjectsByClassNameInto) return false;
        if(!MainMapNative::InstallTerrainHooks(g_self,ObserveTerrainConstruct,ObserveTerrainDestruct)) return false;
        LOG_INFO("MainMap A2.3B: early terrain lifecycle ready; BuildingLayer publication enabled after material proof");
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
        MainMapBuildingLayer::Shutdown();
#endif
        MainMapNative::RemoveTerrainHooks();
        LOG_INFO("MainMap A2.3: proof cleanup leases=%zu",MainMapNative::OutstandingLeaseCount());
        if (g_registered && g_self && g_self->hooks && g_self->hooks->Engine)
        {
            g_self->hooks->Engine->UnregisterOnTick(OnTick);
            g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        }
        g_registered = false;
        // Registry owns mounted content for process lifetime. Never unmount to evade restart.
    }
}
