#include "RenderingProof.h"
#include "Native/NativeApi.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#ifdef MODLOADER_CLIENT_BUILD
#include "BuildingLayer.h"
#include "BuildingComplexConsumer.h"
#endif

namespace
{
    bool g_registered = false;
    bool g_resourceSetupAttempted = false;
    bool g_materialContainerMounted = false;
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
        const bool closed=MainMapBuildingLayer::TerrainDestruct(terrain,admitted);
        if(closed || !admitted)MainMapBuildingComplex::RequestClose();
        if(admitted)MainMapBuildingComplex::ServiceClose();
#endif
    }

    void OnWorldBeginPlay(SDK::UWorld* world)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::WorldBeginPlay(world);
#endif
    }

    void BeforeWorldEnd(SDK::UWorld* world,const char*)
    {
#ifdef MODLOADER_CLIENT_BUILD
        const bool closed=MainMapBuildingLayer::BeforeWorldEnd(world);
        if(closed || !MainMapNative::IsGameThread())MainMapBuildingComplex::RequestClose();
        if(MainMapNative::CanOperate())MainMapBuildingComplex::ServiceClose();
#endif
    }

    void OnTick(float)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::Tick();
#endif
        // Mount the embedded material container before the BCT1 consumer can
        // load M_BuildingComplexPresent. The registry owns it for process life.
        if (!g_resourceSetupAttempted && MainMapNative::CanOperate())
        {
            g_resourceSetupAttempted = true;
            auto* pak = g_self->hooks->Pak;
            if (!pak || !pak->IsAvailable || !pak->IsAvailable() || !pak->MountResource ||
                !pak->LoadObject || !pak->ResultToString)
            {
                LOG_ERROR("MainMap: required pak API is unavailable");
            }
            else
            {
                HMODULE module = nullptr;
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&OnTick), &module))
                {
                    LOG_ERROR("MainMap: plugin module lookup for embedded materials failed");
                }
                else
                {
                    PluginPakMountOptions options{};
                    options.order = PLUGIN_PAK_ORDER_DEFAULT;
                    const auto result = pak->MountResource(g_self, module, "30101", "30102", "30103",
                        "ExternalMapMaterials", &options, &g_pakHandle);
                    if (result == PLUGIN_PAK_ALREADY_MOUNTED)
                    {
                        LOG_ERROR("MainMap: embedded material mount reported already mounted; restart required");
                    }
                    else if (result != PLUGIN_PAK_OK || !g_pakHandle)
                    {
                        LOG_ERROR("MainMap: required embedded material mount failed: %s (%d), handle=%p",
                            pak->ResultToString(result),static_cast<int>(result),g_pakHandle);
                    }
                    else
                    {
                        g_materialContainerMounted = true;
                        LOG_INFO("MainMap diagnostics: material container mount accepted name=ExternalMapMaterials handle=%p",g_pakHandle);
                    }
                }
            }
        }
#ifdef MODLOADER_CLIENT_BUILD
        const bool mainMapVisible=MainMapBuildingLayer::IsMainMapVisible();
        MainMapBuildingComplex::Tick(g_materialContainerMounted,mainMapVisible);
        MainMapBuildingComplex::SetPresentationVisible(mainMapVisible);
#endif
    }
}

namespace MainMapRenderingProof
{
    bool Initialize()
    {
        auto* engine = g_self->hooks->Engine;
        auto* world=g_self->hooks->World;
        if (!engine || !engine->RegisterOnTick || !engine->UnregisterOnTick || !world ||
            !world->RegisterOnWorldBeginPlay || !world->UnregisterOnWorldBeginPlay ||
            !world->RegisterOnBeforeWorldEndPlay || !world->UnregisterOnBeforeWorldEndPlay)
        {
            LOG_ERROR("MainMap: required engine tick/world lifecycle callbacks are unavailable");
            return false;
        }
        if(!MainMapNative::InstallTerrainHooks(g_self,ObserveTerrainConstruct,ObserveTerrainDestruct)) return false;
        MainMapBuildingComplex::Initialize();
        world->RegisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        world->RegisterOnWorldBeginPlay(OnWorldBeginPlay);
        g_resourceSetupAttempted = false;
        g_materialContainerMounted = false;
        g_pakHandle = 0;
        engine->RegisterOnTick(OnTick);
        g_registered = true;
        return true;
    }

    void Shutdown(bool earlyEngineShutdown)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingComplex::Shutdown(earlyEngineShutdown);
        MainMapBuildingLayer::Shutdown();
#endif
        MainMapNative::RemoveTerrainHooks();
        if (g_registered && g_self && g_self->hooks && g_self->hooks->Engine)
        {
            g_self->hooks->Engine->UnregisterOnTick(OnTick);
            g_self->hooks->World->UnregisterOnWorldBeginPlay(OnWorldBeginPlay);
            g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        }
        g_registered = false;
        // Registry owns mounted content for process lifetime. Never unmount to evade restart.
    }
}
