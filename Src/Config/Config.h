/**
 * MainMap settings declared to and read from the ModLoader configuration API.
 * Startup values and live edits are published as atomic Boolean preferences
 * for game-thread consumers that run on every tick.
 */
#pragma once

namespace Config
{
    /**
     * Owns MainMap's user-facing presentation choices. Defaults preserve the
     * requested expanded-map, native-fog, and building-overlay settings until
     * valid values load.
     */
    struct Settings
    {
        bool ExpandedMap = true;
        bool HideFoW = false;
        bool ShowBuildingLayer = true;
    };

    /** Publishes, reads, and observes settings; failures leave built-in defaults active. */
    void Initialize();

    /** Stops accepting edits and unregisters the ModLoader change callback. */
    void Shutdown();

    /** Reads ExpandedMap without acquiring the configuration lifecycle mutex. */
    bool IsExpandedMapEnabled() noexcept;

    /** Reads HideFoW without acquiring the configuration lifecycle mutex. */
    bool IsHideFoWEnabled() noexcept;

    /** Reads ShowBuildingLayer without acquiring the configuration lifecycle mutex. */
    bool IsBuildingLayerEnabled() noexcept;
}
