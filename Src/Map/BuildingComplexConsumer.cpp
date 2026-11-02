#include "BuildingComplexConsumer.h"
#include "BuildingLayer.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "..\..\..\..\MiniMap\Solution\Src\Map\Buildings\BuildingComplexAbi.h"
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

// Use the provider declarations, never a consumer replica of the frozen ABI.
#define BC_LAYOUT(T,S) static_assert(sizeof(T)==S && alignof(T)==8)
BC_LAYOUT(BC_Key,16); BC_LAYOUT(BC_Rect,32); BC_LAYOUT(BC_Ref,24);
BC_LAYOUT(BC_Open,64); BC_LAYOUT(BC_Grid,96); BC_LAYOUT(BC_Demand,64);
BC_LAYOUT(BC_Status,208); BC_LAYOUT(BC_Lease,40); BC_LAYOUT(BC_Policy,64); BC_LAYOUT(BC_Api,104);
#undef BC_LAYOUT
static_assert(BC_VERSION==1 && BC_CONTRACT_ID==0x42435431u);
static_assert(sizeof(void*)==8);
static_assert(offsetof(BC_Key,Y)==8 && offsetof(BC_Rect,MaxY)==24);
static_assert(offsetof(BC_Ref,Session)==8 && offsetof(BC_Ref,Id)==16);
static_assert(offsetof(BC_Open,World)==8 && offsetof(BC_Open,ClientWorldEpoch)==16 && offsetof(BC_Open,GridSchema)==56);
static_assert(offsetof(BC_Grid,Session)==16 && offsetof(BC_Grid,OriginX)==48 && offsetof(BC_Grid,CellPixels)==80);
static_assert(offsetof(BC_Demand,Session)==8 && offsetof(BC_Demand,TileX)==24 && offsetof(BC_Demand,GridId)==48);
static_assert(offsetof(BC_Status,RequestId)==16 && offsetof(BC_Status,PublicationId)==24 &&
    offsetof(BC_Status,TileX)==72 && offsetof(BC_Status,WorldBounds)==88 && offsetof(BC_Status,InteriorSide)==120 &&
    offsetof(BC_Status,UVScaleX)==136 && offsetof(BC_Status,ReadinessRevision)==168 && offsetof(BC_Status,FailedCells)==200);
static_assert(offsetof(BC_Lease,Session)==8 && offsetof(BC_Lease,LeaseId)==16 && offsetof(BC_Lease,PublicationId)==24 && offsetof(BC_Lease,Texture)==32);
static_assert(offsetof(BC_Policy,Sides)==16 && offsetof(BC_Policy,DowngradeFraction)==48 && offsetof(BC_Policy,MaxInteriorSide)==56);
static_assert(offsetof(BC_Api,Open)==16 && offsetof(BC_Api,InspectSession)==24 && offsetof(BC_Api,Submit)==32 &&
    offsetof(BC_Api,InspectRequest)==40 && offsetof(BC_Api,InspectPublication)==48 && offsetof(BC_Api,Acquire)==56 &&
    offsetof(BC_Api,Release)==64 && offsetof(BC_Api,Withdraw)==72 && offsetof(BC_Api,WithdrawAll)==80 &&
    offsetof(BC_Api,Close)==88 && offsetof(BC_Api,GetPolicy)==96);
using Getter=uint32_t (__cdecl *)(uint32_t,BC_Api*);
static_assert(std::is_same_v<decltype(&MiniMap_GetBuildingComplexApi),Getter>);
static_assert(std::is_same_v<BC_OpenFn,uint32_t (BC_CALL *)(const BC_Open*,BC_Grid*)>);
static_assert(std::is_same_v<BC_InspectSessionFn,uint32_t (BC_CALL *)(const BC_Ref*,BC_Grid*)>);
static_assert(std::is_same_v<BC_SubmitFn,uint32_t (BC_CALL *)(const BC_Demand*,uint64_t*)>);
static_assert(std::is_same_v<BC_InspectRequestFn,uint32_t (BC_CALL *)(const BC_Ref*,BC_Status*)>);
static_assert(std::is_same_v<BC_InspectPublicationFn,uint32_t (BC_CALL *)(const BC_Ref*,BC_Status*)>);
static_assert(std::is_same_v<BC_AcquireFn,uint32_t (BC_CALL *)(const BC_Ref*,BC_Lease*)>);
static_assert(std::is_same_v<BC_ReleaseFn,uint32_t (BC_CALL *)(const BC_Ref*)>);
static_assert(std::is_same_v<BC_WithdrawFn,uint32_t (BC_CALL *)(const BC_Ref*)>);
static_assert(std::is_same_v<BC_WithdrawAllFn,uint32_t (BC_CALL *)(const BC_Ref*)>);
static_assert(std::is_same_v<BC_CloseFn,uint32_t (BC_CALL *)(const BC_Ref*)>);
static_assert(std::is_same_v<BC_GetPolicyFn,uint32_t (BC_CALL *)(const BC_Ref*,BC_Policy*)>);

namespace
{
    using TileKey=std::pair<int64_t,int64_t>;
    // Consumer bookkeeping/admission bounds, not provider VRAM policy. An
    // oversized rectangle is rejected whole, never silently truncated.
    constexpr size_t kMaxTiles=4096;
    constexpr unsigned kServiceCalls=32;
    struct Tile final
    {
        BC_Demand demand{};
        uint64_t request=0;
        BC_Status status{},publication{};
        BC_Lease lease{}; // Opaque borrowed Texture; only Release retires this use.
        uint32_t requestResult=BC_UNAVAILABLE,publicationResult=BC_UNAVAILABLE,acquireResult=BC_UNAVAILABLE;
        uint64_t blockedPublication=0,blockedReadiness=0;
        bool wanted=false,available=false,submitFailed=false,releasePending=false;
    };
    struct Consumer final
    {
        BC_Api api{};
        BC_Grid grid{}; // Provider identity, never MainMap's epoch.
        MainMapBuildingLayer::ConsumerContext context{};
        BC_Policy policy{}; // Valid only for grid.Session / ABI v1.
        std::map<TileKey,Tile> tiles;
        TileKey serviceAfter{},cleanupAfter{};
        bool serviceCursor=false,cleanupCursor=false;
        uint64_t nextDemand=1,detailEpoch=0;
        uint32_t side=0;
        bool closing=false;
        bool available=false;
        bool stopped=false;
        bool discovered=false;
        bool policyRejected=false;
    } g_consumer;
    std::atomic_bool g_closeRequested{false};

    bool ValidateGrid(const BC_Grid& grid,const MainMapBuildingLayer::ConsumerContext& context)
    {
        // RestoreEpoch is an opaque value compared on every inspection, not
        // MainMap's epoch. The ABI does not reserve zero for this counter.
        return grid.StructSize==sizeof(BC_Grid) && grid.Version==BC_VERSION && grid.ContractId==BC_CONTRACT_ID &&
            grid.Session && grid.WorldGeneration && grid.GridId &&
            grid.OriginX==context.originX && grid.OriginY==context.originY &&
            grid.CellSizeCm==12500.0 && grid.TileSizeCm==50000.0 && grid.CellPixels==256 &&
            grid.GroupWidth==4 && grid.MaxInteriorSide==1024 && grid.PixelConvention==BC_PIXEL_CONVENTION &&
            (grid.AdmissionFlags & ~15u)==0 && (grid.AdmissionFlags & 1u)!=0 && (grid.AdmissionFlags & 12u)==0;
    }

    bool SameIdentity(const BC_Grid& a,const BC_Grid& b)
    {
        return a.Session==b.Session && a.WorldGeneration==b.WorldGeneration &&
            a.RestoreEpoch==b.RestoreEpoch && a.GridId==b.GridId;
    }

    bool Temporary(uint32_t result)
    {
        return result==BC_REJECTED_THREAD_OR_ADMISSION || result==BC_UNAVAILABLE || result==BC_BUSY;
    }

    bool ValidatePolicy(const BC_Policy& p)
    {
        // BC_Policy has no Version field. Its shape and PolicyId are interpreted
        // only under the negotiated BCT1 v1 table. R2 is deliberately unsupported.
        if(p.StructSize!=sizeof(p)||p.PolicyId!=1||!p.ClassCount||p.ClassCount>8||
            p.Gutter!=1||p.MaxInteriorSide!=1024||p.Flags||p.DowngradeFraction!=0.40)return false;
        uint32_t previous=0;
        for(uint32_t i=0;i<8;++i)
        {
            const auto n=p.Sides[i];
            if(i>=p.ClassCount) { if(n)return false;continue; }
            if((n!=128&&n!=256&&n!=512&&n!=1024)||n<=previous)return false;
            previous=n;
        }
        return previous==p.MaxInteriorSide;
    }

    void MakeUnavailable(Tile& tile)
    {
        tile.available=false;
    }

    bool PrepareRelease(Tile& tile)
    {
        MakeUnavailable(tile);
        tile.releasePending=true;
        // Future presentation must detach all UI/material uses HERE and return
        // false while detachment is unresolved. All Release paths use this gate.
        // Phase 2 never binds or dereferences Texture, so nothing is attached.
        return true;
    }

    void StopDemand()
    {
        auto& owner=g_consumer;
        owner.available=false;
        for(auto& [key,tile]:owner.tiles) { (void)key;tile.wanted=false;MakeUnavailable(tile); }
    }

    // Every rejected cleanup keeps the exact issued tokens. Withdraw never
    // substitutes for Release, including on a revoked world/restore session.
    void CleanupTiles(unsigned& calls)
    {
        auto& owner=g_consumer;
        auto it=owner.cleanupCursor?owner.tiles.upper_bound(owner.cleanupAfter):owner.tiles.begin();
        if(it==owner.tiles.end())it=owner.tiles.begin();
        size_t remaining=owner.tiles.size();
        while(remaining-- && !owner.tiles.empty() && calls)
        {
            if(it==owner.tiles.end())it=owner.tiles.begin();
            auto current=it++;
            owner.cleanupAfter=current->first;owner.cleanupCursor=true;
            auto& tile=current->second;
            if(!tile.wanted || tile.releasePending)
            {
                MakeUnavailable(tile);
                if(tile.lease.LeaseId)
                {
                    if(!PrepareRelease(tile))continue;
                    const BC_Ref lease{sizeof(BC_Ref),0,tile.lease.Session,tile.lease.LeaseId};
                    --calls;
                    if(owner.api.Release(&lease)!=BC_RESULT_OK)continue;
                    tile.lease={};tile.releasePending=false;
                }
                if(tile.wanted)continue; // Release debt never cancels visible interest.
                if(tile.request)
                {
                    if(!calls)continue;
                    const BC_Ref request{sizeof(BC_Ref),0,owner.grid.Session,tile.request};
                    --calls;
                    if(owner.api.Withdraw(&request)!=BC_RESULT_OK)continue;
                    tile.request=0;
                }
                owner.tiles.erase(current);
            }
        }
    }

    void SuspendTiles()
    {
        auto& owner=g_consumer;
        owner.available=false;
        for(auto& [key,tile]:owner.tiles) { (void)key;MakeUnavailable(tile); }
        // Cleanup depends on admitted ownership, not drawable layout/source
        // readiness. Retry existing release/withdraw debt during suspension too.
        unsigned calls=kServiceCalls;CleanupTiles(calls);
    }

    struct TileRange { int64_t minX=0,minY=0,maxX=0,maxY=0; };
    bool VisibleRange(const MainMapBuildingLayer::WorldRect& r,TileRange& out)
    {
        const auto& grid=g_consumer.grid;
        if(!std::isfinite(r.minX)||!std::isfinite(r.minY)||!std::isfinite(r.maxX)||!std::isfinite(r.maxY)||
            r.minX>=r.maxX||r.minY>=r.maxY)return false;
        const long double firstX=std::floor((static_cast<long double>(r.minX)-grid.OriginX)/grid.TileSizeCm);
        const long double firstY=std::floor((static_cast<long double>(r.minY)-grid.OriginY)/grid.TileSizeCm);
        const long double lastX=std::ceil((static_cast<long double>(r.maxX)-grid.OriginX)/grid.TileSizeCm)-1;
        const long double lastY=std::ceil((static_cast<long double>(r.maxY)-grid.OriginY)/grid.TileSizeCm)-1;
        // Match the provider's checked canonical-cell multiplication domain.
        constexpr int64_t low=(std::numeric_limits<int64_t>::min)()/4;
        constexpr int64_t high=((std::numeric_limits<int64_t>::max)()-3)/4;
        if(!std::isfinite(firstX)||!std::isfinite(firstY)||!std::isfinite(lastX)||!std::isfinite(lastY)||
            firstX<low||firstY<low||lastX>high||lastY>high||lastX<firstX||lastY<firstY)return false;
        const long double width=lastX-firstX+1,height=lastY-firstY+1;
        if(width>kMaxTiles||height>kMaxTiles||width*height>kMaxTiles)return false;
        // Reject coordinates where floating-point bounds collapse. The provider
        // uses these same double world bounds, not integer-only tile positions.
        const long double keys[4]={firstX,lastX,firstY,lastY};
        for(int i=0;i<4;++i)
        {
            const long double origin=i<2?grid.OriginX:grid.OriginY;
            const double a=static_cast<double>(origin+keys[i]*50000.0L);
            const double b=static_cast<double>(origin+keys[i]*50000.0L+50000.0L);
            if(!std::isfinite(a)||!std::isfinite(b)||a>=b)return false;
        }
        out={static_cast<int64_t>(firstX),static_cast<int64_t>(firstY),static_cast<int64_t>(lastX),static_cast<int64_t>(lastY)};
        return true;
    }

    uint32_t SelectSide(double pixels)
    {
        const auto& owner=g_consumer;
        auto initial=owner.policy.Sides[owner.policy.ClassCount-1];
        for(uint32_t i=0;i<owner.policy.ClassCount;++i)
            if(owner.policy.Sides[i]>=pixels) { initial=owner.policy.Sides[i];break; }
        if(!owner.side||pixels>owner.side||pixels<=owner.policy.DowngradeFraction*owner.side)return initial;
        return owner.side;
    }

    bool ValidateStatus(const BC_Status& s,const Tile& tile,bool publication)
    {
        const auto& grid=g_consumer.grid;
        const auto& d=tile.demand;
        const double x=static_cast<double>(static_cast<long double>(grid.OriginX)+static_cast<long double>(d.TileX)*50000.0L);
        const double y=static_cast<double>(static_cast<long double>(grid.OriginY)+static_cast<long double>(d.TileY)*50000.0L);
        const double maxX=static_cast<double>(static_cast<long double>(grid.OriginX)+static_cast<long double>(d.TileX)*50000.0L+50000.0L);
        const double maxY=static_cast<double>(static_cast<long double>(grid.OriginY)+static_cast<long double>(d.TileY)*50000.0L+50000.0L);
        if(s.StructSize!=sizeof(s)||s.State<BC_PENDING||s.State>BC_STALE||(s.Flags&~127u)||
            s.Reason>BC_STALE_OR_REVOKED_IDENTITY||s.WorldGeneration!=grid.WorldGeneration||
            s.RestoreEpoch!=grid.RestoreEpoch||s.GridId!=grid.GridId||s.TileX!=d.TileX||s.TileY!=d.TileY||
            s.InteriorSide!=d.InteriorSide||s.WorldBounds.MinX!=x||s.WorldBounds.MinY!=y||
            s.WorldBounds.MaxX!=maxX||s.WorldBounds.MaxY!=maxY||x>=maxX||y>=maxY)return false;
        return publication ? s.PublicationId==tile.status.PublicationId :
            s.RequestId==tile.request&&s.DetailEpoch==d.DetailEpoch;
    }

    bool Drawable(const BC_Status& s)
    {
        return s.State==BC_READY && (s.Flags&(BC_CURRENT|BC_NATIVE_READY|BC_COMPLETE))==
            (BC_CURRENT|BC_NATIVE_READY|BC_COMPLETE) && !(s.Flags&BC_HAS_FAILURE) &&
            s.PublicationId && s.CertificateId && s.Gutter==1 &&
            s.ReadyCells==s.RequiredCells && !s.PendingCells && !s.FailedCells &&
            s.TextureWidth==s.InteriorSide+2 && s.TextureHeight==s.InteriorSide+2 &&
            s.UVScaleX==double(s.InteriorSide)/double(s.InteriorSide+2) && s.UVScaleY==s.UVScaleX &&
            s.UVBiasX==1.0/double(s.InteriorSide+2) && s.UVBiasY==s.UVBiasX;
    }

    void ServiceTiles(unsigned& calls)
    {
        auto& owner=g_consumer;
        auto it=owner.serviceCursor?owner.tiles.upper_bound(owner.serviceAfter):owner.tiles.begin();
        if(it==owner.tiles.end())it=owner.tiles.begin();
        size_t remaining=owner.tiles.size();
        // Reserve enough operations to finish inspection -> acquisition in one
        // admitted visit. The rotating cursor prevents large views starving.
        while(remaining-- && it!=owner.tiles.end() && calls>=3)
        {
            auto& tile=it->second;
            owner.serviceAfter=it->first;owner.serviceCursor=true;++it;
            if(it==owner.tiles.end() && remaining)it=owner.tiles.begin();
            if(!tile.wanted)continue;
            if(!tile.request)
            {
                if(tile.submitFailed)continue;
                if(owner.nextDemand==(std::numeric_limits<uint64_t>::max)())
                { MainMapBuildingComplex::RequestClose();return; }
                // IDs follow actual call order, not dictionary insertion or
                // cursor order. Non-OK Submit accepts no interest in BCT1 v1.
                tile.demand.ClientDemandId=owner.nextDemand++;
                uint64_t request=0;--calls;
                const auto result=owner.api.Submit(&tile.demand,&request);
                tile.requestResult=result;
                if(result==BC_RESULT_OK && request)tile.request=request;
                else if(!Temporary(result))tile.submitFailed=true;
                if(result==BC_STALE_SESSION||result==BC_CLOSED) { MainMapBuildingComplex::RequestClose();return; }
                continue;
            }
            BC_Status status{};status.StructSize=sizeof(status);
            const BC_Ref request{sizeof(BC_Ref),0,owner.grid.Session,tile.request};
            --calls;
            const auto result=owner.api.InspectRequest(&request,&status);
            tile.requestResult=result;
            MakeUnavailable(tile);
            if(result!=BC_RESULT_OK)
            {
                if(result==BC_STALE_SESSION||result==BC_CLOSED) { MainMapBuildingComplex::RequestClose();return; }
                // A temporary failure grants no currentness. Release any prior
                // use through the same boundary; preserve its request interest.
                if(tile.lease.LeaseId)
                {
                    if(!PrepareRelease(tile))continue;
                    const BC_Ref lease{sizeof(BC_Ref),0,tile.lease.Session,tile.lease.LeaseId};
                    --calls;if(owner.api.Release(&lease)==BC_RESULT_OK) { tile.lease={};tile.releasePending=false; }
                }
                continue;
            }
            if(!ValidateStatus(status,tile,false)) { MainMapBuildingComplex::RequestClose();return; }
            tile.status=status;
            // Inspection is observational. Provider ServiceRequests alone owns
            // SourceRevision/DependencyRevision retry gates (RETRY_ELIGIBLE).
            // Never withdraw/re-submit Failed/Partial to bypass those gates.
            bool ready=Drawable(status);
            if(ready)
            {
                BC_Status publication{};publication.StructSize=sizeof(publication);
                const BC_Ref pub{sizeof(BC_Ref),0,owner.grid.Session,status.PublicationId};
                --calls;
                const auto inspected=owner.api.InspectPublication(&pub,&publication);
                tile.publicationResult=inspected;
                ready=inspected==BC_RESULT_OK&&ValidateStatus(publication,tile,true)&&Drawable(publication)&&
                    publication.CertificateId==status.CertificateId;
                if(ready)tile.publication=publication;
                if(inspected==BC_STALE_SESSION||inspected==BC_CLOSED) { MainMapBuildingComplex::RequestClose();return; }
            }
            if(tile.lease.LeaseId && (tile.releasePending||!ready||tile.lease.PublicationId!=status.PublicationId))
            {
                if(!PrepareRelease(tile))continue;
                const BC_Ref lease{sizeof(BC_Ref),0,tile.lease.Session,tile.lease.LeaseId};
                --calls;if(owner.api.Release(&lease)==BC_RESULT_OK) { tile.lease={};tile.releasePending=false; }
                continue; // Never acquire a replacement before release resolves.
            }
            if(!ready)continue; // Pending/Empty/Partial/Failed/Stale are not acquired.
            if(!tile.lease.LeaseId)
            {
                if(tile.blockedPublication==status.PublicationId&&tile.blockedReadiness==status.ReadinessRevision)continue;
                BC_Lease lease{};lease.StructSize=sizeof(lease);
                const BC_Ref pub{sizeof(BC_Ref),0,owner.grid.Session,status.PublicationId};
                --calls;
                tile.acquireResult=owner.api.Acquire(&pub,&lease);
                if(tile.acquireResult!=BC_RESULT_OK)
                {
                    if(tile.acquireResult==BC_STALE_SESSION||tile.acquireResult==BC_CLOSED)
                    { MainMapBuildingComplex::RequestClose();return; }
                    if(!Temporary(tile.acquireResult))
                    { tile.blockedPublication=status.PublicationId;tile.blockedReadiness=status.ReadinessRevision; }
                    continue;
                }
                tile.lease=lease; // Record ownership BEFORE validating metadata.
                if(lease.StructSize!=sizeof(lease)||lease.Reserved||lease.Session!=owner.grid.Session||
                    !lease.LeaseId||lease.PublicationId!=status.PublicationId||!lease.Texture)
                { MainMapBuildingComplex::RequestClose();return; }
            }
            tile.available=true;
        }
    }

    void ReconcileVisible()
    {
        auto& owner=g_consumer;
        MainMapBuildingLayer::WorldRect visible{};TileRange range{};double pixels=0;
        if(!MainMapBuildingLayer::TryGetVisibleWorldRect(visible)||
            !MainMapBuildingLayer::TryGetGroupedTilePixelSpan(pixels)||!VisibleRange(visible,range))
        {
            // Layout failure never means Empty. Retain interests, suspend new
            // demand/acquisition and clear consumer availability.
            SuspendTiles();
            return;
        }
        const auto side=SelectSide(pixels);
        if(side!=owner.side)
        {
            if(owner.detailEpoch==(std::numeric_limits<uint64_t>::max)()) { MainMapBuildingComplex::RequestClose();return; }
            ++owner.detailEpoch;owner.side=side;
        }
        for(auto& [key,tile]:owner.tiles)
        {
            tile.wanted=key.first>=range.minX&&key.first<=range.maxX&&key.second>=range.minY&&key.second<=range.maxY&&
                tile.demand.InteriorSide==side&&tile.demand.DetailEpoch==owner.detailEpoch;
            if(!tile.wanted)MakeUnavailable(tile);
        }
        unsigned cleanup=kServiceCalls/2;CleanupTiles(cleanup);
        // Allocate at most one service batch per tick; old rejected ownership
        // consumes capacity until acknowledged. No offscreen request is added.
        unsigned added=0;
        for(int64_t y=range.minY;y<=range.maxY&&added<kServiceCalls&&owner.tiles.size()<kMaxTiles;++y)
            for(int64_t x=range.minX;x<=range.maxX&&added<kServiceCalls&&owner.tiles.size()<kMaxTiles;++x)
            {
                if(owner.tiles.contains({x,y}))continue;
                Tile tile;tile.wanted=true;
                tile.demand={sizeof(BC_Demand),side,owner.grid.Session,0,x,y,1,0,owner.grid.GridId,owner.detailEpoch};
                try { owner.tiles.emplace(TileKey{x,y},tile);++added; }
                // Preserve progress inspecting existing visible ownership even
                // if allocating bookkeeping for a new tile fails.
                catch(...) { added=kServiceCalls;break; }
            }
        unsigned calls=kServiceCalls-kServiceCalls/2+cleanup;
        ServiceTiles(calls);
    }

    void ServiceCloseWithBudget(unsigned calls)
    {
        auto& owner=g_consumer;
        if(g_closeRequested.exchange(false)) { owner.closing=true;StopDemand(); }
        if(owner.stopped || !owner.closing || !MainMapNative::CanOperate())return;
        if(!owner.grid.Session) { owner.closing=false;return; }
        CleanupTiles(calls);
        if(!owner.tiles.empty())return;
        const BC_Ref ref{sizeof(BC_Ref),0,owner.grid.Session,0};
        if(owner.api.WithdrawAll(&ref)!=BC_RESULT_OK)return;
        // Close OK alone acknowledges closure; every rejection retains ownership.
        if(owner.api.Close(&ref)!=BC_RESULT_OK)return;
        owner.grid={};owner.context={};owner.policy={};owner.closing=false;
        owner.side=0;owner.detailEpoch=0;owner.nextDemand=1;
        owner.serviceCursor=false;owner.cleanupCursor=false;
    }

    void Discover()
    {
        auto& owner=g_consumer;
        if(owner.discovered)return;
        owner.discovered=true;
        // Borrowed module only. Startup-only policy supplies logical lifetime;
        // no Windows reference/pin can enforce that policy or prevent shutdown.
        const auto module=GetModuleHandleW(L"MiniMap.dll");
        if(!module)return;
        const auto getter=reinterpret_cast<Getter>(GetProcAddress(module,"MiniMap_GetBuildingComplexApi"));
        BC_Api api{};api.StructSize=sizeof(api);
        if(!getter || getter(BC_VERSION,&api)!=BC_RESULT_OK || api.StructSize!=104 ||
            api.Version!=1 || api.ContractId!=0x42435431u || api.Reserved ||
            !api.Open || !api.InspectSession || !api.Submit || !api.InspectRequest || !api.InspectPublication ||
            !api.Acquire || !api.Release || !api.Withdraw || !api.WithdrawAll || !api.Close || !api.GetPolicy)return;
        owner.api=api;
    }
}

namespace MainMapBuildingComplex
{
    void Initialize()
    {
        // Initialized once per startup. Never replace unresolved ownership.
        if(g_consumer.grid.Session || g_consumer.stopped)return;
        g_closeRequested.store(false);
    }

    void RequestClose() { g_closeRequested.store(true); }

    void ServiceClose()
    {
        ServiceCloseWithBudget(kServiceCalls);
    }

    void Tick()
    {
        auto& owner=g_consumer;
        if(owner.stopped)return;
        owner.available=false;
        if(owner.closing||g_closeRequested.load()) { ServiceClose();return; }
        if(!MainMapNative::CanOperate() || owner.closing || owner.policyRejected)return;
        Discover(); // First admitted tick, after startup plugin initialization.
        if(!owner.api.Open)return;
        MainMapBuildingLayer::ConsumerContext context{};
        if(!MainMapBuildingLayer::TryGetConsumerContext(context))
        {
            RequestClose();ServiceClose();return;
        }
        if(owner.grid.Session)
        {
            if(context.world!=owner.context.world || context.epoch!=owner.context.epoch ||
                context.originX!=owner.context.originX || context.originY!=owner.context.originY)
            {
                RequestClose();ServiceClose();return;
            }
            BC_Grid inspected{};inspected.StructSize=sizeof(inspected);
            const BC_Ref ref{sizeof(BC_Ref),0,owner.grid.Session,0};
            const auto result=owner.api.InspectSession(&ref,&inspected);
            owner.available=false;
            if(Temporary(result)) { SuspendTiles();return; }
            if(result!=BC_RESULT_OK || !ValidateGrid(inspected,context) || !SameIdentity(owner.grid,inspected))
            {
                RequestClose();ServiceClose();return;
            }
            owner.grid=inspected;
            owner.available=(inspected.AdmissionFlags & 2u)!=0;
        }
        else
        {
            BC_Open request{};request.StructSize=sizeof(request);
            request.World=context.world;request.ClientWorldEpoch=context.epoch;
            request.OriginX=context.originX;request.OriginY=context.originY;
            request.CellSizeCm=12500.0;request.CellPixels=256;request.GroupWidth=4;request.GridSchema=BC_SCHEMA;
            BC_Grid opened{};opened.StructSize=sizeof(opened);
            if(owner.api.Open(&request,&opened)!=BC_RESULT_OK)return;
            // Record ownership before validating success metadata. An incompatible
            // descriptor is not permission to leak an issued session.
            owner.grid=opened;owner.context=context;
            if(!ValidateGrid(opened,context))
            {
                RequestClose();ServiceClose();
                // Success without an issued token is a broken provider contract.
                if(!opened.Session) { owner.api={};owner.discovered=true; }
                return;
            }
            owner.available=(opened.AdmissionFlags & 2u)!=0;
        }
        if(!owner.policy.PolicyId)
        {
            BC_Policy policy{};policy.StructSize=sizeof(policy);
            const BC_Ref ref{sizeof(BC_Ref),0,owner.grid.Session,0};
            const auto result=owner.api.GetPolicy(&ref,&policy);
            if(Temporary(result)) { owner.available=false;return; }
            if(result!=BC_RESULT_OK||!ValidatePolicy(policy))
            {
                if(result==BC_RESULT_OK||result==BC_VERSION_MISMATCH)owner.policyRejected=true;
                RequestClose();ServiceClose();return;
            }
            owner.policy=policy;
        }
        if(owner.available)ReconcileVisible();
        else SuspendTiles();
        // A failed identity or exhaustion can request closure during servicing.
        if(g_closeRequested.load())ServiceClose();
    }

    void Shutdown(bool earlyEngineShutdown)
    {
        auto& owner=g_consumer;
        if(owner.stopped)return;
        RequestClose();
        // The early engine callback precedes loader PluginShutdown/DLL teardown.
        // At late PluginShutdown the provider's teardown order is unspecified.
        // One finite visit per record; no retry loop or wait at final teardown.
        if(earlyEngineShutdown)ServiceCloseWithBudget(static_cast<unsigned>(2*kMaxTiles));
        StopDemand();
        owner.available=false;owner.stopped=true;
        owner.api={}; // No future export invocation, even with unresolved debt.
        if(owner.grid.Session)
        {
            size_t leases=0;
            for(const auto& [key,tile]:owner.tiles) { (void)key;if(tile.lease.LeaseId)++leases; }
            LOG_ERROR("MainMap BCT1: session %llu closure unacknowledged at shutdown; %zu tile records / %zu leases retained, no later provider calls",
                static_cast<unsigned long long>(owner.grid.Session),owner.tiles.size(),leases);
        }
    }
}
