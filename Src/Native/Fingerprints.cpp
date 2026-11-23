#include "Fingerprints.h"

#include "plugin_interface.h"

#include <Windows.h>
#include <array>
#include <cstring>

namespace
{
    bool ReadResolvedBytes(uintptr_t address, void* destination, size_t size)
    {
        SIZE_T bytesRead = 0;
        return address != 0 && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address), destination, size, &bytesRead) != FALSE &&
            bytesRead == size;
    }

    bool ApplyRelativeDisplacement(uintptr_t next, int32_t displacement, uintptr_t& target)
    {
        if (displacement < 0)
        {
            const uintptr_t magnitude = static_cast<uintptr_t>(-static_cast<int64_t>(displacement));
            if (next < magnitude) return false;
            target = next - magnitude;
            return true;
        }

        const uintptr_t magnitude = static_cast<uintptr_t>(displacement);
        if (magnitude > UINTPTR_MAX - next) return false;
        target = next + magnitude;
        return true;
    }

    bool ValidateGuardedRelease(uintptr_t address, uintptr_t& exitPurge)
    {
        // Mirror MiniMap's narrow CL-127004 validation: confirm the wrapper
        // compares its purge byte, branches over release, and that the guarded
        // branch target is the observed strong-object release implementation.
        std::array<uint8_t, 13> wrapper{};
        if (!ReadResolvedBytes(address, wrapper.data(), wrapper.size()) ||
            wrapper[0] != 0x80 || wrapper[1] != 0x3d || wrapper[6] != 0x00 ||
            wrapper[7] != 0x0f || wrapper[8] != 0x84)
            return false;

        int32_t purgeDisplacement = 0;
        int32_t guardedBranchDisplacement = 0;
        std::memcpy(&purgeDisplacement, wrapper.data() + 2, sizeof(purgeDisplacement));
        std::memcpy(&guardedBranchDisplacement, wrapper.data() + 9, sizeof(guardedBranchDisplacement));

        const uintptr_t purgeNext = address + 7;
        const uintptr_t branchNext = address + wrapper.size();
        if (purgeNext < address || branchNext < address) return false;
        uintptr_t purgeByte = 0;
        uintptr_t guardedTarget = 0;
        if (!ApplyRelativeDisplacement(purgeNext, purgeDisplacement, purgeByte) ||
            !ApplyRelativeDisplacement(branchNext, guardedBranchDisplacement, guardedTarget) ||
            purgeByte == 0 || guardedTarget == 0)
            return false;
        uint8_t purgeValue = 0;
        if (!ReadResolvedBytes(purgeByte, &purgeValue, sizeof(purgeValue))) return false;

        constexpr std::array<uint8_t, 16> expectedReleaseEntry = {
            0x40,0x53,0x48,0x83,0xec,0x30,0x8b,0x59,
            0x0c,0x85,0xdb,0x79,0x26,0x4c,0x8d,0x0d
        };
        std::array<uint8_t, 16> actualReleaseEntry{};
        if (!ReadResolvedBytes(guardedTarget, actualReleaseEntry.data(), actualReleaseEntry.size()) ||
            actualReleaseEntry != expectedReleaseEntry)
            return false;

        exitPurge = purgeByte;
        return true;
    }

    bool ResolveRipGlobal(IPluginSelf* self, IPluginHookScanner* scanner,
        const char* name, const char* pattern, const std::array<uint8_t, 3>& prefix,
        uintptr_t& global)
    {
        const uintptr_t instruction = scanner->ResolveRequired(self, name, pattern);
        std::array<uint8_t, 7> bytes{};
        if (!instruction || !ReadResolvedBytes(instruction, bytes.data(), bytes.size()) ||
            bytes[0] != prefix[0] || bytes[1] != prefix[1] || bytes[2] != prefix[2])
            return false;

        int32_t displacement = 0;
        std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
        const uintptr_t next = instruction + bytes.size();
        uint8_t stateValue = 0;
        return next >= instruction && ApplyRelativeDisplacement(next, displacement, global) && global != 0 &&
            ReadResolvedBytes(global, &stateValue, sizeof(stateValue));
    }
}

namespace MainMapFingerprints
{
    bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner,
        ResolvedAddresses& addresses)
    {
        addresses = {};
        if (!self || !scanner || !scanner->ResolveRequired) return false;

        // CL-127004 signatures are adapted from MiniMap's independently
        // maintained bindings. The loader performs required main-image scans.
        // Keep only the A2.1 object-lifetime and admission bindings.
        addresses.retainObject = scanner->ResolveRequired(self,
            "MainMap::RetainObject",
            "40 53 48 83 EC 30 8B 59 0C 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 0D 04 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 3B D8 0F 8D ?? ?? ?? ?? 48 89 74 24 40 48 89 7C 24 48 8B FB C1 EF 10 0F B7 F3 3B D8 7D ?? 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 89 5C 24 20 41 B8 2C 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B F8 72 ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2D 03 00 00 89 7C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 8B 05 ?? ?? ?? ?? 3B D8 7C ?? 89 44 24 28 4C 8D 0D ?? ?? ?? ?? 41 B8 2E 03 00 00 89 5C 24 20 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 8B 1C F8 48 8B 7C 24 48 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 30 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8D 0C 76 48 8B 74 24 40 48 8D 1C CB 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 4B 03 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC EB ?? 33 DB F0 FF 43 14 8B 43 08 0F BA E0 1D 72 ?? BA 00 00 00 20 48 8B CB 48 83 C4 30 5B E9 ?? ?? ?? ?? 48 83 C4 30 5B C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 10 57 48 83 EC 20 48 8B 02 48 8B FA 48 8B D9 48 8D 51 78 45 33 C9 45 33 C0 48 8B CF FF 90 88 00 00 00 F6 43 10 01 74 ?? 48 8B 43 10 48 83 E0 FE EB ?? 33 C0 80 3D ?? ?? ?? ?? 00 48 89 44 24 30 74 ?? 48 85 C0 74 ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 44 24 30 48 85 C0 74 ?? 48 8B 07");
        if (!addresses.retainObject) return false;

        // The leading compare/conditional-return in this full guarded-wrapper
        // fingerprint is the observed GExitPurge admission check. Never bind
        // the direct ReleaseRef body contained later in the signature.
        addresses.releaseObject = scanner->ResolveRequired(self,
            "MainMap::ReleaseObject",
            "80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? C3 CC CC 40 53 55 56 57 41 54 41 56 41 57 48 83 EC 50 48 8B EA 4C 8B F9 48 8B D1 49 8B D8 48 8D 4C 24 30 E8 ?? ?? ?? ?? 4D 8B 67 08 49 8D 77 08 4D 63 77 10 49 8B FC 49 C1 E6 04 4D 03 F4 4D 3B E6 74 ?? 33 C0 88 84 24 90 00 00 00 48 39 5F 08 75 ?? 88 84 24 98 00 00 00 48 85 ED 74 ?? 48 8B CF 88 84 24 A0 00 00 00 E8 ?? ?? ?? ?? 48 3B E8 74 ?? 48 83 C7 10 49 3B FE 75 ?? 49 8D 77 08 41 B0 01 48 8D 94 24 A8 00 00 00 48 8B CE E8 ?? ?? ?? ?? 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 83 C4 50 41 5F 41 5E 41 5C 5F 5E 5D 5B C3 49 2B FC 49 8D 77 08 48 C1 FF 04 83 FF FF 74 ?? 48 63 56 08 48 8D 5E 08 33 C0 8B CF F7 D1 48 8B EE C1 E9 1F 3B FA 0F 4C C1 85 C0 75 ?? 48 89 54 24 28 4C 8D 0D ?? ?? ?? ?? 48 63 CF 48 8D 15 ?? ?? ?? ?? 48 89 4C 24 20 41 B8 2B 04 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 49 8D 6F 08 8B 0B BA 01 00 00 00 4C 8B 0E 8B C1 2B C7 FF C8 3B C2 0F 4F C2 85 C0 74 ?? 2B C8 4C 63 C0 48 63 D1 48 C1 E2 04 48 63 CF 49 03 D1 48 C1 E1 04 49 03 C9 49 C1 E0 04 E8 ?? ?? ?? ?? 48 8B F5 FF 0B E9 ?? ?? ?? ?? CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 48 89 5C 24 18 55 48 83 EC 20 8B 41 08 48 8B EA 48 8B D9 3B 41 34 0F 84 ?? ?? ?? ?? 4C 8B C2 48 89 74 24 30 49 C1 E8 04 41 8B C0 48 89 7C 24 38 C1 E8 10 33 FF 41 33 C0 4C 8B 43 40 69 D0 6B CA EB 85 8B C2 C1 E8 0D 33 C2 69 C8 35 AE B2 C2 48 63 C1 8B D1 48 C1 EA 10 48 33 D0 8B 43 48 FF C8 48 63 C8 48 8D 43 38 48 23 D1 4D 85 C0 4C 0F 44 C0 41 8B 04 90 49 8D 34 90 83 F8 FF 74 ?? 66 90 48 63 C8 48 8D 14 89 48 8B 0B 48 39 2C D1 4C 8D 04 D1");
        if (!addresses.releaseObject) return false;
        if (!ValidateGuardedRelease(addresses.releaseObject, addresses.exitPurge))
        {
            if (scanner->ReportFailure)
                scanner->ReportFailure(self, "MainMap::ReleaseObject",
                    "resolved entry did not validate as the GExitPurge-guarded strong-object release wrapper");
            addresses = {};
            return false;
        }

        if (!ResolveRipGlobal(self, scanner,
            "MainMap::ActiveGarbageCollection",
            "0F B6 35 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01 FF 15 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? 0F 57 C0 39 05 ?? ?? ?? ?? F2 48 0F 2A 44 24 50",
            {0x0f, 0xb6, 0x35}, addresses.activeGarbageCollection))
        {
            if (scanner->ReportFailure)
                scanner->ReportFailure(self, "MainMap::ActiveGarbageCollection",
                    "could not validate the active garbage-collection state byte");
            addresses = {};
            return false;
        }

        addresses.isGarbageCollectingAndLocking = scanner->ResolveRequired(self,
            "MainMap::IsGarbageCollectingAndLocking",
            "0F B6 05 ?? ?? ?? ?? 90 C3 CC CC CC CC CC CC CC 40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 48 85 DB 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 B2 00 00 00");
        addresses.isIncrementalPurgePending = scanner->ResolveRequired(self,
            "MainMap::IsIncrementalPurgePending",
            "0F B6 05 ?? ?? ?? ?? 90 84 C0 75 ?? 0F B6 05 ?? ?? ?? ?? 90 84 C0 75 ?? C3 B0 01 C3 CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC");
        addresses.isIncrementalReachabilityPending = scanner->ResolveRequired(self,
            "MainMap::IsIncrementalReachabilityPending",
            "0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 8B 05 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 7C ?? 80 3D ?? ?? ?? ?? 00 75 ?? 32 C0 C3 B0 01 C3");

        addresses.isGameThread = scanner->ResolveRequired(self,
            "MainMap::IsGameThread",
            "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 ?? 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 02 74 ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 81 3C 02 02 00 00 40 0F 94 C0 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 8B 04 02 C1 E8 05 24 01 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC CC CC 83 3D ?? ?? ?? ?? 00 BA D0 09 00 00 75 ?? 65 48 8B 04 25 58 00 00 00 8B 0D ?? ?? ?? ?? 48 8B 0C C8 8B 04 0A 83 F8 10 74 ?? 83 F8 02 74 ?? 3D 10 00 00 40 74 ?? 32 C0 C3 B0 01 C3 8B 05 ?? ?? ?? ?? 65 48 8B 0C 25 58 00 00 00 48 8B 0C C1 8B 04 11 83 E8 10 A9 EF FF FF BF 0F 94 C0 C3 CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 20 0F 94 C0 C3 48 83 EC 28 83 3D ?? ?? ?? ?? 00 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 0C C8 75 ?? 8B 04 0A 83 F8 02 74 ?? 83 F8 10 74 ?? E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28 C3 B0 01 48 83 C4 28 C3 83 3C 0A 10 0F 94 C0 48 83 C4 28 C3 CC CC CC CC CC CC CC CC CC CC CC CC CC 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA D0 09 00 00 48 8B 04 C8 83 3C 02 04 0F 94 C0 C3 48 8B 49 08 48 8B 01 48 FF A0 B8 00 00 00 CC CC 40 53 48 83 EC 20 48 8B 19 48 85 DB 74 ?? 48 83 BB 90 00 00 00 00 74 ?? FF 15 ?? ?? ?? ?? 48 8B 8B 90 00 00 00 3B 41 34 74 ?? B0 01 48 83 C4 20 5B C3 32 C0 48 83 C4 20 5B C3 CC CC CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC");

        // Both complete native override entries were correlated with the current
        // CL-127004 class constructor/vtable and PE exception-directory entries.
        // Resolve enforces one match and FUNCTION_START; no mid-function fallback.
        if (!scanner->Resolve) return false;
        PluginScanRequest terrain = PLUGIN_SCAN_REQUEST_INIT;
        terrain.kind = PLUGIN_SCAN_FUNCTION_START;
        terrain.hookName = "MainMap::UCrUW_MapMenuTerrain::NativeConstruct";
        terrain.pattern =
            "40 56 48 83 EC 30 48 8B F1 E8 ?? ?? ?? ?? "
            "48 8B 06 48 8B CE FF 90 80 01 00 00 48 8B C8 E8 ?? ?? ?? ?? "
            "48 85 C0 0F 84 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
            "80 BE 98 04 00 00 00 0F 85 ?? ?? ?? ??";
        addresses.terrainConstruct = scanner->Resolve(self, &terrain);
        terrain.hookName = "MainMap::UCrUW_MapMenuTerrain::NativeDestruct";
        terrain.pattern =
            "40 56 48 83 EC 30 48 8B F1 E8 ?? ?? ?? ?? "
            "80 BE 98 04 00 00 00 0F 84 ?? ?? ?? ?? "
            "48 89 5C 24 40 33 DB 48 89 7C 24 48 39 9E 90 04 00 00 "
            "0F 8E ?? ?? ?? ?? 4C 89 74 24 50 45 33 F6 0F 1F 44 00 00";
        addresses.terrainDestruct = scanner->Resolve(self, &terrain);

        // CL-130045 entries were verified against PDB symbols and primary vtable
        // slots; relative call displacements are masked in these unique signatures.
        PluginScanRequest mapMenu = PLUGIN_SCAN_REQUEST_INIT;
        mapMenu.kind = PLUGIN_SCAN_FUNCTION_START;
        mapMenu.hookName = "MainMap::UCrUW_MapMenu::NativeTick";
        mapMenu.pattern =
            "48 89 5C 24 08 57 48 83 EC 30 F3 0F 10 89 28 06 00 00 "
            "0F 57 C0 0F 2E C8 48 8B FA 48 8B D9 0F 29 74 24 20 0F 28 F2 "
            "74 12 45 33 C0 E8 ?? ?? ?? ?? C7 83 28 06 00 00 00 00 00 00";
        addresses.mapMenuNativeTick = scanner->Resolve(self, &mapMenu);
        mapMenu.hookName = "MainMap::UCrUW_MapMenu::NativeOnDeactivated";
        mapMenu.pattern =
            "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B D9 FF 90 80 01 00 00 "
            "48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 2A E8 ?? ?? ?? ?? "
            "48 8B 4F 10 48 83 C0 30 48 63 50 08 3B 51 38 7F 14 48 8B 49 30 "
            "48 39 04 D1 75 0A 33 D2 48 8B CF E8 ?? ?? ?? ??";
        addresses.mapMenuNativeOnDeactivated = scanner->Resolve(self, &mapMenu);

        return addresses.terrainConstruct != 0 && addresses.terrainDestruct != 0 &&
            addresses.mapMenuNativeTick != 0 && addresses.mapMenuNativeOnDeactivated != 0 &&
            addresses.isGameThread != 0 && addresses.activeGarbageCollection != 0 &&
            addresses.exitPurge != 0 && addresses.isGarbageCollectingAndLocking != 0 &&
            addresses.isIncrementalPurgePending != 0 &&
            addresses.isIncrementalReachabilityPending != 0;
    }
}
