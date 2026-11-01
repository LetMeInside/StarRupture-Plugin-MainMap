#pragma once

namespace MainMapBuildingComplex
{
    // Startup-loaded MiniMap only. Runtime load/unload/reload is unsupported;
    // the current ModLoader does not enforce the future RESTART policy.
    void Initialize();
    void Tick();
    // Notification-only; safe when native/GT admission is rejected.
    void RequestClose();
    void ServiceClose();
    // Only the early engine-shutdown callback permits a final provider call.
    // Late PluginShutdown must not call a possibly torn-down provider.
    void Shutdown(bool earlyEngineShutdown);
}
