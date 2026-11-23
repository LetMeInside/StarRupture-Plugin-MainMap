#pragma once

#include <cstddef>
#include <cstdint>
struct IPluginSelf;

namespace SDK { class UObject; }
namespace SDK { class UTexture2D; }
namespace SDK { class FName; }
namespace SDK { struct FGeometry; }
namespace SDK { enum class EPixelFormat : uint8_t; }
namespace MainMapFingerprints { struct ResolvedAddresses; }

namespace MainMapNative
{
    /** Provides typed access to mandatory resolved engine functions under MainMap admission. */
    class NativeApi final
    {
    public:
        /** Creates a transient texture from width/height, format, and borrowed pixel bytes; returns null when admission or creation fails. The engine copies the bytes before returning. */
        static SDK::UTexture2D* CreateTransientTexture(int32_t width, int32_t height,
            SDK::EPixelFormat format, const uint8_t* pixels, int64_t byteCount) noexcept;
    };

    bool Initialize(const MainMapFingerprints::ResolvedAddresses& addresses);
    bool Available() noexcept;
    bool IsGameThread() noexcept;
    // Same admission predicates as lease acquisition; no separate lifetime state.
    bool CanOperate() noexcept;
    using TerrainCallback = void (*)(SDK::UObject* terrain);
    // Rejected destruction is notification only: consumer must not touch
    // UObjects or session ownership; it may request cleanup using atomic state.
    using TerrainDestructCallback = void (*)(SDK::UObject* terrain, bool admitted);
    using MapMenuTickCallback = void (*)(SDK::UObject* menu,
        const SDK::FGeometry& geometry, float deltaTime);
    using MapMenuDeactivatedCallback = void (*)(SDK::UObject* menu);
    // Installs all required terrain/menu hooks and rolls back the full set on any failure.
    bool InstallLifecycleHooks(IPluginSelf* self, TerrainCallback afterConstruct,
        TerrainDestructCallback beforeDestruct, MapMenuTickCallback beforeMenuTick,
        MapMenuDeactivatedCallback beforeMenuDeactivated);
    // Removes the menu hooks before terrain hooks and clears every typed original pointer.
    void RemoveLifecycleHooks() noexcept;
    size_t OutstandingLeaseCount() noexcept;

    // Closes new acquisition. Existing leases must be reset before engine
    // shutdown. The release table remains live if Shutdown refuses due to
    // outstanding leases.
    void BeginShutdown(bool engineTeardown) noexcept;
    bool Shutdown() noexcept;

    class StrongObjectLease final
    {
    public:
        StrongObjectLease() noexcept = default;
        ~StrongObjectLease() noexcept;

        StrongObjectLease(const StrongObjectLease&) = delete;
        StrongObjectLease& operator=(const StrongObjectLease&) = delete;

        StrongObjectLease(StrongObjectLease&& other) noexcept;
        StrongObjectLease& operator=(StrongObjectLease&& other) noexcept;

        // Acquisition succeeds only into an empty lease. The exact supplied
        // UObject is retained; its Outer is never substituted.
        bool Acquire(SDK::UObject* object) noexcept;
        // On failure the lease remains non-empty and retains its ownership
        // record, allowing the caller to retry while the lifecycle is valid.
        bool Reset() noexcept;

        SDK::UObject* Get() const noexcept { return object_; }
        explicit operator bool() const noexcept { return object_ != nullptr; }

    private:
        SDK::UObject* object_ = nullptr;
    };
}
