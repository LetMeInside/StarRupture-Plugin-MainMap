#include "RenderingProof.h"
#include "Native/NativeApi.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#ifdef MODLOADER_CLIENT_BUILD
#include "BuildingLayer.h"
#endif

namespace
{
    bool g_registered = false;
    bool g_resourceSetupAttempted = false;
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

    void ObserveMapMenuTick(SDK::UObject* menu, const SDK::FGeometry& geometry,
        float deltaTime)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::MapMenuNativeTick(menu, geometry, deltaTime);
#endif
    }

    void ObserveMapMenuDeactivated(SDK::UObject* menu)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::MapMenuNativeOnDeactivated(menu);
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
        MainMapBuildingLayer::BeforeWorldEnd(world);
#endif
    }

    void OnTick(float)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::Tick();
#endif
        // Mount embedded presentation materials once. The registry owns the container for process life.
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
                        MainMapRenderingProof::RenderingProof::RecordMaterialContainerReadiness(true);
                        LOG_INFO("MainMap diagnostics: material container mount accepted name=ExternalMapMaterials handle=%p",g_pakHandle);
                    }
                }
            }
        }
    }
}

namespace MainMapRenderingProof
{
    bool RenderingProof::materialContainerReady_ = false;

    /** Reports the accepted status of the existing one-time embedded resource mount. */
    bool RenderingProof::IsMaterialContainerReady() noexcept
    {
        return materialContainerReady_ && g_pakHandle != 0;
    }

    /** Records the existing mount result without creating another mount path. */
    void RenderingProof::RecordMaterialContainerReadiness(bool ready) noexcept
    {
        materialContainerReady_ = ready;
    }

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
        if(!MainMapNative::InstallLifecycleHooks(g_self, ObserveTerrainConstruct,
            ObserveTerrainDestruct, ObserveMapMenuTick, ObserveMapMenuDeactivated))
            return false;
        world->RegisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        world->RegisterOnWorldBeginPlay(OnWorldBeginPlay);
        g_resourceSetupAttempted = false;
        RenderingProof::RecordMaterialContainerReadiness(false);
        g_pakHandle = 0;
        engine->RegisterOnTick(OnTick);
        g_registered = true;
        return true;
    }

    void Shutdown(bool earlyEngineShutdown)
    {
#ifdef MODLOADER_CLIENT_BUILD
        MainMapBuildingLayer::Shutdown();
#endif
        (void)earlyEngineShutdown;
        MainMapNative::RemoveLifecycleHooks();
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
