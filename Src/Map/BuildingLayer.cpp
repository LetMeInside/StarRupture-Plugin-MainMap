#include "BuildingLayer.h"
#include "MaterialProof.h"
#include "ProofReflection.h"
#include "ChimeraUI_classes.hpp"
#include "UMG_classes.hpp"
#include "plugin_helpers.h"

#include <atomic>
#include <cmath>
#include <new>

static_assert(sizeof(SDK::FGeometry)==0x38);
static_assert(sizeof(SDK::FAnchors)==0x20);
static_assert(sizeof(SDK::FMargin)==0x10);

namespace
{
    struct Session final
    {
        unsigned long long id=0;
        MainMapNative::StrongObjectLease terrain,world,canvas,image;
        MainMapNative::StrongObjectLease composeParent,presentParent;
        MainMapProof::PresentationFrame frame;
        bool attached=false;
        bool brushBound=false;
        bool published=false;
        bool closing=false;
        bool waitingGeometryLogged=false;
        bool deferredLogged=false;
    };

    // One owner, no replacement queue. Deleted only after every lease resets.
    // If final teardown rejects release, leave the bookkeeping intact and log
    // it rather than running lease destructors after native admission closes.
    Session* g_session=nullptr;
    unsigned long long g_nextSession=0;
    std::atomic_bool g_cleanupRequested{false};
    std::atomic_bool g_stopping{false};

    bool IsInstance(SDK::UObject* object)
    {
        const auto forbidden=SDK::EObjectFlags::ClassDefaultObject | SDK::EObjectFlags::ArchetypeObject |
            SDK::EObjectFlags::InheritableComponentTemplate | SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed;
        for (int depth=0; object && depth<32; ++depth,object=object->Outer)
            if ((static_cast<unsigned>(object->Flags) & static_cast<unsigned>(forbidden))!=0) return false;
        return object==nullptr;
    }

    bool ValidateReflection()
    {
        using namespace MainMapProofReflection;
        static bool attempted=false,valid=false;
        if (attempted) return valid;
        attempted=true;
        auto* widget=SDK::UWidget::StaticClass();
        auto* image=SDK::UImage::StaticClass();
        auto* slot=SDK::UCanvasPanelSlot::StaticClass();
        valid=Require(SDK::UGameplayStatics::StaticClass(),"GameplayStatics","SpawnObject",0x18,
            {{"ObjectClass",0,8,"ClassProperty"},{"Outer",8,8,"ObjectProperty"},
             {"ReturnValue",16,8,"ObjectProperty"}}) &&
            Require(widget,"Widget","GetOwningPlayer",8,{{"ReturnValue",0,8,"ObjectProperty"}}) &&
            Require(SDK::AActor::StaticClass(),"Actor","GetLevel",8,{{"ReturnValue",0,8,"ObjectProperty"}}) &&
            Require(widget,"Widget","GetCachedGeometry",0x38,
                {{"ReturnValue",0,0x38,"StructProperty",0x8000582}}) &&
            Require(SDK::USlateBlueprintLibrary::StaticClass(),"SlateBlueprintLibrary","GetLocalSize",0x48,
                {{"Geometry",0,0x38,"StructProperty",0x8000182},{"ReturnValue",0x38,0x10,"StructProperty"}}) &&
            Require(widget,"Widget","SetVisibility",1,{{"InVisibility",0,1,"EnumProperty"}}) &&
            Require(widget,"Widget","RemoveFromParent",0,{}) &&
            Require(image,"Image","SetBrushFromMaterial",8,{{"Material",0,8,"ObjectProperty"}}) &&
            Require(image,"Image","SetColorAndOpacity",0x10,{{"InColorAndOpacity",0,0x10,"StructProperty"}}) &&
            Require(SDK::UCanvasPanel::StaticClass(),"CanvasPanel","AddChildToCanvas",0x10,
                {{"Content",0,8,"ObjectProperty"},{"ReturnValue",8,8,"ObjectProperty"}}) &&
            Require(slot,"CanvasPanelSlot","SetAlignment",0x10,{{"InAlignment",0,0x10,"StructProperty"}}) &&
            Require(slot,"CanvasPanelSlot","SetAnchors",0x20,{{"InAnchors",0,0x20,"StructProperty"}}) &&
            Require(slot,"CanvasPanelSlot","SetAutoSize",1,{{"InbAutoSize",0,1,"BoolProperty"}}) &&
            Require(slot,"CanvasPanelSlot","SetOffsets",0x10,{{"InOffset",0,0x10,"StructProperty"}}) &&
            Require(slot,"CanvasPanelSlot","SetZOrder",4,{{"InZOrder",0,4,"IntProperty"}});
        LOG_INFO("MainMap A2.3B: BuildingLayer UMG reflection gate %s",valid?"PASS":"FAIL");
        return valid;
    }

    bool Cleanup()
    {
        if (!g_session) return true;
        auto& session=*g_session;
        if (!MainMapNative::CanOperate())
        {
            if (!session.deferredLogged)
                LOG_WARN("MainMap A2.3B: session=%llu cleanup deferred by admission; ownership retained",
                    session.id);
            session.deferredLogged=true;
            return false;
        }
        auto* image=static_cast<SDK::UImage*>(session.image.Get());
        if (image && session.attached)
        {
            image->RemoveFromParent();
            session.attached=false;
        }
        if (image && session.brushBound)
        {
            image->SetBrushFromMaterial(nullptr);
            session.brushBound=false;
        }
        // Retire consumers before dependencies. On rejection keep this exact
        // owner, including any not-yet-reset leases, for the next admitted tick.
        if (!session.image.Reset() || !session.frame.Reset() ||
            !session.presentParent.Reset() || !session.composeParent.Reset() ||
            !session.canvas.Reset() || !session.terrain.Reset() || !session.world.Reset())
        {
            if (!session.deferredLogged)
                LOG_WARN("MainMap A2.3B: session=%llu release deferred; ownership retained",session.id);
            session.deferredLogged=true;
            return false;
        }
        LOG_INFO("MainMap A2.3B: session=%llu resources released; leases=%zu",session.id,
            MainMapNative::OutstandingLeaseCount());
        delete g_session; // Native C++ owner only; never delete a UObject.
        g_session=nullptr;
        return true;
    }

    void Close(const char* reason)
    {
        if (!MainMapNative::IsGameThread())
        {
            g_cleanupRequested.store(true);
            LOG_WARN("MainMap A2.3B: cleanup requested off GT (%s); no UObject access",reason);
            return;
        }
        if (!g_session) return;
        if (!g_session->closing)
            LOG_INFO("MainMap A2.3B: session=%llu closing (%s)",g_session->id,reason);
        g_session->closing=true;
        Cleanup();
    }

    void Reject(const char* reason)
    {
        LOG_ERROR("MainMap A2.3B: session=%llu candidate rejected (%s)",g_session->id,reason);
        Close("candidate failure");
    }
}

namespace MainMapBuildingLayer
{
    void TerrainConstruct(SDK::UObject* object)
    {
        if (g_stopping.load() || !MainMapNative::CanOperate()) return;
        if (g_cleanupRequested.exchange(false)) Close("rejected destruction");
        if (g_session)
        {
            LOG_WARN("MainMap A2.3B: construct receiver=%p ignored; session=%llu still owns terrain=%p",
                object,g_session->id,g_session->terrain.Get());
            return;
        }
        if (!object || !IsInstance(object) || !object->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass()) ||
            !ValidateReflection())
        {
            LOG_ERROR("MainMap A2.3B: terrain receiver=%p rejected by instance/reflection gate",object);
            return;
        }
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(object);
        auto* tree=terrain->WidgetTree;
        auto* canvas=terrain->CanvasPanelTerrain;
        auto* player=terrain->GetOwningPlayer();
        auto* level=player?player->GetLevel():nullptr;
        auto* world=level?level->OwningWorld:nullptr;
        if (!tree || !canvas || !world || !IsInstance(tree) || !IsInstance(canvas) || !IsInstance(world) ||
            !tree->IsA(SDK::UWidgetTree::StaticClass()) || !canvas->IsA(SDK::UCanvasPanel::StaticClass()) ||
            tree->Outer!=terrain || canvas->Outer!=tree)
        {
            LOG_ERROR("MainMap A2.3B: terrain=%p tree/canvas/owning-player world validation FAILED",terrain);
            return;
        }
        g_session=new(std::nothrow) Session;
        if (!g_session)
        {
            LOG_ERROR("MainMap A2.3B: session allocation failed");
            return;
        }
        auto& session=*g_session;
        session.id=++g_nextSession;
        if (!session.terrain.Acquire(terrain) || !session.world.Acquire(world) || !session.canvas.Acquire(canvas))
        {
            Reject("terrain/world/canvas retention");
            return;
        }
        LOG_INFO("MainMap A2.3B: session=%llu admitted terrain=%p name=%s world=%p canvas=%p",
            session.id,terrain,terrain->GetName().c_str(),world,canvas);
        auto* objectImage=SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(),tree);
        if (!objectImage || !session.image.Acquire(objectImage))
        {
            Reject("image creation/retention");
            return;
        }
        if (!objectImage->IsA(SDK::UImage::StaticClass()) || objectImage->Outer!=tree || !IsInstance(objectImage))
        {
            Reject("image identity");
            return;
        }
        auto* image=static_cast<SDK::UImage*>(objectImage);
        image->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
        image->SetColorAndOpacity({1,1,1,1});
        LOG_INFO("MainMap A2.3B: session=%llu BuildingLayer created image=%p (detached, passive)",session.id,image);
        // Never draw or attach from native widget rebuilding. The admitted
        // post-engine tick prepares one frame after resources/layout are ready.
    }

    void TerrainDestruct(SDK::UObject* terrain,bool admitted)
    {
        if (!admitted)
        {
            // May be called off GT. No receiver dereference or owner access.
            g_cleanupRequested.store(true);
            return;
        }
        if (g_session && g_session->terrain.Get()==terrain) Close("terrain destruct before original");
    }

    void Tick(bool materialsReady)
    {
        if (!MainMapNative::CanOperate()) return;
        if (g_cleanupRequested.exchange(false)) Close("admission-rejected destruction");
        if (!g_session) return;
        if (g_session->closing) { Cleanup(); return; }
        if (g_stopping.load() || g_session->published || !materialsReady) return;
        auto& session=*g_session;
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get());
        auto* canvas=static_cast<SDK::UCanvasPanel*>(session.canvas.Get());
        auto* image=static_cast<SDK::UImage*>(session.image.Get());
        if (!image || !IsInstance(terrain) || !IsInstance(canvas) || terrain->CanvasPanelTerrain!=canvas)
        {
            Reject("session canvas/image changed");
            return;
        }
        const auto size=SDK::USlateBlueprintLibrary::GetLocalSize(canvas->GetCachedGeometry());
        if (!std::isfinite(size.X) || !std::isfinite(size.Y) || size.X<=0 || size.Y<=0)
        {
            if (!session.waitingGeometryLogged)
                LOG_INFO("MainMap A2.3B: session=%llu awaiting positive canvas geometry",session.id);
            session.waitingGeometryLogged=true;
            return;
        }
        auto* compose=MainMapProof::LoadMaterial("/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose",
            SDK::EMaterialDomain::MD_Surface,session.composeParent);
        auto* present=MainMapProof::LoadMaterial("/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
            SDK::EMaterialDomain::MD_UI,session.presentParent);
        if (!compose || !present || !MainMapProof::BuildPresentationFrame(
            static_cast<SDK::UWorld*>(session.world.Get()),compose,present,session.frame,false))
        {
            Reject("diagnostic frame preparation");
            return;
        }
        LOG_INFO("MainMap A2.3B: session=%llu diagnostic candidate prepared target=%p retained (no readback)",
            session.id,session.frame.target.Get());
        if (!MainMapNative::CanOperate() || g_cleanupRequested.load() || g_stopping.load())
        {
            Reject("publication admission/lifecycle closed");
            return;
        }
        image->SetBrushFromMaterial(session.frame.presentMid);
        session.brushBound=true;
        if (image->Brush.ResourceObject!=session.frame.presentMid)
        {
            Reject("presentation material binding");
            return;
        }
        LOG_INFO("MainMap A2.3B: session=%llu presentation MID bound=%p",session.id,session.frame.presentMid);
        auto* slot=canvas->AddChildToCanvas(image);
        // AddChildToCanvas may have attached even if its return is unexpected.
        // Record attachment before validation so rollback always withdraws it.
        session.attached=image->Slot!=nullptr;
        if (!slot || !slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) || image->Slot!=slot ||
            slot->Parent!=canvas || slot->Content!=image)
        {
            Reject("canvas attachment");
            return;
        }
        // Stretch across the parent's current local geometry. Zero margins
        // and alignment give local origin (0,0); no map-world origin is used.
        slot->SetAutoSize(false);
        slot->SetAlignment({0,0});
        slot->SetAnchors({{0,0},{1,1}});
        slot->SetOffsets({0,0,0,0});
        slot->SetZOrder(1);
        session.published=true;
        LOG_INFO("MainMap A2.3B: session=%llu canvas attachment OK slot=%p local=(0,0) size=(%.2f,%.2f) stretch Z=1",
            session.id,slot,size.X,size.Y);
        LOG_INFO("MainMap A2.3B: session=%llu render target published image=%p target=%p; visual result UNVERIFIED",
            session.id,image,session.frame.target.Get());
    }

    void BeforeWorldEnd(SDK::UWorld* world)
    {
        if (!MainMapNative::IsGameThread())
        {
            g_cleanupRequested.store(true);
            LOG_WARN("MainMap A2.3B: world-end cleanup deferred off GT");
            return;
        }
        if (g_session && g_session->world.Get()==world) Close("world end");
    }

    void Shutdown()
    {
        if (g_stopping.exchange(true)) return;
        Close("shutdown");
        const auto outstanding=MainMapNative::OutstandingLeaseCount();
        LOG_INFO("MainMap A2.3B: shutdown leases=%zu; active-game hot reload unsupported, restart required",outstanding);
        if (outstanding!=0)
            LOG_ERROR("MainMap A2.3B: shutdown cleanup incomplete; lease bookkeeping retained, no unsafe release attempted");
    }
}
