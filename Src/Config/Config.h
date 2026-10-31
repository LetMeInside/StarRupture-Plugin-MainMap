#pragma once
#include "plugin.h"

namespace MainMapConfig
{
    constexpr bool DefaultEnabled = true;
    bool Initialize(IPluginSelf* self);
    bool IsEnabled();
}