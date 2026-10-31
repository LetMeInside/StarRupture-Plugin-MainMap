#include "Config.h"
#include "plugin_helpers.h"

namespace
{
    static const ConfigEntry CONFIG_ENTRIES[] =
    {
        {
            "General",
            "Enabled",
            ConfigValueType::Boolean,
            "true",
            "Enable or disable MainMap"
        }
    };

    static const ConfigSchema SCHEMA =
    {
        CONFIG_ENTRIES,
        sizeof(CONFIG_ENTRIES) / sizeof(CONFIG_ENTRIES[0])
    };

    bool g_enabled = MainMapConfig::DefaultEnabled;
}

namespace MainMapConfig
{
    bool Initialize(IPluginSelf* self)
    {
        if (self == nullptr || self->config == nullptr)
        {
            LOG_ERROR("MainMap: configuration interface is unavailable");
            return false;
        }

        if (!self->config->InitializeFromSchema(self, &SCHEMA))
        {
            LOG_ERROR("MainMap: config schema initialization failed");
            return false;
        }

        self->config->ValidateConfig(self, &SCHEMA);
        g_enabled = self->config->ReadBool(self, "General", "Enabled", DefaultEnabled);
        return true;
    }

    bool IsEnabled()
    {
        return g_enabled;
    }
}
