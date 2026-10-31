#include "RenderingProof.h"
#include "Native/NativeApi.h"
#include "plugin.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#include "MaterialProof.h"

namespace
{
    bool g_registered = false;
    bool g_attempted = false;
    PluginPakHandle g_pakHandle = 0;
    void ObserveTerrainConstruct(SDK::UObject*) {}
    void ObserveTerrainDestruct(SDK::UObject*) {}
    void InspectExistingTerrain(const char* boundary)
    {
        if(!MainMapNative::CanOperate())
        {
            LOG_WARN("MainMap A2.3: terrain lookup at %s rejected by admission",boundary);
            return;
        }
        auto* walker=g_self->hooks->ObjectWalker;
        if(!walker->IsReady()) return;
        for(const char* cls : {"WBP_MapMenuTerrain_C","CrUW_MapMenuTerrain","WBP_MapMenuMapArea_C","WBP_MapMenu_C"})
        {
            PluginObjectInfo objects[4]{};
            const int total=walker->FindObjectsByClassNameInto(cls,PluginObjectLookup_InstanceOnly,objects,4);
            LOG_INFO("MainMap A2.3: terrain lookup boundary=%s class=%s instances=%d",boundary,cls,total);
            for(int i=0;i<total && i<4;++i)
                LOG_INFO("MainMap A2.3: terrain lookup object=%p index=%d name=%s",objects[i].object,
                    objects[i].objectIndex,objects[i].objectName);
        }
    }
    void BeforeWorldEnd(SDK::UWorld*,const char*) { InspectExistingTerrain("before-world-end"); }

    SDK::UMaterial* InspectMaterial(const char* path, SDK::EMaterialDomain expectedDomain,
        MainMapNative::StrongObjectLease& lease)
    {
        auto* object = static_cast<SDK::UObject*>(g_self->hooks->Pak->LoadObject(path));
        if (!object)
        {
            LOG_ERROR("MainMap A2.3: exact material load FAILED: %s", path);
            return nullptr;
        }
        if (!lease.Acquire(object))
        {
            LOG_ERROR("MainMap A2.3: loaded object could not be retained: %s", path);
            return nullptr;
        }
        // SDK GetFullName/GetName use FName::ToString, which strips slash prefixes.
        // Raw package FName preserves the exact long package path.
        const std::string loadedPath = object->Outer ?
            object->Outer->Name.GetRawString() + "." + object->Name.GetRawString() : "";
        if (!object->IsA(SDK::UMaterial::StaticClass()) || loadedPath != path)
        {
            LOG_ERROR("MainMap A2.3: wrong class/object identity: %s rawPath=%s",
                object->GetFullName().c_str(), loadedPath.c_str());
            return nullptr;
        }
        else
        {
            auto* material = static_cast<SDK::UMaterial*>(object);
            const bool accepted = material->MaterialDomain == expectedDomain &&
                material->BlendMode == SDK::EBlendMode::BLEND_AlphaComposite;
            LOG_INFO("MainMap A2.3: exact object=%s domain=%u blend=%u domain/blend=%s", path,
                static_cast<unsigned>(material->MaterialDomain), static_cast<unsigned>(material->BlendMode),
                accepted ? "PASS" : "FAIL");
            if (!accepted) return nullptr;
        }
        return static_cast<SDK::UMaterial*>(object);
    }

    void OnTick(float)
    {
        if (g_attempted || !MainMapNative::CanOperate()) return;
        g_attempted = true; // No retry after timeout or uncertain engine acceptance.
        InspectExistingTerrain("initial-admitted-tick");
        auto* pak = g_self->hooks->Pak;
        if (!pak || !pak->IsAvailable || !pak->IsAvailable() || !pak->MountResource ||
            !pak->LoadObject || !pak->ResultToString)
        {
            LOG_ERROR("MainMap A2.3: pak API unavailable; compatibility gate CLOSED");
            return;
        }
        HMODULE module = nullptr;
        // Resolve the DLL containing this function without acquiring another module reference.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&OnTick), &module))
        {
            LOG_ERROR("MainMap A2.3: actual MainMap module lookup failed");
            return;
        }
        PluginPakMountOptions options{};
        options.order = PLUGIN_PAK_ORDER_DEFAULT;
        const auto result = pak->MountResource(g_self, module, "30101", "30102", "30103",
            "MainMapMaterials", &options, &g_pakHandle);
        LOG_INFO("MainMap A2.3: MountResource result=%s (%d) handle=%p module=%p",
            pak->ResultToString(result), static_cast<int>(result),
            g_pakHandle, module);
        if (result == PLUGIN_PAK_ALREADY_MOUNTED)
        {
            LOG_ERROR("MainMap A2.3: ALREADY_MOUNTED does not prove freshness; full restart required; gate CLOSED");
            return;
        }
        if (result != PLUGIN_PAK_OK || !g_pakHandle) return;
        MainMapNative::StrongObjectLease composeLease,presentLease;
        auto* compose=InspectMaterial("/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose",
            SDK::EMaterialDomain::MD_Surface,composeLease);
        if (!MainMapNative::CanOperate()) return;
        auto* present=InspectMaterial("/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
            SDK::EMaterialDomain::MD_UI,presentLease);
        if(compose && present)
            MainMapProof::ValidateAndMeasure(SDK::UWorld::GetWorld(),compose,present);
        LOG_WARN("MainMap A2.3: diagnostic operation finished; actual Slate/presentation/lifetime remain UNVERIFIED");
    }
}

namespace MainMapRenderingProof
{
    bool Initialize()
    {
        auto* engine = g_self->hooks->Engine;
        auto* world=g_self->hooks->World;
        auto* walker=g_self->hooks->ObjectWalker;
        if (!engine || !engine->RegisterOnTick || !engine->UnregisterOnTick || !world ||
            !world->RegisterOnBeforeWorldEndPlay || !world->UnregisterOnBeforeWorldEndPlay ||
            !walker || !walker->IsReady || !walker->FindObjectsByClassNameInto) return false;
        if(!MainMapNative::InstallTerrainHooks(g_self,ObserveTerrainConstruct,ObserveTerrainDestruct)) return false;
        LOG_INFO("MainMap A2.3: early terrain lifecycle observation ready; no persistent rendering owner attached");
        world->RegisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        g_attempted = false;
        g_pakHandle = 0;
        engine->RegisterOnTick(OnTick);
        g_registered = true;
        return true;
    }

    void Shutdown()
    {
        MainMapNative::RemoveTerrainHooks();
        LOG_INFO("MainMap A2.3: proof cleanup leases=%zu",MainMapNative::OutstandingLeaseCount());
        if (g_registered && g_self && g_self->hooks && g_self->hooks->Engine)
        {
            g_self->hooks->Engine->UnregisterOnTick(OnTick);
            g_self->hooks->World->UnregisterOnBeforeWorldEndPlay(BeforeWorldEnd);
        }
        g_registered = false;
        // Registry owns mounted content for process lifetime. Never unmount to evade restart.
    }
}
