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
#include "BuildingComplexConsumer.h"
#endif

namespace
{
    bool g_registered = false;
    bool g_resourceSetupAttempted = false;
    bool g_materialsReady = false;
    PluginPakHandle g_pakHandle = 0;
#ifdef MODLOADER_CLIENT_BUILD
    HMODULE g_miniMapModule=nullptr;
    MiniMapBuildingSessionApi g_miniMapBuildingApi{};
    bool g_miniMapSessionActive=false;

    void DiscoverMiniMapBuildingApi()
    {
        HMODULE module=nullptr;
        if(!GetModuleHandleExW(0,L"MiniMap.dll",&module))return;
        auto entry=reinterpret_cast<decltype(&MiniMap_GetBuildingSessionApi)>(
            GetProcAddress(module,"MiniMap_GetBuildingSessionApi"));
        MiniMapBuildingSessionApi api{};
        api.StructSize=sizeof(api);
        if(!entry || entry(MINIMAP_BUILDING_SESSION_ABI_VERSION,&api)!=MINIMAP_BUILDING_ABI_OK ||
            api.Version!=MINIMAP_BUILDING_SESSION_ABI_VERSION ||
            !api.Render || !api.Close)
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
        const auto result=g_miniMapBuildingApi.Close();
        if(result==MINIMAP_BUILDING_ABI_OK)
        {
            g_miniMapSessionActive=false;
        }
        return !g_miniMapSessionActive;
    }

    void ReleaseMiniMapBuildingApi(bool providerCallsAllowed)
    {
        if(!providerCallsAllowed && g_miniMapSessionActive)
        {
            LOG_ERROR("MainMap: legacy MiniMap closure unresolved outside early shutdown; retaining ownership without provider calls");
            return;
        }
        if(!CloseMiniMapBuildingSession())
        {
            LOG_ERROR("ExternalMap: MiniMap session close was not admitted; retaining its module reference");
            return;
        }
        g_miniMapBuildingApi={};
        if(g_miniMapModule)FreeLibrary(g_miniMapModule);
        g_miniMapModule=nullptr;
    }

    void ServiceMiniMapBuildingSession(float delta)
    {
        if(!g_miniMapBuildingApi.Render || !MainMapNative::CanOperate())return;
        // A rejected invocation may already have acquired provider ownership.
        // Only explicit successful Close is an ownership acknowledgement.
        g_miniMapSessionActive=true;
        MiniMapBuildingRenderStatus status{};status.StructSize=sizeof(status);
        MainMapBuildingLayer::RenderBuildings(g_miniMapBuildingApi.Render,delta,status);
    }

#endif
    void ObserveTerrainConstruct(SDK::UObject* terrain)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::TerrainConstruct(terrain);
#endif
    }
    void ObserveTerrainDestruct(SDK::UObject* terrain,bool admitted)
    {
#ifdef MODLOADER_CLIENT_BUILD
        const bool closed=MainMapBuildingLayer::TerrainDestruct(terrain,admitted);
        if(closed || !admitted)MainMapBuildingComplex::RequestClose();
        if(admitted)MainMapBuildingComplex::ServiceClose();
        if(closed)CloseMiniMapBuildingSession();
#endif
    }
    void BeforeWorldEnd(SDK::UWorld* world,const char*)
    {
#ifdef MODLOADER_CLIENT_BUILD
        const bool closed=MainMapBuildingLayer::BeforeWorldEnd(world);
        if(closed || !MainMapNative::IsGameThread())MainMapBuildingComplex::RequestClose();
        if(MainMapNative::CanOperate())MainMapBuildingComplex::ServiceClose();
        if(closed)CloseMiniMapBuildingSession();
#endif
    }

    void OnTick(float delta)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::Tick(g_materialsReady);
        MainMapBuildingComplex::Tick();
        if(!MainMapNative::CanOperate())return;
        if(!MainMapBuildingLayer::HasActiveTerrainSession())CloseMiniMapBuildingSession();
        else ServiceMiniMapBuildingSession(delta);
#endif
        if (g_resourceSetupAttempted || !MainMapNative::CanOperate()) return;
        g_resourceSetupAttempted = true; // Mount acceptance is intentionally attempted once.
        auto* pak = g_self->hooks->Pak;
        if (!pak || !pak->IsAvailable || !pak->IsAvailable() || !pak->MountResource ||
            !pak->LoadObject || !pak->ResultToString)
        {
            LOG_ERROR("MainMap: required pak API is unavailable");
            return;
        }
        HMODULE module = nullptr;
        // Resolve the DLL containing this function without acquiring another module reference.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&OnTick), &module))
        {
            LOG_ERROR("MainMap: plugin module lookup for embedded materials failed");
            return;
        }
        PluginPakMountOptions options{};
        options.order = PLUGIN_PAK_ORDER_DEFAULT;
        const auto result = pak->MountResource(g_self, module, "30101", "30102", "30103",
            "ExternalMapMaterials", &options, &g_pakHandle);
        if (result == PLUGIN_PAK_ALREADY_MOUNTED)
        {
            LOG_ERROR("MainMap: embedded material mount reported already mounted; restart required");
            return;
        }
        if (result != PLUGIN_PAK_OK || !g_pakHandle)
        {
            LOG_ERROR("MainMap: required embedded material mount failed: %s (%d), handle=%p",
                pak->ResultToString(result),static_cast<int>(result),g_pakHandle);
            return;
        }
        MainMapNative::StrongObjectLease composeLease,presentLease;
        auto* compose=MainMapProof::LoadMaterial("/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose",
            SDK::EMaterialDomain::MD_Surface,composeLease);
        if (!MainMapNative::CanOperate()) return;
        auto* present=MainMapProof::LoadMaterial("/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
            SDK::EMaterialDomain::MD_UI,presentLease);
        if(compose && present)
            g_materialsReady=true;
    }
}

namespace MainMapRenderingProof
{
    bool Initialize()
    {
        auto* engine = g_self->hooks->Engine;
        auto* world=g_self->hooks->World;
        if (!engine || !engine->RegisterOnTick || !engine->UnregisterOnTick || !world ||
            !world->RegisterOnBeforeWorldEndPlay || !world->UnregisterOnBeforeWorldEndPlay)
        {
            LOG_ERROR("MainMap: required engine tick/world-end callbacks are unavailable");
            return false;
        }
        if(!MainMapNative::InstallTerrainHooks(g_self,ObserveTerrainConstruct,ObserveTerrainDestruct)) return false;
        DiscoverMiniMapBuildingApi(); // Optional dependency; ExternalMap initialization remains independent.
        MainMapBuildingComplex::Initialize();
        world->RegisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        g_resourceSetupAttempted = false;
        g_materialsReady = false;
        g_pakHandle = 0;
        engine->RegisterOnTick(OnTick);
        g_registered = true;
        return true;
    }

    void Shutdown(bool earlyEngineShutdown)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingComplex::Shutdown(earlyEngineShutdown);
        ReleaseMiniMapBuildingApi(earlyEngineShutdown);
        MainMapBuildingLayer::Shutdown();
#endif
        MainMapNative::RemoveTerrainHooks();
        if (g_registered && g_self && g_self->hooks && g_self->hooks->Engine)
        {
            g_self->hooks->Engine->UnregisterOnTick(OnTick);
            g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        }
        g_registered = false;
        // Registry owns mounted content for process lifetime. Never unmount to evade restart.
    }
}
