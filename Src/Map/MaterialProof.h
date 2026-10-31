#pragma once
#include "BuildingTarget.h"
namespace SDK { class UWorld; class UMaterial; }
namespace MainMapProof
{
    struct PresentationFrame final
    {
        MainMapNative::StrongObjectLease composeMidLease,presentMidLease,solidMidLease,textureLease;
        SDK::UMaterialInstanceDynamic* composeMid=nullptr;
        SDK::UMaterialInstanceDynamic* presentMid=nullptr;
        SDK::UMaterialInstanceDynamic* solidMid=nullptr;
        BuildingTarget target;
        bool submitted=false;
    };
    bool ValidateAndMeasure(SDK::UWorld* world, SDK::UMaterial* compose, SDK::UMaterial* present);
    bool BuildPresentationFrame(SDK::UWorld* world, SDK::UMaterial* compose, SDK::UMaterial* present,
        PresentationFrame& frame, bool alternate, bool rejectAfterBegin = false);
}
