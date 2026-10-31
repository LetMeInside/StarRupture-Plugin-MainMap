#pragma once
#include "BuildingTarget.h"
namespace SDK { class UWorld; class UMaterial; }
namespace MainMapProof
{
    struct PresentationFrame final
    {
        MainMapNative::StrongObjectLease composeMidLease,presentMidLease;
        SDK::UMaterialInstanceDynamic* composeMid=nullptr;
        SDK::UMaterialInstanceDynamic* presentMid=nullptr;
        BuildingTarget target;
        bool Reset();
    };
    SDK::UMaterial* LoadMaterial(const char* path, SDK::EMaterialDomain expectedDomain,
        MainMapNative::StrongObjectLease& lease);
    bool PreparePresentationFrame(SDK::UWorld* world, SDK::UMaterial* compose, SDK::UMaterial* present,
        PresentationFrame& frame);
}
