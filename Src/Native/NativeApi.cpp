#include "NativeApi.h"

#include "Fingerprints.h"
#include "Basic.hpp"
#include "CoreUObject_structs.hpp"
#include "plugin_helpers.h"
#include "plugin_interface.h"

#include <mutex>

namespace
{
    struct NativeFunctions
    {
        using ObjectFn = void (*)(SDK::UObject*);
        using IsGameThreadFn = bool (*)();
        using PredicateFn = bool (*)();
        using MapMenuTickFn = void (*)(SDK::UObject*, const SDK::FGeometry&, float);
        /** Matches UE 5.6 TConstArrayView64<uint8>: data pointer followed by signed 64-bit length. */
        struct ConstByteView64 final
        {
            const uint8_t* Data = nullptr;
            int64_t Num = 0;
        };
        static_assert(sizeof(ConstByteView64) == 16);
        static_assert(alignof(ConstByteView64) == 8);
        static_assert(offsetof(ConstByteView64, Data) == 0);
        static_assert(offsetof(ConstByteView64, Num) == 8);
        /** Matches the static UTexture2D overload's x64 native parameter sequence. */
        using TextureCreateTransientFn = SDK::UTexture2D* (__fastcall*)(int32_t, int32_t,
            SDK::EPixelFormat, SDK::FName, ConstByteView64);
        static_assert(sizeof(SDK::FName) == 8);
        static_assert(sizeof(SDK::EPixelFormat) == 1);

        ObjectFn retainObject = nullptr;
        ObjectFn releaseObject = nullptr;
        IsGameThreadFn isGameThread = nullptr;
        const bool* activeGarbageCollection = nullptr;
        const uint8_t* exitPurge = nullptr;
        PredicateFn isGarbageCollectingAndLocking = nullptr;
        PredicateFn isIncrementalPurgePending = nullptr;
        PredicateFn isIncrementalReachabilityPending = nullptr;
        ObjectFn terrainConstruct = nullptr;
        ObjectFn terrainDestruct = nullptr;
        MapMenuTickFn mapMenuNativeTick = nullptr;
        ObjectFn mapMenuNativeOnDeactivated = nullptr;
        TextureCreateTransientFn textureCreateTransient = nullptr;
        ObjectFn originalTerrainConstruct = nullptr;
        ObjectFn originalTerrainDestruct = nullptr;
        MapMenuTickFn originalMapMenuNativeTick = nullptr;
        ObjectFn originalMapMenuNativeOnDeactivated = nullptr;
    };

    NativeFunctions g_functions{};
    std::mutex g_mutex;
    bool g_initialized = false;
    bool g_accepting = false;
    bool g_engineTeardown = false;
    size_t g_outstandingLeases = 0;
    HookHandle g_terrainConstructHook = nullptr;
    HookHandle g_terrainDestructHook = nullptr;
    HookHandle g_mapMenuNativeTickHook = nullptr;
    HookHandle g_mapMenuNativeOnDeactivatedHook = nullptr;
    IPluginHookUtils* g_hooks = nullptr;
    MainMapNative::TerrainCallback g_afterTerrainConstruct = nullptr;
    MainMapNative::TerrainDestructCallback g_beforeTerrainDestruct = nullptr;
    MainMapNative::MapMenuTickCallback g_beforeMapMenuTick = nullptr;
    MainMapNative::MapMenuDeactivatedCallback g_beforeMapMenuDeactivated = nullptr;

    bool AdmitTerrainBoundary()
    {
        return MainMapNative::CanOperate();
    }

    void TerrainConstructDetour(SDK::UObject* terrain)
    {
        // Exactly one original call. Publication is scheduled later outside rebuilding.
        const auto original=g_functions.originalTerrainConstruct;
        original(terrain);
        if(AdmitTerrainBoundary() && g_afterTerrainConstruct)
            g_afterTerrainConstruct(terrain);
    }

    void TerrainDestructDetour(SDK::UObject* terrain)
    {
        // Base NativeDestruct is the original's first call; withdraw before it.
        const auto original=g_functions.originalTerrainDestruct;
        const bool admitted=AdmitTerrainBoundary();
        if(g_beforeTerrainDestruct) g_beforeTerrainDestruct(terrain,admitted);
        original(terrain);
    }

    // Runs the admitted consumer hook before native menu ticking, then calls the original once.
    void MapMenuNativeTickDetour(SDK::UObject* menu,
        const SDK::FGeometry& geometry, float deltaTime)
    {
        const auto original = g_functions.originalMapMenuNativeTick;
        if (AdmitTerrainBoundary() && g_beforeMapMenuTick)
            g_beforeMapMenuTick(menu, geometry, deltaTime);
        original(menu, geometry, deltaTime);
    }

    // Withdraws admitted menu ownership before CommonUI's original deactivation path.
    void MapMenuNativeOnDeactivatedDetour(SDK::UObject* menu)
    {
        const auto original = g_functions.originalMapMenuNativeOnDeactivated;
        if (AdmitTerrainBoundary() && g_beforeMapMenuDeactivated)
            g_beforeMapMenuDeactivated(menu);
        original(menu);
    }

    bool IsGCAdmittedOnGameThread() noexcept
    {
        return g_functions.activeGarbageCollection && !*g_functions.activeGarbageCollection &&
            g_functions.exitPurge && *g_functions.exitPurge == 0 &&
            g_functions.isGarbageCollectingAndLocking && !g_functions.isGarbageCollectingAndLocking() &&
            g_functions.isIncrementalPurgePending && !g_functions.isIncrementalPurgePending() &&
            g_functions.isIncrementalReachabilityPending && !g_functions.isIncrementalReachabilityPending();
    }

    // Private typed calls: only StrongObjectLease can pair them, preserving
    // exact-object ownership and the one-acquire/one-release counter.
    bool RetainObject(SDK::UObject* object) noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!object || !g_initialized || !g_accepting || g_engineTeardown ||
            !g_functions.retainObject || !g_functions.isGameThread ||
            !g_functions.isGameThread() || !IsGCAdmittedOnGameThread()) return false;
        g_functions.retainObject(object);
        ++g_outstandingLeases;
        return true;
    }

    bool ReleaseObject(SDK::UObject* object) noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!object || !g_initialized || g_engineTeardown || !g_functions.releaseObject ||
            !g_functions.isGameThread || !g_functions.isGameThread() ||
            !IsGCAdmittedOnGameThread() || g_outstandingLeases == 0)
            return false;

        g_functions.releaseObject(object);
        --g_outstandingLeases;
        return true;
    }
}

namespace MainMapNative
{
    bool Initialize(const MainMapFingerprints::ResolvedAddresses& addresses)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_initialized || !addresses.retainObject || !addresses.releaseObject ||
            !addresses.isGameThread || !addresses.activeGarbageCollection || !addresses.exitPurge ||
            !addresses.isGarbageCollectingAndLocking || !addresses.isIncrementalPurgePending ||
            !addresses.isIncrementalReachabilityPending || !addresses.terrainConstruct || !addresses.terrainDestruct ||
            !addresses.mapMenuNativeTick || !addresses.mapMenuNativeOnDeactivated ||
            !addresses.textureCreateTransient)
            return false;

        NativeFunctions candidate{};
        candidate.retainObject = reinterpret_cast<NativeFunctions::ObjectFn>(addresses.retainObject);
        candidate.releaseObject = reinterpret_cast<NativeFunctions::ObjectFn>(addresses.releaseObject);
        candidate.isGameThread = reinterpret_cast<NativeFunctions::IsGameThreadFn>(addresses.isGameThread);
        candidate.activeGarbageCollection = reinterpret_cast<const bool*>(addresses.activeGarbageCollection);
        candidate.exitPurge = reinterpret_cast<const uint8_t*>(addresses.exitPurge);
        candidate.isGarbageCollectingAndLocking = reinterpret_cast<NativeFunctions::PredicateFn>(addresses.isGarbageCollectingAndLocking);
        candidate.isIncrementalPurgePending = reinterpret_cast<NativeFunctions::PredicateFn>(addresses.isIncrementalPurgePending);
        candidate.isIncrementalReachabilityPending = reinterpret_cast<NativeFunctions::PredicateFn>(addresses.isIncrementalReachabilityPending);
        candidate.terrainConstruct = reinterpret_cast<NativeFunctions::ObjectFn>(addresses.terrainConstruct);
        candidate.terrainDestruct = reinterpret_cast<NativeFunctions::ObjectFn>(addresses.terrainDestruct);
        candidate.mapMenuNativeTick = reinterpret_cast<NativeFunctions::MapMenuTickFn>(addresses.mapMenuNativeTick);
        candidate.mapMenuNativeOnDeactivated = reinterpret_cast<NativeFunctions::ObjectFn>(addresses.mapMenuNativeOnDeactivated);
        candidate.textureCreateTransient = reinterpret_cast<NativeFunctions::TextureCreateTransientFn>(
            addresses.textureCreateTransient);

        if (!candidate.retainObject || !candidate.releaseObject || !candidate.isGameThread ||
            !candidate.activeGarbageCollection || !candidate.exitPurge ||
            !candidate.isGarbageCollectingAndLocking || !candidate.isIncrementalPurgePending ||
            !candidate.isIncrementalReachabilityPending || !candidate.terrainConstruct ||
            !candidate.terrainDestruct || !candidate.mapMenuNativeTick ||
            !candidate.mapMenuNativeOnDeactivated || !candidate.textureCreateTransient)
            return false;

        g_functions = candidate;
        g_engineTeardown = false;
        g_outstandingLeases = 0;
        g_initialized = true;
        g_accepting = true;
        return true;
    }

    bool Available() noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_initialized && g_accepting && !g_engineTeardown &&
            g_functions.retainObject && g_functions.releaseObject && g_functions.isGameThread;
    }

    bool IsGameThread() noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_initialized && !g_engineTeardown && g_functions.isGameThread &&
            g_functions.isGameThread();
    }

    bool CanOperate() noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_initialized && g_accepting && !g_engineTeardown &&
            g_functions.isGameThread && g_functions.isGameThread() &&
            IsGCAdmittedOnGameThread();
    }

    /** Calls the typed engine texture constructor only while MainMap native admission is open. */
    SDK::UTexture2D* NativeApi::CreateTransientTexture(int32_t width, int32_t height,
        SDK::EPixelFormat format, const uint8_t* pixels, int64_t byteCount) noexcept
    {
        if (!pixels || byteCount <= 0 || !CanOperate() || !g_functions.textureCreateTransient)
        {
            return nullptr;
        }
        const NativeFunctions::ConstByteView64 initialData{pixels, byteCount};
        return g_functions.textureCreateTransient(width, height, format, SDK::FName{}, initialData);
    }

    size_t OutstandingLeaseCount() noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_outstandingLeases;
    }

    bool InstallLifecycleHooks(IPluginSelf* self, TerrainCallback afterConstruct,
        TerrainDestructCallback beforeDestruct, MapMenuTickCallback beforeMenuTick,
        MapMenuDeactivatedCallback beforeMenuDeactivated)
    {
        // Installation is native code patching through the loader, not UObject
        // access. PluginInit is its supported boundary while engine-ready waits
        // for plugins. UObject access still requires CanOperate; a rejected
        // destruct notification may only set an atomic cleanup request.
        if(!Available() || !self || !self->hooks || !self->hooks->Hooks ||
            !self->hooks->Hooks->Install || !self->hooks->Hooks->Remove ||
            !self->hooks->Hooks->IsInstalled || !afterConstruct || !beforeDestruct ||
            !beforeMenuTick || !beforeMenuDeactivated || g_terrainConstructHook ||
            g_terrainDestructHook || g_mapMenuNativeTickHook || g_mapMenuNativeOnDeactivatedHook)
            return false;
        g_hooks=self->hooks->Hooks;
        g_afterTerrainConstruct=afterConstruct;
        g_beforeTerrainDestruct=beforeDestruct;
        g_beforeMapMenuTick=beforeMenuTick;
        g_beforeMapMenuDeactivated=beforeMenuDeactivated;
        g_terrainConstructHook=g_hooks->Install(reinterpret_cast<uintptr_t>(g_functions.terrainConstruct),
            reinterpret_cast<void*>(&TerrainConstructDetour),reinterpret_cast<void**>(&g_functions.originalTerrainConstruct));
        if(g_terrainConstructHook)
            g_terrainDestructHook=g_hooks->Install(reinterpret_cast<uintptr_t>(g_functions.terrainDestruct),
                reinterpret_cast<void*>(&TerrainDestructDetour),reinterpret_cast<void**>(&g_functions.originalTerrainDestruct));
        if(g_terrainDestructHook)
            g_mapMenuNativeTickHook=g_hooks->Install(reinterpret_cast<uintptr_t>(g_functions.mapMenuNativeTick),
                reinterpret_cast<void*>(&MapMenuNativeTickDetour),reinterpret_cast<void**>(&g_functions.originalMapMenuNativeTick));
        if(g_mapMenuNativeTickHook)
            g_mapMenuNativeOnDeactivatedHook=g_hooks->Install(
                reinterpret_cast<uintptr_t>(g_functions.mapMenuNativeOnDeactivated),
                reinterpret_cast<void*>(&MapMenuNativeOnDeactivatedDetour),
                reinterpret_cast<void**>(&g_functions.originalMapMenuNativeOnDeactivated));
        if(!g_terrainConstructHook || !g_terrainDestructHook || !g_mapMenuNativeTickHook ||
            !g_mapMenuNativeOnDeactivatedHook ||
            !g_functions.originalTerrainConstruct || !g_functions.originalTerrainDestruct ||
            !g_functions.originalMapMenuNativeTick || !g_functions.originalMapMenuNativeOnDeactivated ||
            !g_hooks->IsInstalled(g_terrainConstructHook) || !g_hooks->IsInstalled(g_terrainDestructHook) ||
            !g_hooks->IsInstalled(g_mapMenuNativeTickHook) ||
            !g_hooks->IsInstalled(g_mapMenuNativeOnDeactivatedHook))
        {
            RemoveLifecycleHooks();
            LOG_ERROR("MainMap: mandatory native lifecycle hook installation FAILED");
            return false;
        }
        return true;
    }

    void RemoveLifecycleHooks() noexcept
    {
        if(g_hooks)
        {
            if(g_mapMenuNativeOnDeactivatedHook) g_hooks->Remove(g_mapMenuNativeOnDeactivatedHook);
            if(g_mapMenuNativeTickHook) g_hooks->Remove(g_mapMenuNativeTickHook);
            if(g_terrainDestructHook) g_hooks->Remove(g_terrainDestructHook);
            if(g_terrainConstructHook) g_hooks->Remove(g_terrainConstructHook);
        }
        g_terrainConstructHook=g_terrainDestructHook=nullptr;
        g_mapMenuNativeTickHook=g_mapMenuNativeOnDeactivatedHook=nullptr;
        g_hooks=nullptr;
        g_afterTerrainConstruct=nullptr;
        g_beforeTerrainDestruct=nullptr;
        g_beforeMapMenuTick=nullptr;
        g_beforeMapMenuDeactivated=nullptr;
        g_functions.originalTerrainConstruct=g_functions.originalTerrainDestruct=nullptr;
        g_functions.originalMapMenuNativeTick=nullptr;
        g_functions.originalMapMenuNativeOnDeactivated=nullptr;
    }

    void BeginShutdown(bool engineTeardown) noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_accepting = false;
        if (engineTeardown) g_engineTeardown = true;
    }

    bool Shutdown() noexcept
    {
        // Callers remove hooks on the admitted GT before closing the lifetime table.
        if(g_terrainConstructHook || g_terrainDestructHook || g_mapMenuNativeTickHook ||
            g_mapMenuNativeOnDeactivatedHook)
        {
            LOG_ERROR("MainMap: native shutdown refused while lifecycle hooks remain installed");
            return false;
        }
        size_t outstanding = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_accepting = false;
            if (!g_initialized) return true;
            outstanding = g_outstandingLeases;
            if (outstanding == 0)
            {
                g_functions = {};
                g_initialized = false;
                g_engineTeardown = false;
            }
        }

        if (outstanding != 0)
        {
            LOG_ERROR("MainMap: native API shutdown refused with %zu outstanding UObject lease(s)",
                outstanding);
            return false;
        }
        return true;
    }

    StrongObjectLease::~StrongObjectLease() noexcept
    {
        if (object_ && !Reset())
            LOG_ERROR("MainMap: UObject lease destructor could not release; lifecycle order was violated");
    }

    StrongObjectLease::StrongObjectLease(StrongObjectLease&& other) noexcept
        : object_(other.object_)
    {
        other.object_ = nullptr;
    }

    StrongObjectLease& StrongObjectLease::operator=(StrongObjectLease&& other) noexcept
    {
        if (this == &other) return *this;
        if (!Reset())
        {
            LOG_ERROR("MainMap: move assignment rejected because the current lease cannot be released");
            return *this;
        }
        object_ = other.object_;
        other.object_ = nullptr;
        return *this;
    }

    bool StrongObjectLease::Acquire(SDK::UObject* object) noexcept
    {
        if (object_ || !object || !RetainObject(object)) return false;
        object_ = object;
        return true;
    }

    bool StrongObjectLease::Reset() noexcept
    {
        if (!object_) return true;
        if (!ReleaseObject(object_)) return false;
        object_ = nullptr;
        return true;
    }
}
