#pragma once

#include <cstddef>
struct IPluginSelf;

namespace SDK { class UObject; }
namespace MainMapFingerprints { struct ResolvedAddresses; }

namespace MainMapNative
{
    bool Initialize(const MainMapFingerprints::ResolvedAddresses& addresses);
    bool Available() noexcept;
    bool IsGameThread() noexcept;
    // Same admission predicates as lease acquisition; no separate lifetime state.
    bool CanOperate() noexcept;
    using TerrainCallback = void (*)(SDK::UObject* terrain);
    bool InstallTerrainHooks(IPluginSelf* self, TerrainCallback afterConstruct,
        TerrainCallback beforeDestruct);
    void RemoveTerrainHooks() noexcept;
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
