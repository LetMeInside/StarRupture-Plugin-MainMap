#include "MaterialProof.h"
#include "plugin_helpers.h"
#include "plugin.h"
#include <string>

namespace MainMapProof
{
    bool PresentationFrame::Reset()
    {
        // Withdraw the image/brush first. Keep dependencies until the MIDs
        // relinquish their references. Every failed reset remains retryable.
        if (!presentMidLease.Reset()) return false;
        presentMid=nullptr;
        if (!composeMidLease.Reset()) return false;
        composeMid=nullptr;
        return target.Reset();
    }

    SDK::UMaterial* LoadMaterial(const char* path, SDK::EMaterialDomain expectedDomain,
        MainMapNative::StrongObjectLease& lease)
    {
        if (!MainMapNative::CanOperate() || !g_self || !g_self->hooks ||
            !g_self->hooks->Pak || !g_self->hooks->Pak->LoadObject) return nullptr;
        auto* object=static_cast<SDK::UObject*>(g_self->hooks->Pak->LoadObject(path));
        if (!object || !lease.Acquire(object))
        {
            LOG_ERROR("MainMap A2.3: material load/retention FAILED: %s",path);
            return nullptr;
        }
        const std::string loadedPath=object->Outer ?
            object->Outer->Name.GetRawString()+"."+object->Name.GetRawString() : "";
        if (!object->IsA(SDK::UMaterial::StaticClass()) || loadedPath!=path)
        {
            LOG_ERROR("MainMap A2.3: wrong material identity path=%s actual=%s",path,loadedPath.c_str());
            return nullptr;
        }
        auto* material=static_cast<SDK::UMaterial*>(object);
        const bool accepted=material->MaterialDomain==expectedDomain &&
            material->BlendMode==SDK::EBlendMode::BLEND_AlphaComposite;
        if (!accepted) LOG_ERROR("MainMap: required material has incompatible domain/blend: %s",path);
        return accepted?material:nullptr;
    }

    static bool PrepareFrame(SDK::UWorld* world,SDK::UMaterial* compose,SDK::UMaterial* present,
        PresentationFrame& frame)
    {
        if(!world || !compose || !present || !MainMapNative::CanOperate()) return false;
        auto*& composeMid=frame.composeMid;
        auto*& presentMid=frame.presentMid;
        auto* mid=SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(world,compose,{},SDK::EMIDCreationFlags::None);
        if(!mid || !frame.composeMidLease.Acquire(mid)) return false;
        composeMid=mid;
        mid=SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(world,present,{},SDK::EMIDCreationFlags::None);
        if(!mid || !frame.presentMidLease.Acquire(mid)) return false;
        presentMid=mid;
        const auto spriteName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"SpriteAtlas"));
        const auto targetName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"BuildingTarget"));
        auto* spriteDefault=composeMid->K2_GetTextureParameterValue(spriteName);
        auto* targetDefault=presentMid->K2_GetTextureParameterValue(targetName);
        const bool parameters=spriteDefault && targetDefault && spriteDefault->IsA(SDK::UTexture2D::StaticClass()) &&
            targetDefault->IsA(SDK::UTexture2D::StaticClass()) && spriteDefault->SRGB && !targetDefault->SRGB;
        if(!parameters)
        {
            LOG_ERROR("MainMap: required material texture parameters are incompatible");
            return false;
        }
        auto& target=frame.target;
        if(!target.Create(world)) return false;
        presentMid->SetTextureParameterValue(targetName,target.Get());
        if(presentMid->K2_GetTextureParameterValue(targetName)!=target.Get()) return false;
        return true;
    }

    bool PreparePresentationFrame(SDK::UWorld* world,SDK::UMaterial* compose,SDK::UMaterial* present,
        PresentationFrame& frame)
    {
        return PrepareFrame(world,compose,present,frame);
    }
}
