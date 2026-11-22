// Owns native terrain admission, clipped world geometry, and composite-grid presentation ordering.
#include "BuildingLayer.h"
#if defined(_DEBUG)
#include "BuildingMapGeometryDiagnostics.h"
#endif
#include "BuildingMapRemote.h"
#include "BuildingMapTarget.h"
#include "BuildingMapPresentation.h"
#include "RenderingProof.h"
#include "Native/NativeApi.h"
#include "ChimeraUI_classes.hpp"
#include "WBP_MapMenuMapArea_classes.hpp"
#include "UMG_classes.hpp"
#include "SlateCore_structs.hpp"
#include "plugin_helpers.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <new>
#include <limits>

static_assert(sizeof(SDK::FGeometry)==0x38);
static_assert(sizeof(SDK::FAnchors)==0x20);
static_assert(sizeof(SDK::FMargin)==0x10);

namespace
{
    struct Session final
    {
        MainMapNative::StrongObjectLease terrain,world,canvas;
        MainMapNative::StrongObjectLease owningMenu;
        uint64_t epoch=0;
        bool closing=false;
        bool terrainDestructionStarted=false;
        uint64_t diagnosticTickBlocks=0;
        uint64_t diagnosticContextRejections=0;
        uint64_t diagnosticCleanupDeferrals=0;
        const char* closingReason=nullptr;
    };

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
    uint64_t g_nextConsumerEpoch=1;
    std::atomic<const char*> g_cleanupRequestedReason{nullptr};
    std::atomic_bool g_stopping{false};
    // Lifecycle identity only; never dereferenced or used as a UObject lease.
    std::atomic<SDK::UWorld*> g_gameplayWorld{nullptr};
    uint64_t g_contextWithoutSession=0;
    uint64_t g_tickWithoutAdmission=0;
    MainMapBuildingMap::BuildingMapRemote g_buildingMapRemote;

    void RequestCleanup(const char* reason)
    {
        const char* expected=nullptr;
        g_cleanupRequestedReason.compare_exchange_strong(expected,reason);
    }

    void LogSessionGate(uint64_t& seen,unsigned gate,const char* category,
        const Session* session,const char* reason)
    {
        if(gate>=64)return;
        const uint64_t bit=uint64_t{1}<<gate;
        if(seen&bit)return;
        seen|=bit;
        LOG_INFO("MainMap diagnostics: %s epoch=%llu world=%p gate=%s",category,
            static_cast<unsigned long long>(session?session->epoch:0),
            session?session->world.Get():nullptr,reason);
    }

    bool IsInstance(SDK::UObject* object)
    {
        const auto forbidden=SDK::EObjectFlags::ClassDefaultObject | SDK::EObjectFlags::ArchetypeObject |
            SDK::EObjectFlags::InheritableComponentTemplate | SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed;
        for (int depth=0; object && depth<32; ++depth,object=object->Outer)
            if ((static_cast<unsigned>(object->Flags) & static_cast<unsigned>(forbidden))!=0) return false;
        return object==nullptr;
    }

    bool IsCurrentTerrainWorld(SDK::UCrUW_MapMenuTerrain* terrain,SDK::UWorld* world)
    {
        // GWorld is a context-dependent accessor, not the gameplay lifecycle.
        // All callers have native admission before following these typed links.
        if(!terrain || !world || world!=g_gameplayWorld.load() ||
            !IsInstance(world) || !IsInstance(terrain) ||
            !terrain->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass()))return false;
        auto* player=terrain->GetOwningPlayer();
        if(!player || !IsInstance(player))return false;
        auto* level=player->GetLevel();
        return level && IsInstance(level) && level->OwningWorld==world;
    }

    bool TryGetVisibleWorldRectImpl(MainMapBuildingLayer::WorldRect& out)
    {
        if(!MainMapNative::CanOperate()||!g_session||g_session->closing) return false;
        auto& session=*g_session;
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get());
        auto* world=static_cast<SDK::UWorld*>(session.world.Get());
        if(!IsCurrentTerrainWorld(terrain,world)||
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
        MainMapBuildingLayer::WorldRect result{originX+localBounds.minX*100.0,originY+localBounds.minY*100.0,
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
        auto* const worldIdentity=session.world.Get();
        if (!MainMapNative::CanOperate())
        {
            LogSessionGate(session.diagnosticCleanupDeferrals,0,"terrain session cleanup deferred",
                &session,"native-admission-closed");
            return false;
        }
        if (!MainMapBuildingMap::BuildingMapPresentation::RetireShared(session.terrainDestructionStarted))
        {
            LogSessionGate(session.diagnosticCleanupDeferrals,3,"terrain session cleanup deferred",
                &session,"presentation-resource-release-rejected");
            return false;
        }
        if (!MainMapBuildingMap::BuildingMapTarget::InvalidateGridPlanInputShared())
        {
            LogSessionGate(session.diagnosticCleanupDeferrals, 4, "terrain session cleanup deferred",
                &session, "grid-resource-release-rejected");
            return false;
        }
        if (!MainMapBuildingMap::BuildingMapTarget::RetireShared())
        {
            LogSessionGate(session.diagnosticCleanupDeferrals,2,"terrain session cleanup deferred",
                &session,"composition-resource-release-rejected");
            return false;
        }
        g_buildingMapRemote.CloseViewport();
        // Retire the Section composition before releasing terrain dependencies.
        // Preserve every terrain lease if any release is rejected.
        if (!session.owningMenu.Reset() || !session.canvas.Reset() ||
            !session.terrain.Reset() || !session.world.Reset())
        {
            LogSessionGate(session.diagnosticCleanupDeferrals,1,"terrain session cleanup deferred",
                &session,"strong-lease-release-rejected");
            return false;
        }
        LOG_INFO("MainMap diagnostics: terrain session cleanup completed epoch=%llu world=%p reason=%s",
            static_cast<unsigned long long>(session.epoch),worldIdentity,
            session.closingReason?session.closingReason:"unspecified");
        delete g_session; // Native C++ owner only; never delete a UObject.
        g_session=nullptr;
        return true;
    }

    void Close(const char* reason)
    {
        if (!MainMapNative::IsGameThread())
        {
            RequestCleanup(reason);
            LOG_WARN("MainMap A2.3B: cleanup requested off GT (%s); no UObject access",reason);
            return;
        }
        if (!g_session) return;
        if(!g_session->closing)
        {
            g_session->closingReason=reason;
            LOG_INFO("MainMap diagnostics: terrain session closing epoch=%llu world=%p reason=%s",
                static_cast<unsigned long long>(g_session->epoch),g_session->world.Get(),reason);
        }
        g_session->closing=true;
        Cleanup();
    }

    void Reject(const char* reason)
    {
        LOG_ERROR("MainMap: terrain session rejected (%s)",reason);
        Close(reason);
    }
}

namespace MainMapBuildingLayer
{
    bool TryGetConsumerContext(ConsumerContext& out)
    {
        out={};
        if(!MainMapNative::CanOperate())
        {
            LogSessionGate(g_contextWithoutSession,0,"consumer context rejected",nullptr,"native-admission-closed");
            return false;
        }
        if(!g_session)
        {
            LogSessionGate(g_contextWithoutSession,1,"consumer context rejected",nullptr,"no-terrain-session");
            return false;
        }
        auto& session=*g_session;
        if(session.closing)
        {
            LogSessionGate(session.diagnosticContextRejections,0,"consumer context rejected",&session,"session-closing");
            return false;
        }
        if(g_stopping.load())
        {
            LogSessionGate(session.diagnosticContextRejections,1,"consumer context rejected",&session,"shutdown-started");
            return false;
        }
        if(g_cleanupRequestedReason.load())
        {
            LogSessionGate(session.diagnosticContextRejections,2,"consumer context rejected",&session,"cleanup-request-pending");
            return false;
        }
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get());
        auto* world=static_cast<SDK::UWorld*>(session.world.Get());
        if(!IsCurrentTerrainWorld(terrain,world))
        { LogSessionGate(session.diagnosticContextRejections,3,"consumer context rejected",&session,"world-missing-or-replaced");return false; }
        if(!IsInstance(world) || !IsInstance(terrain))
        { LogSessionGate(session.diagnosticContextRejections,4,"consumer context rejected",&session,"world-or-terrain-instance-invalid");return false; }
        auto* treeObject=terrain->Outer;
        if(!treeObject || !IsInstance(treeObject) || !treeObject->IsA(SDK::UWidgetTree::StaticClass()))
        { LogSessionGate(session.diagnosticContextRejections,5,"consumer context rejected",&session,"terrain-outer-not-live-widget-tree");return false; }
        auto* tree=static_cast<SDK::UWidgetTree*>(treeObject);
        auto* areaObject=tree->Outer;
        if(!areaObject || !IsInstance(areaObject) || !areaObject->IsA(SDK::UWBP_MapMenuMapArea_C::StaticClass()))
        { LogSessionGate(session.diagnosticContextRejections,6,"consumer context rejected",&session,"tree-outer-not-live-map-area");return false; }
        auto* area=static_cast<SDK::UCrUW_MapMenuMapArea*>(areaObject);
        if(area->WidgetTree!=tree || area->MapMenuTerrain!=terrain ||
            !area->CanvasPanelMapArea || !IsInstance(area->CanvasPanelMapArea) ||
            !area->CanvasPanelMapArea->IsA(SDK::UCanvasPanel::StaticClass()) ||
            terrain->GetParent()!=area->CanvasPanelMapArea)
        { LogSessionGate(session.diagnosticContextRejections,7,"consumer context rejected",&session,"map-area-widget-links");return false; }
        auto* menuTreeObject=area->Outer;
        if(!menuTreeObject || !IsInstance(menuTreeObject) || !menuTreeObject->IsA(SDK::UWidgetTree::StaticClass()))
        { LogSessionGate(session.diagnosticContextRejections,8,"consumer context rejected",&session,"area-outer-not-live-menu-tree");return false; }
        auto* menuTree=static_cast<SDK::UWidgetTree*>(menuTreeObject);
        auto* menuObject=menuTree->Outer;
        if(!menuObject || !IsInstance(menuObject) || !menuObject->IsA(SDK::UCrUW_MapMenu::StaticClass()))
        { LogSessionGate(session.diagnosticContextRejections,9,"consumer context rejected",&session,"menu-tree-outer-not-live-menu");return false; }
        auto* menu=static_cast<SDK::UCrUW_MapMenu*>(menuObject);
        if(menu->WidgetTree!=menuTree || menu->MapMenuMapArea!=area)
        { LogSessionGate(session.diagnosticContextRejections,10,"consumer context rejected",&session,"menu-widget-links");return false; }
        if(session.owningMenu.Get() && session.owningMenu.Get()!=menu)
        { LogSessionGate(session.diagnosticContextRejections,11,"consumer context rejected",&session,"owning-menu-mismatch");return false; }
        if(!session.owningMenu.Get() && !session.owningMenu.Acquire(menu))
        { LogSessionGate(session.diagnosticContextRejections,12,"consumer context rejected",&session,"owning-menu-retention");return false; }
        if(!menu->IsActivated())
        { LogSessionGate(session.diagnosticContextRejections,13,"consumer context rejected",&session,"menu-not-activated");return false; }
        // Activation is separate from layout availability. Phase 1 opens only
        // metadata ownership; Phase 2 must also validate clipped geometry/materials.
        for(auto* widget=static_cast<SDK::UWidget*>(terrain);widget;widget=widget->GetParent())
            if(!IsInstance(widget) || !widget->IsVisible() || !widget->IsRendered())
            { LogSessionGate(session.diagnosticContextRejections,14,"consumer context rejected",&session,"terrain-widget-ancestor-not-live-visible-rendered");return false; }
        if(!menu->IsVisible() || !menu->IsRendered() || !area->IsVisible() || !area->IsRendered())
        { LogSessionGate(session.diagnosticContextRejections,15,"consumer context rejected",&session,"menu-or-area-not-visible-rendered");return false; }
        auto* settings=SDK::UCrMapMenuDevSettings::GetDefaultObj();
        auto* data=settings?settings->TerrainData.Get():nullptr;
        if(!data || !IsInstance(data) || !data->IsA(SDK::UCrMapMenuTerrainData::StaticClass()))
        { LogSessionGate(session.diagnosticContextRejections,16,"consumer context rejected",&session,"terrain-data-invalid");return false; }
        const double x=data->MapTerrainTopLeftPivotPoint.X,y=data->MapTerrainTopLeftPivotPoint.Y;
        if(!std::isfinite(x) || !std::isfinite(y))
        { LogSessionGate(session.diagnosticContextRejections,17,"consumer context rejected",&session,"terrain-origin-non-finite");return false; }
        out={world,session.epoch,x,y};
        return true;
    }

    bool TryGetTilePresentationContext(SDK::UCanvasPanel*& canvas,SDK::UWidgetTree*& tree)
    {
        canvas=nullptr;tree=nullptr;
        if(!MainMapNative::CanOperate()||!g_session||g_session->closing)return false;
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(g_session->terrain.Get());
        if(!terrain||!IsInstance(terrain)||!terrain->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass()))return false;
        auto* treeObject=terrain->WidgetTree;
        auto* canvasObject=terrain->CanvasPanelTerrain;
        if(!treeObject||!canvasObject||!IsInstance(treeObject)||!IsInstance(canvasObject)||
            !treeObject->IsA(SDK::UWidgetTree::StaticClass())||!canvasObject->IsA(SDK::UCanvasPanel::StaticClass())||
            treeObject->Outer!=terrain||canvasObject->Outer!=treeObject)return false;
        canvas=static_cast<SDK::UCanvasPanel*>(canvasObject);
        tree=static_cast<SDK::UWidgetTree*>(treeObject);
        return true;
    }

    bool TryGetVisibleWorldRect(WorldRect& out)
    {
        return TryGetVisibleWorldRectImpl(out);
    }

    /** Exposes the terrain owner's remote data to presentation without duplicating its image cache. */
    MainMapBuildingMap::BuildingMapRemote& GetBuildingMapRemote() noexcept
    {
        return g_buildingMapRemote;
    }

    bool TryGetGroupedTilePixelSpan(double& out)
    {
        out=0;
        if(!MainMapNative::CanOperate() || !g_session || g_session->closing)return false;
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(g_session->terrain.Get());
        auto* world=static_cast<SDK::UWorld*>(g_session->world.Get());
        if(!IsCurrentTerrainWorld(terrain,world))return false;
        const auto geometry=terrain->GetPaintSpaceGeometry();
        Rect supported{};
        if(!GeometryRect(geometry,supported))return false;
        // Verify the live viewport root convention instead of treating Slate
        // absolute units as pixels or assuming a retained design resolution.
        const auto root=SDK::UWidgetLayoutLibrary::GetViewportWidgetGeometry(world);
        const auto rootSize=SDK::USlateBlueprintLibrary::GetLocalSize(root);
        const auto outputSize=SDK::UWidgetLayoutLibrary::GetViewportSize(world);
        SDK::FVector2D rootMin{},rootMax{},unused{};
        SDK::USlateBlueprintLibrary::LocalToViewport(world,root,{0,0},&rootMin,&unused);
        SDK::USlateBlueprintLibrary::LocalToViewport(world,root,rootSize,&rootMax,&unused);
        if(!std::isfinite(rootSize.X)||!std::isfinite(rootSize.Y)||rootSize.X<=0||rootSize.Y<=0||
            !std::isfinite(outputSize.X)||!std::isfinite(outputSize.Y)||outputSize.X<=0||outputSize.Y<=0||
            !std::isfinite(rootMin.X)||!std::isfinite(rootMin.Y)||!std::isfinite(rootMax.X)||!std::isfinite(rootMax.Y)||
            std::abs(rootMin.X)>kGeometryEpsilon||std::abs(rootMin.Y)>kGeometryEpsilon||
            std::abs(rootMax.X-outputSize.X)>kGeometryEpsilon*outputSize.X||
            std::abs(rootMax.Y-outputSize.Y)>kGeometryEpsilon*outputSize.Y)return false;
        // PixelPosition is viewport output pixels; ViewportPosition is widget
        // space. Do not multiply by a second viewport/DPI scale.
        SDK::FVector2D pixels[4]{},viewport{};
        const SDK::FVector2D local[4]={{0,0},{500,0},{0,500},{500,500}};
        for(int i=0;i<4;++i)
        {
            SDK::USlateBlueprintLibrary::LocalToViewport(world,geometry,local[i],&pixels[i],&viewport);
            if(!std::isfinite(pixels[i].X)||!std::isfinite(pixels[i].Y))return false;
        }
        const double xx=pixels[1].X-pixels[0].X,xy=pixels[1].Y-pixels[0].Y;
        const double yx=pixels[2].X-pixels[0].X,yy=pixels[2].Y-pixels[0].Y;
        const double x=std::hypot(xx,xy),y=std::hypot(yx,yy);
        if(!std::isfinite(x)||!std::isfinite(y)||x<=0||y<=0 ||
            std::abs(x-y)>kGeometryEpsilon*(std::max)(x,y) ||
            std::abs(xx*yx+xy*yy)>kGeometryEpsilon*x*y ||
            std::abs(pixels[3].X-pixels[0].X-xx-yx)>kGeometryEpsilon ||
            std::abs(pixels[3].Y-pixels[0].Y-xy-yy)>kGeometryEpsilon)return false;
        out=(std::max)(x,y);
        return true;
    }

    bool HasActiveTerrainSession()
    {
        return g_session && !g_session->closing && !g_stopping.load();
    }

    bool IsMainMapVisible()
    {
        ConsumerContext context{};
        return TryGetConsumerContext(context);
    }

    void TerrainConstruct(SDK::UObject* object)
    {
        if (g_stopping.load() || !MainMapNative::CanOperate()) return;
        if (const char* reason=g_cleanupRequestedReason.exchange(nullptr)) Close(reason);
        if (g_session)
        {
            if (g_session->closing) Cleanup();
        }
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
        if(g_nextConsumerEpoch==(std::numeric_limits<uint64_t>::max)())return;
        g_session=new(std::nothrow) Session;
        if (!g_session)
        {
            LOG_ERROR("MainMap A2.3B: session allocation failed");
            return;
        }
        auto& session=*g_session;
        session.epoch=g_nextConsumerEpoch++;
        LOG_INFO("MainMap diagnostics: terrain session created epoch=%llu world=%p terrain=%p",
            static_cast<unsigned long long>(session.epoch),world,terrain);
        if (!session.terrain.Acquire(terrain) || !session.world.Acquire(world) || !session.canvas.Acquire(canvas))
        {
            Reject("terrain/world/canvas retention");
            return;
        }
        // BCT1 owns its per-tile image and material leases. This service owns
        // only the terrain session and its geometry/admission context.
    }

    bool TerrainDestruct(SDK::UObject* terrain,bool admitted)
    {
        if (!admitted)
        {
            // May be called off GT. No receiver dereference or owner access.
            RequestCleanup("terrain destruct admission rejected");
            return false;
        }
        if (g_session && g_session->terrain.Get()==terrain)
        {
            Close("terrain destruct before original");
            if (g_session && g_session->terrain.Get()==terrain)
                g_session->terrainDestructionStarted=true;
            return true;
        }
        return false;
    }

    /** Advances the admitted terrain session and overlay, with automatic geometry sampling in Client Debug. */
    void Tick()
    {
        if (!MainMapNative::CanOperate())
        {
            LogSessionGate(g_tickWithoutAdmission,0,"terrain Tick blocked",nullptr,"native-admission-closed");
            return;
        }
        if (const char* reason=g_cleanupRequestedReason.exchange(nullptr)) Close(reason);
        if (!g_session) return;
        if (g_session->closing) { Cleanup(); return; }
        auto& session=*g_session;
        if (g_stopping.load())
        { LogSessionGate(session.diagnosticTickBlocks,1,"terrain Tick blocked",&session,"shutdown-started");return; }
        if(!IsCurrentTerrainWorld(static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get()),
            static_cast<SDK::UWorld*>(session.world.Get())))
        { LogSessionGate(session.diagnosticTickBlocks,8,"terrain Tick blocked",&session,"world-replaced");Close("world replacement");return; }
        auto* terrain=static_cast<SDK::UCrUW_MapMenuTerrain*>(session.terrain.Get());
        auto* canvas=static_cast<SDK::UCanvasPanel*>(session.canvas.Get());
        if (!IsInstance(terrain) || !IsInstance(canvas) || terrain->CanvasPanelTerrain!=canvas)
        {
            LogSessionGate(session.diagnosticTickBlocks,9,"terrain Tick blocked",&session,"session-terrain-or-canvas-invalid");
            Close("session terrain/canvas changed");
            return;
        }
        MainMapBuildingLayer::WorldRect visibleWorld{};
        if (!TryGetVisibleWorldRect(visibleWorld))
        {
            if (!MainMapBuildingMap::BuildingMapPresentation::RetireShared()) return;
            MainMapBuildingMap::BuildingMapTarget::InvalidateGridPlanInputShared();
            if (!MainMapBuildingMap::BuildingMapTarget::RetireShared()) return;
            g_buildingMapRemote.CloseViewport();
            return;
        }
        g_buildingMapRemote.UpdateViewport(visibleWorld.minX, visibleWorld.minY,
            visibleWorld.maxX - visibleWorld.minX, visibleWorld.maxY - visibleWorld.minY);
        MainMapBuildingLayer::ConsumerContext context{};
        SDK::UCanvasPanel* presentationCanvas=nullptr;
        SDK::UWidgetTree* presentationTree=nullptr;
        if (!TryGetConsumerContext(context) || !TryGetTilePresentationContext(presentationCanvas,presentationTree))
        {
            if (!MainMapBuildingMap::BuildingMapPresentation::RetireShared()) return;
            MainMapBuildingMap::BuildingMapTarget::InvalidateGridPlanInputShared();
            if (!MainMapBuildingMap::BuildingMapTarget::RetireShared()) return;
            g_buildingMapRemote.CloseViewport();
            return;
        }
        const bool gridPlanCurrent = MainMapBuildingMap::BuildingMapTarget::UpdateGridPlanShared(
            visibleWorld, session.epoch);
        if (gridPlanCurrent)
        {
            MainMapBuildingMap::BuildingMapTarget::ReconcileGridSourcesShared(
                g_buildingMapRemote, session.epoch);
        }
        const auto& gridPlanner = MainMapBuildingMap::BuildingMapTarget::GetGridPlannerShared();
        const bool sourceSnapshotCurrent =
            g_buildingMapRemote.GetPollState() == MainMapBuildingMap::BuildingMapRemote::PollState::Complete &&
            MainMapBuildingMap::BuildingMapTarget::HasCurrentGridSourcesShared();
        const bool presentationPrepared =
            MainMapBuildingMap::BuildingMapPresentation::PrepareGridUpdateShared(
                session.epoch, gridPlanner.Assignments(), sourceSnapshotCurrent);
        if (gridPlanCurrent && presentationPrepared)
        {
            MainMapBuildingMap::BuildingMapTarget::PrepareGridCompositesShared(
                static_cast<SDK::UObject*>(session.world.Get()), g_buildingMapRemote,
                session.epoch, MainMapRenderingProof::RenderingProof::IsMaterialContainerReady());
        }
        if (!gridPlanCurrent && presentationPrepared)
            MainMapBuildingMap::BuildingMapTarget::RetireShared();
#if defined(_DEBUG)
        MainMapBuildingMap::BuildingMapGeometryDiagnostics::Sample(terrain,context,visibleWorld);
#endif
        if (!presentationPrepared && sourceSnapshotCurrent)
        {
            return;
        }
        const auto compositeTargets = MainMapBuildingMap::BuildingMapTarget::GetCompositeTargetInfosShared();
        if (!MainMapBuildingMap::BuildingMapPresentation::UpdateGridShared(context, visibleWorld,
            presentationCanvas, presentationTree, compositeTargets, sourceSnapshotCurrent,
            MainMapRenderingProof::RenderingProof::IsMaterialContainerReady()))
            LogSessionGate(session.diagnosticTickBlocks,10,"Building Map presentation update failed",
                &session,"resource-or-geometry-update-rejected");
    }

    void WorldBeginPlay(SDK::UWorld* world)
    {
        g_gameplayWorld.store(world);
    }

    bool BeforeWorldEnd(SDK::UWorld* world)
    {
        auto* expected=world;
        g_gameplayWorld.compare_exchange_strong(expected,nullptr);
        if (!MainMapNative::IsGameThread())
        {
            RequestCleanup("world end callback off game thread");
            LOG_WARN("MainMap A2.3B: world-end cleanup deferred off GT");
            return false;
        }
        if (g_session && g_session->world.Get()==world)
        {
            Close("world end");
            return true;
        }
        return false;
    }

    void Shutdown()
    {
        if (g_stopping.exchange(true)) return;
        Close("shutdown");
        const bool presentationRetired=MainMapBuildingMap::BuildingMapPresentation::RetireShared(
            g_session && g_session->terrainDestructionStarted);
        if (presentationRetired && MainMapBuildingMap::BuildingMapTarget::RetireShared() && g_session)
            Cleanup();
        g_buildingMapRemote.Shutdown();
        const auto outstanding=MainMapNative::OutstandingLeaseCount();
        if (outstanding!=0)
            LOG_ERROR("MainMap A2.3B: shutdown cleanup incomplete; lease bookkeeping retained, no unsafe release attempted");
    }
}
