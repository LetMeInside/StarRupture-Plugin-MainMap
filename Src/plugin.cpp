#include "plugin.h"
#include "plugin_helpers.h"
#include "Config/Config.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "Native/Fingerprints.h"
#include "Native/NativeApi.h"
#include "Map/RenderingProof.h"
#endif

IPluginSelf* g_self = nullptr;
IPluginSelf* GetSelf() { return g_self; }

#ifdef MODLOADER_CLIENT_BUILD
namespace
{
    MainMapFingerprints::ResolvedAddresses g_nativeAddresses{};
    bool g_shutdownCallbackRegistered = false;
    bool g_engineShutdownSeen = false;

    void OnEngineShutdown()
    {
        g_engineShutdownSeen = true;
        MainMapRenderingProof::Shutdown();
        MainMapNative::BeginShutdown(true);
        if (!MainMapNative::Shutdown())
            LOG_ERROR("MainMap: UObject leases remained at engine shutdown");
    }

    void UnregisterEngineShutdownCallback()
    {
        if (!g_shutdownCallbackRegistered || g_engineShutdownSeen || !g_self ||
            !g_self->hooks || !g_self->hooks->Engine ||
            !g_self->hooks->Engine->UnregisterOnShutdown)
            return;
        g_self->hooks->Engine->UnregisterOnShutdown(OnEngineShutdown);
        g_shutdownCallbackRegistered = false;
    }
}
#endif

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "0.1.0"
#endif

#ifdef MODLOADER_SERVER_BUILD
#define MAINMAP_EXPORTS_TARGET PLUGIN_TARGET_SERVER
#else
#define MAINMAP_EXPORTS_TARGET PLUGIN_TARGET_CLIENT
#endif

static PluginInfo s_pluginInfo = {
    "MainMap",
    MODLOADER_BUILD_TAG,
    "Kian369",
    "TODO: Add plugin description",
    PLUGIN_INTERFACE_VERSION,
    MAINMAP_EXPORTS_TARGET
};

extern "C" __declspec(dllexport)
void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
{
#ifdef MODLOADER_CLIENT_BUILD
    g_nativeAddresses = {};
    if (!MainMapFingerprints::Resolve(self, scanner, g_nativeAddresses))
    {
        LOG_ERROR("MainMap: mandatory native lifetime fingerprint resolution failed");
        return;
    }
#else
    (void)self;
    (void)scanner;
#endif
}

extern "C"
{
    __declspec(dllexport)
    PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport)
    bool PluginInit(IPluginSelf* self)
    {
        g_self = self;
        if (g_self == nullptr || g_self->hooks == nullptr)
        {
            g_self = nullptr;
            return false;
        }

#ifdef MODLOADER_CLIENT_BUILD
        if (!MainMapNative::Initialize(g_nativeAddresses))
        {
            LOG_ERROR("MainMap: native lifetime API initialization failed");
            g_self = nullptr;
            return false;
        }
        g_nativeAddresses = {};

        if (!g_self->hooks->Engine || !g_self->hooks->Engine->RegisterOnShutdown ||
            !g_self->hooks->Engine->UnregisterOnShutdown)
        {
            LOG_ERROR("MainMap: required early engine-shutdown callback is unavailable");
            MainMapNative::Shutdown();
            g_self = nullptr;
            return false;
        }
        g_engineShutdownSeen = false;
        g_self->hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
        g_shutdownCallbackRegistered = true;
#endif

        if (!MainMapConfig::Initialize(g_self))
        {
#ifdef MODLOADER_CLIENT_BUILD
            UnregisterEngineShutdownCallback();
            MainMapNative::Shutdown();
#endif
            g_self = nullptr;
            return false;
        }

#ifdef MODLOADER_CLIENT_BUILD
        if (MainMapConfig::IsEnabled() && !MainMapRenderingProof::Initialize())
        {
            UnregisterEngineShutdownCallback();
            MainMapNative::Shutdown();
            g_self = nullptr;
            return false;
        }
#endif
        LOG_INFO("MainMap: initialization complete");
        return true;
    }

    __declspec(dllexport)
    void PluginShutdown()
    {
        LOG_INFO("MainMap: shutting down");
#ifdef MODLOADER_CLIENT_BUILD
        MainMapRenderingProof::Shutdown();
        UnregisterEngineShutdownCallback();
        MainMapNative::BeginShutdown(false);
        if (!MainMapNative::Shutdown())
            LOG_ERROR("MainMap: native API remains unavailable because UObject leases were not released");
#endif
        g_self = nullptr;
    }
}
