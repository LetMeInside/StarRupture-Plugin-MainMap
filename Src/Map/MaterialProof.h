#pragma once
#include "../Native/NativeApi.h"
#include "Engine_classes.hpp"
namespace SDK { class UWorld; class UMaterial; }
namespace MainMapProof
{
    SDK::UMaterial* LoadMaterial(const char* path, SDK::EMaterialDomain expectedDomain,
        MainMapNative::StrongObjectLease& lease);
}
