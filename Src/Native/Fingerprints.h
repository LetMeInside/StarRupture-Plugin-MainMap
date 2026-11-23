#pragma once

#include <cstdint>

struct IPluginSelf;
struct IPluginHookScanner;

namespace MainMapFingerprints
{
    struct ResolvedAddresses
    {
        uintptr_t retainObject = 0;
        uintptr_t releaseObject = 0;
        uintptr_t isGameThread = 0;
        uintptr_t activeGarbageCollection = 0;
        uintptr_t exitPurge = 0;
        uintptr_t isGarbageCollectingAndLocking = 0;
        uintptr_t isIncrementalPurgePending = 0;
        uintptr_t isIncrementalReachabilityPending = 0;
        uintptr_t terrainConstruct = 0;
        uintptr_t terrainDestruct = 0;
        uintptr_t mapMenuNativeTick = 0;
        uintptr_t mapMenuNativeOnDeactivated = 0;
    };

    // Every entry is mandatory. A miss rejects plugin loading.
    bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner,
        ResolvedAddresses& addresses);
}
