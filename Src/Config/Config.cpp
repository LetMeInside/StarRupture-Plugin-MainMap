/**
 * Registers MainMap's map settings with ModLoader and publishes startup
 * values and live edits through atomic preferences for game-thread consumers.
 */
#include "Config.h"
#include "plugin_helpers.h"

#include <atomic>
#include <cctype>
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

    std::mutex g_callbackStateMutex;
    std::atomic<bool> g_expandedMap{true};
    std::atomic<bool> g_hideFoW{false};
    std::atomic<bool> g_showBuildingLayer{true};
    bool g_acceptChanges = false;
    bool g_callbackRegistered = false;

    /** Parses the Boolean spellings accepted from both schema reads and live notifications. */
    class BooleanConfigParser final
    {
    public:
        /** Compares equal-length Boolean spellings without depending on letter case. */
        static bool EqualsCaseInsensitive(std::string_view text, std::string_view expected)
        {
            if (text.size() != expected.size())
                return false;

            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const auto textCharacter = static_cast<unsigned char>(text[index]);
                const auto expectedCharacter = static_cast<unsigned char>(expected[index]);
                if (std::tolower(textCharacter) != std::tolower(expectedCharacter))
                    return false;
            }

            return true;
        }

        /** Converts SDK-supported Boolean spellings, returning false for invalid text. */
        static bool Parse(std::string_view text, bool& value)
        {
            if (text == "1" || EqualsCaseInsensitive(text, "true") ||
                EqualsCaseInsensitive(text, "yes"))
            {
                value = true;
                return true;
            }
            if (text == "0" || EqualsCaseInsensitive(text, "false") ||
                EqualsCaseInsensitive(text, "no"))
            {
                value = false;
                return true;
            }
            return false;
        }

        /** Reads each schema value, retaining its default only when that value is invalid. */
        static bool ReadSettings(IPluginSelf* self, IPluginConfig* config, Config::Settings& settings)
        {
            char expandedMapText[16]{};
            char hideFoWText[16]{};
            char showBuildingLayerText[16]{};
            const bool expandedMapRead = config->ReadString(self, "Map", "ExpandedMap",
                expandedMapText, sizeof(expandedMapText), "true");
            const bool hideFoWRead = config->ReadString(self, "Map", "HideFoW",
                hideFoWText, sizeof(hideFoWText), "false");
            const bool showBuildingLayerRead = config->ReadString(self, "Buildings", "ShowLayer",
                showBuildingLayerText, sizeof(showBuildingLayerText), "true");
            bool allSettingsRead = true;
            bool parsedValue = false;
            if (expandedMapRead && Parse(expandedMapText, parsedValue))
                settings.ExpandedMap = parsedValue;
            else
                allSettingsRead = false;
            if (hideFoWRead && Parse(hideFoWText, parsedValue))
                settings.HideFoW = parsedValue;
            else
                allSettingsRead = false;
            if (showBuildingLayerRead && Parse(showBuildingLayerText, parsedValue))
                settings.ShowBuildingLayer = parsedValue;
            else
                allSettingsRead = false;
            return allSettingsRead;
        }
    };

    /** Validates a live notification and publishes its value to the matching atomic preference. */
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
        bool parsedValue = false;
        if (!BooleanConfigParser::Parse(value, parsedValue))
            return;

        std::lock_guard lock(g_callbackStateMutex);
        if (!g_acceptChanges)
            return;

        if (expandedMapSetting)
            g_expandedMap.store(parsedValue, std::memory_order_relaxed);
        else if (hideFogSetting)
            g_hideFoW.store(parsedValue, std::memory_order_relaxed);
        else if (showBuildingLayerSetting)
            g_showBuildingLayer.store(parsedValue, std::memory_order_relaxed);

        LOG_INFO("MainMap: configuration change received section=%s key=%s value=%s",
            section, key, parsedValue ? "true" : "false");
    }
}

namespace Config
{
    void Initialize()
    {
        Shutdown();
        const Config::Settings defaultSettings{};
        g_expandedMap.store(defaultSettings.ExpandedMap, std::memory_order_relaxed);
        g_hideFoW.store(defaultSettings.HideFoW, std::memory_order_relaxed);
        g_showBuildingLayer.store(defaultSettings.ShowBuildingLayer, std::memory_order_relaxed);
        IPluginSelf* self = GetSelf();
        IPluginConfig* config = self != nullptr ? self->config : nullptr;
        if (config == nullptr || config->InitializeFromSchema == nullptr || config->ReadString == nullptr ||
            !config->InitializeFromSchema(self, &kSchema))
        {
            LOG_WARN("MainMap: configuration API unavailable or registration failed; using built-in settings");
            LOG_INFO("MainMap: initial configuration values source=built-in defaults ExpandedMap=%d HideFoW=%d ShowBuildingLayer=%d",
                defaultSettings.ExpandedMap, defaultSettings.HideFoW, defaultSettings.ShowBuildingLayer);
            return;
        }
        if (config->ValidateConfig != nullptr)
            config->ValidateConfig(self, &kSchema);
        Config::Settings loadedSettings{};
        const bool allSettingsRead = BooleanConfigParser::ReadSettings(self, config, loadedSettings);
        if (!allSettingsRead)
            LOG_WARN("MainMap: one or more configuration values were invalid; defaults retained only for those keys");
        g_expandedMap.store(loadedSettings.ExpandedMap, std::memory_order_relaxed);
        g_hideFoW.store(loadedSettings.HideFoW, std::memory_order_relaxed);
        g_showBuildingLayer.store(loadedSettings.ShowBuildingLayer, std::memory_order_relaxed);
        LOG_INFO("MainMap: initial configuration values source=%s ExpandedMap=%d HideFoW=%d ShowBuildingLayer=%d",
            allSettingsRead ? "saved-config" : "partial-config-with-defaults",
            loadedSettings.ExpandedMap, loadedSettings.HideFoW, loadedSettings.ShowBuildingLayer);
        if (self->hooks == nullptr || self->hooks->UI == nullptr ||
            self->hooks->UI->RegisterOnConfigChanged == nullptr ||
            self->hooks->UI->UnregisterOnConfigChanged == nullptr)
        {
            LOG_WARN("MainMap: live configuration notifications unavailable; current settings remain active");
            return;
        }
        {
            std::lock_guard lock(g_callbackStateMutex);
            g_acceptChanges = true;
            g_callbackRegistered = true;
        }
        self->hooks->UI->RegisterOnConfigChanged(self, OnConfigChanged);
        LOG_INFO("MainMap: configuration-change callback registered; saved values were read separately");
    }

    void Shutdown()
    {
        IPluginSelf* self = GetSelf();
        bool unregisterCallback = false;
        {
            std::lock_guard lock(g_callbackStateMutex);
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

    bool IsExpandedMapEnabled() noexcept
    {
        return g_expandedMap.load(std::memory_order_relaxed);
    }

    bool IsHideFoWEnabled() noexcept
    {
        return g_hideFoW.load(std::memory_order_relaxed);
    }

    bool IsBuildingLayerEnabled() noexcept
    {
        return g_showBuildingLayer.load(std::memory_order_relaxed);
    }
}
