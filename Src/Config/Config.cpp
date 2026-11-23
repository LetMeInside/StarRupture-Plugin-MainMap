/**
 * Registers MainMap's map settings with ModLoader and keeps an owned,
 * synchronized snapshot for future game-thread presentation consumers.
 */
#include "Config.h"
#include "plugin_helpers.h"

#include <cstring>
#include <mutex>
#include <string_view>

namespace
{
    constexpr ConfigEntry kEntries[] =
    {
        {
            "Map",
            "ExpandedMap",
            ConfigValueType::Boolean,
            "true",
            "Enable the enlarged native map layout; false preserves the game's original layout."
        },
        {
            "Map",
            "HideFoW",
            ConfigValueType::Boolean,
            "false",
            "Hide Fog of War presentation when enabled; exploration data remains unchanged."
        },
        {
            "Buildings",
            "ShowLayer",
            ConfigValueType::Boolean,
            "true",
            "Show or hide MainMap's building overlay."
        }
    };

    constexpr ConfigSchema kSchema =
    {
        kEntries,
        static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]))
    };

    std::mutex g_settingsMutex;
    Config::Settings g_settings{};
    bool g_acceptChanges = false;
    bool g_callbackRegistered = false;

    /** Validates the notified section, key, and text, then updates settings only. */
    void OnConfigChanged(const char* section, const char* key, const char* newValue)
    {
        if (section == nullptr || key == nullptr || newValue == nullptr)
            return;

        const bool mapSection = std::strcmp(section, "Map") == 0;
        const bool buildingsSection = std::strcmp(section, "Buildings") == 0;
        if (!mapSection && !buildingsSection)
            return;

        const bool expandedMapSetting = mapSection && std::strcmp(key, "ExpandedMap") == 0;
        const bool hideFogSetting = mapSection && std::strcmp(key, "HideFoW") == 0;
        const bool showBuildingLayerSetting = buildingsSection && std::strcmp(key, "ShowLayer") == 0;
        if (!expandedMapSetting && !hideFogSetting && !showBuildingLayerSetting)
            return;

        const std::string_view value(newValue);
        if (value != "true" && value != "false")
            return;
        const bool parsedValue = value == "true";

        std::lock_guard lock(g_settingsMutex);
        if (!g_acceptChanges)
            return;

        if (expandedMapSetting)
            g_settings.ExpandedMap = parsedValue;
        else if (hideFogSetting)
            g_settings.HideFoW = parsedValue;
        else if (showBuildingLayerSetting)
            g_settings.ShowBuildingLayer = parsedValue;
    }
}

namespace Config
{
    void Initialize()
    {
        Shutdown();
        {
            std::lock_guard lock(g_settingsMutex);
            g_settings = {};
        }
        IPluginSelf* self = GetSelf();
        IPluginConfig* config = self != nullptr ? self->config : nullptr;
        if (config == nullptr || config->InitializeFromSchema == nullptr || config->ReadString == nullptr ||
            !config->InitializeFromSchema(self, &kSchema))
        {
            LOG_WARN("MainMap: configuration API unavailable or registration failed; using built-in settings");
            return;
        }
        if (config->ValidateConfig != nullptr)
            config->ValidateConfig(self, &kSchema);
        char expandedMapText[16]{};
        char hideFoWText[16]{};
        char showBuildingLayerText[16]{};
        const bool expandedMapRead = config->ReadString(self, "Map", "ExpandedMap", expandedMapText, sizeof(expandedMapText), "true");
        const bool hideFoWRead = config->ReadString(self, "Map", "HideFoW", hideFoWText, sizeof(hideFoWText), "false");
        const bool showBuildingLayerRead = config->ReadString(self, "Buildings", "ShowLayer", showBuildingLayerText, sizeof(showBuildingLayerText), "true");
        const bool valuesRead = expandedMapRead && hideFoWRead && showBuildingLayerRead &&
            ((std::strcmp(expandedMapText, "true") == 0) || (std::strcmp(expandedMapText, "false") == 0)) &&
            ((std::strcmp(hideFoWText, "true") == 0) || (std::strcmp(hideFoWText, "false") == 0)) &&
            ((std::strcmp(showBuildingLayerText, "true") == 0) || (std::strcmp(showBuildingLayerText, "false") == 0));
        if (!valuesRead)
            LOG_WARN("MainMap: configuration read or validation failed; using built-in settings");
        else
        {
            std::lock_guard lock(g_settingsMutex);
            g_settings.ExpandedMap = std::strcmp(expandedMapText, "true") == 0;
            g_settings.HideFoW = std::strcmp(hideFoWText, "true") == 0;
            g_settings.ShowBuildingLayer = std::strcmp(showBuildingLayerText, "true") == 0;
        }
        if (self->hooks == nullptr || self->hooks->UI == nullptr ||
            self->hooks->UI->RegisterOnConfigChanged == nullptr ||
            self->hooks->UI->UnregisterOnConfigChanged == nullptr)
        {
            LOG_WARN("MainMap: live configuration notifications unavailable; current settings remain active");
            return;
        }
        {
            std::lock_guard lock(g_settingsMutex);
            g_acceptChanges = true;
            g_callbackRegistered = true;
        }
        self->hooks->UI->RegisterOnConfigChanged(self, OnConfigChanged);
    }

    void Shutdown()
    {
        IPluginSelf* self = GetSelf();
        bool unregisterCallback = false;
        {
            std::lock_guard lock(g_settingsMutex);
            g_acceptChanges = false;
            unregisterCallback = g_callbackRegistered;
            g_callbackRegistered = false;
        }

        if (unregisterCallback && self != nullptr && self->hooks != nullptr &&
            self->hooks->UI != nullptr && self->hooks->UI->UnregisterOnConfigChanged != nullptr)
        {
            self->hooks->UI->UnregisterOnConfigChanged(self, OnConfigChanged);
        }
    }

    Settings Get()
    {
        std::lock_guard lock(g_settingsMutex);
        return g_settings;
    }
}
