#include "MaterialProof.h"
#include "plugin_helpers.h"
#include "plugin.h"
#include "Engine_classes.hpp"
#include <string>

namespace MainMapProof
{
    SDK::UMaterial* LoadMaterial(const char* path, SDK::EMaterialDomain expectedDomain,
        MainMapNative::StrongObjectLease& lease)
    {
        if (!MainMapNative::CanOperate() || !g_self || !g_self->hooks ||
            !g_self->hooks->Pak || !g_self->hooks->Pak->LoadObject)
        {
            LOG_INFO("MainMap diagnostics: material load not attempted path=%s reason=operation-or-pak-admission-unavailable",path);
            return nullptr;
        }
        auto* object=static_cast<SDK::UObject*>(g_self->hooks->Pak->LoadObject(path));
        if(!object)
        {
            LOG_ERROR("MainMap diagnostics: material load failed path=%s result=null-object",path);
            return nullptr;
        }
        if(!lease.Acquire(object))
        {
            LOG_ERROR("MainMap diagnostics: material retention failed path=%s object=%p",path,object);
            return nullptr;
        }
        const std::string loadedPath=object->Outer ?
            object->Outer->Name.GetRawString()+"."+object->Name.GetRawString() : "";
        if (!object->IsA(SDK::UMaterial::StaticClass()) || loadedPath!=path)
        {
            LOG_ERROR("MainMap diagnostics: material identity validation failed path=%s actual=%s isUMaterial=%u",
                path,loadedPath.c_str(),unsigned(object->IsA(SDK::UMaterial::StaticClass())));
            return nullptr;
        }
        auto* material=static_cast<SDK::UMaterial*>(object);
        const bool accepted=material->MaterialDomain==expectedDomain &&
            material->BlendMode==SDK::EBlendMode::BLEND_AlphaComposite;
        if (!accepted) LOG_ERROR("MainMap diagnostics: material validation failed path=%s domain=%u expectedDomain=%u blend=%u expectedBlend=%u",
            path,static_cast<unsigned>(material->MaterialDomain),static_cast<unsigned>(expectedDomain),
            static_cast<unsigned>(material->BlendMode),static_cast<unsigned>(SDK::EBlendMode::BLEND_AlphaComposite));
        else LOG_INFO("MainMap diagnostics: material validated path=%s object=%s domain=%u blend=%u retained=1",
            path,loadedPath.c_str(),static_cast<unsigned>(material->MaterialDomain),
            static_cast<unsigned>(material->BlendMode));
        return accepted?material:nullptr;
    }

}
