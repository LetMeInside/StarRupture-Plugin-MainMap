/**
 * MainMap settings declared to and read from the ModLoader configuration API.
 * Values are returned as owned snapshots so consumers can safely read them
 * without retaining shared mutable storage.
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

    /** Returns a synchronized copy that remains valid after the call returns. */
    Settings Get();
}
