#include "BuildingLayer.h"
#include "MaterialProof.h"
#include "ChimeraUI_classes.hpp"
#include "WBP_MapMenuMapArea_classes.hpp"
#include "UMG_classes.hpp"
#include "SlateCore_structs.hpp"
#include "plugin_helpers.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <new>

static_assert(sizeof(SDK::FGeometry)==0x38);
static_assert(sizeof(SDK::FAnchors)==0x20);
static_assert(sizeof(SDK::FMargin)==0x10);

namespace
{
    struct Session final
    {
        MainMapNative::StrongObjectLease terrain,world,canvas,image;
        MainMapNative::StrongObjectLease composeParent,presentParent;
        MainMapProof::PresentationFrame frame;
        bool attached=false;
        bool brushBound=false;
        bool published=false;
        bool closing=false;
    };

    struct WorldRect final { double minX,minY,maxX,maxY; };
    struct Rect final { double minX=0,minY=0,maxX=0,maxY=0; };
    struct Point final { double x=0,y=0; };

    constexpr double kGeometryEpsilon=0.001;

    bool FinitePoint(const Point& p) { return std::isfinite(p.x)&&std::isfinite(p.y); }
    bool Positive(const Rect& r)
    { return std::isfinite(r.minX)&&std::isfinite(r.minY)&&std::isfinite(r.maxX)&&std::isfinite(r.maxY)&&r.maxX>r.minX&&r.maxY>r.minY; }
    bool IsInstance(SDK::UObject* object);

    bool GeometryRect(const SDK::FGeometry& geometry, Rect& out)
    {
        const auto size=SDK::USlateBlueprintLibrary::GetLocalSize(geometry);
        if(!std::isfinite(size.X)||!std::isfinite(size.Y)||size.X<=0||size.Y<=0) return false;
        const SDK::FVector2D local[4]={{0,0},{size.X,0},{size.X,size.Y},{0,size.Y}};
        Point corners[4]{};
        for(int i=0;i<4;++i)
        {
            const auto p=SDK::USlateBlueprintLibrary::LocalToAbsolute(geometry,local[i]);
            corners[i]={p.X,p.Y};
            if(!FinitePoint(corners[i])) return false;
        }
        for(int i=0;i<4;++i)
        {
            const auto& a=corners[i]; const auto& b=corners[(i+1)%4];
            if(std::abs(a.x-b.x)>kGeometryEpsilon && std::abs(a.y-b.y)>kGeometryEpsilon) return false;
        }
        out={corners[0].x,corners[0].y,corners[0].x,corners[0].y};
        for(int i=1;i<4;++i)
        { out.minX=(std::min)(out.minX,corners[i].x); out.minY=(std::min)(out.minY,corners[i].y);
          out.maxX=(std::max)(out.maxX,corners[i].x); out.maxY=(std::max)(out.maxY,corners[i].y); }
        return Positive(out);
    }

    bool Intersect(Rect& a,const Rect& b)
    { a.minX=(std::max)(a.minX,b.minX); a.minY=(std::max)(a.minY,b.minY); a.maxX=(std::min)(a.maxX,b.maxX); a.maxY=(std::min)(a.maxY,b.maxY); return Positive(a); }
    // One owner, no replacement queue. Deleted only after every lease resets.
    // If final teardown rejects release, leave the bookkeeping intact and log
    // it rather than running lease destructors after native admission closes.
    Session* g_session=nullptr;
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

    bool TryGetVisibleWorldRect(WorldRect& out)
    {
        if(!MainMapNative::CanOperate()||!g_session||g_session->closing) return false;
        auto& session=*g_session;
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get());
        auto* world=static_cast<SDK::UWorld*>(session.world.Get());
        if(!terrain||!world||world!=SDK::UWorld::GetWorld()||
            !IsInstance(terrain)||!terrain->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass())||
            !terrain->IsVisible()||!terrain->IsRendered()) return false;

        auto* treeObject=terrain->Outer;
        if(!treeObject||!IsInstance(treeObject)||!treeObject->IsA(SDK::UWidgetTree::StaticClass())) return false;
        auto* tree=static_cast<SDK::UWidgetTree*>(treeObject);
        auto* areaObject=tree->Outer;
        if(!areaObject||!IsInstance(areaObject)||!areaObject->IsA(SDK::UWBP_MapMenuMapArea_C::StaticClass())) return false;
        auto* area=static_cast<SDK::UCrUW_MapMenuMapArea*>(areaObject);
        if(area->WidgetTree!=tree||area->MapMenuTerrain!=terrain) return false;

        auto* canvas=area->CanvasPanelMapArea;
        if(!canvas||!IsInstance(canvas)||!canvas->IsA(SDK::UCanvasPanel::StaticClass())||
            terrain->GetParent()!=canvas||!area->IsVisible()||!area->IsRendered()||
            !canvas->IsVisible()||!canvas->IsRendered()) return false;

        Rect effective{};
        if(!GeometryRect(canvas->GetPaintSpaceGeometry(),effective)) return false;
        for(auto* ancestor=static_cast<SDK::UWidget*>(canvas);ancestor;ancestor=ancestor->GetParent())
        {
            if(!IsInstance(ancestor)||!ancestor->IsVisible()||!ancestor->IsRendered()) return false;
            const auto clipping=ancestor->GetClipping();
            if(clipping==SDK::EWidgetClipping::Inherit) continue;
            if(clipping!=SDK::EWidgetClipping::ClipToBounds&&
                clipping!=SDK::EWidgetClipping::ClipToBoundsAlways) return false;
            Rect clip{};
            if(!GeometryRect(ancestor->GetPaintSpaceGeometry(),clip)||!Intersect(effective,clip)) return false;
        }

        Rect viewport{};
        if(!GeometryRect(SDK::UWidgetLayoutLibrary::GetViewportWidgetGeometry(world),viewport)||
            !Intersect(effective,viewport)) return false;

        const auto terrainGeometry=terrain->GetPaintSpaceGeometry();
        const Point absolute[4]={{effective.minX,effective.minY},{effective.maxX,effective.minY},
            {effective.maxX,effective.maxY},{effective.minX,effective.maxY}};
        Rect localBounds{};
        for(int i=0;i<4;++i)
        {
            const auto local=SDK::USlateBlueprintLibrary::AbsoluteToLocal(terrainGeometry,
                {static_cast<float>(absolute[i].x),static_cast<float>(absolute[i].y)});
            const Point point{local.X,local.Y};
            if(!FinitePoint(point)) return false;
            if(i==0) localBounds={point.x,point.y,point.x,point.y};
            else
            {
                localBounds.minX=(std::min)(localBounds.minX,point.x);
                localBounds.minY=(std::min)(localBounds.minY,point.y);
                localBounds.maxX=(std::max)(localBounds.maxX,point.x);
                localBounds.maxY=(std::max)(localBounds.maxY,point.y);
            }
        }
        if(!Positive(localBounds)) return false;

        auto* settings=SDK::UCrMapMenuDevSettings::GetDefaultObj();
        auto* terrainData=settings?settings->TerrainData.Get():nullptr;
        if(!terrainData||!IsInstance(terrainData)||!terrainData->IsA(SDK::UCrMapMenuTerrainData::StaticClass())) return false;
        const double originX=terrainData->MapTerrainTopLeftPivotPoint.X;
        const double originY=terrainData->MapTerrainTopLeftPivotPoint.Y;
        WorldRect result{originX+localBounds.minX*100.0,originY+localBounds.minY*100.0,
            originX+localBounds.maxX*100.0,originY+localBounds.maxY*100.0};
        if(!std::isfinite(result.minX)||!std::isfinite(result.minY)||
            !std::isfinite(result.maxX)||!std::isfinite(result.maxY)||
            result.maxX<=result.minX||result.maxY<=result.minY) return false;
        out=result;
        return true;
    }
    bool Cleanup()
    {
        if (!g_session) return true;
        auto& session=*g_session;
        if (!MainMapNative::CanOperate())
        {
            LOG_WARN("MainMap: session cleanup deferred by admission; ownership retained");
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
            LOG_WARN("MainMap: session resource release deferred; ownership retained");
            return false;
        }
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
        g_session->closing=true;
        Cleanup();
    }

    void Reject(const char* reason)
    {
        LOG_ERROR("MainMap: terrain presentation candidate rejected (%s)",reason);
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
            LOG_WARN("MainMap: terrain construct receiver=%p ignored while an existing session owns terrain=%p",
                object,g_session->terrain.Get());
            return;
        }
        if (!object || !IsInstance(object) || !object->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass()))
        {
            LOG_ERROR("MainMap A2.3B: terrain receiver=%p rejected by instance gate",object);
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
        if (!session.terrain.Acquire(terrain) || !session.world.Acquire(world) || !session.canvas.Acquire(canvas))
        {
            Reject("terrain/world/canvas retention");
            return;
        }
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
        if (g_stopping.load()) return;
        auto& session=*g_session;
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get());
        auto* canvas=static_cast<SDK::UCanvasPanel*>(session.canvas.Get());
        auto* image=static_cast<SDK::UImage*>(session.image.Get());
        if (!image || !IsInstance(terrain) || !IsInstance(canvas) || terrain->CanvasPanelTerrain!=canvas)
        {
            Reject("session canvas/image changed");
            return;
        }
        if(session.published||!materialsReady) return;
        const auto size=SDK::USlateBlueprintLibrary::GetLocalSize(canvas->GetCachedGeometry());
        if (!std::isfinite(size.X) || !std::isfinite(size.Y) || size.X<=0 || size.Y<=0)
        {
            return;
        }
        auto* compose=MainMapProof::LoadMaterial("/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose",
            SDK::EMaterialDomain::MD_Surface,session.composeParent);
        auto* present=MainMapProof::LoadMaterial("/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
            SDK::EMaterialDomain::MD_UI,session.presentParent);
        if (!compose || !present || !MainMapProof::PreparePresentationFrame(
            static_cast<SDK::UWorld*>(session.world.Get()),compose,present,session.frame))
        {
            Reject("production material/target preparation");
            return;
        }
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
        if (outstanding!=0)
            LOG_ERROR("MainMap A2.3B: shutdown cleanup incomplete; lease bookkeeping retained, no unsafe release attempted");
    }
}
