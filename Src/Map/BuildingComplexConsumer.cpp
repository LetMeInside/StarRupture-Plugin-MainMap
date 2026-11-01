#include "BuildingComplexConsumer.h"
#include "BuildingLayer.h"
#include "Native/NativeApi.h"
#include "plugin_helpers.h"
#include "..\..\..\..\MiniMap\Solution\Src\Map\Buildings\BuildingComplexAbi.h"
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <type_traits>

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
    struct Consumer final
    {
        BC_Api api{};
        BC_Grid grid{}; // Provider identity, never MainMap's epoch.
        MainMapBuildingLayer::ConsumerContext context{};
        bool closing=false;
        bool available=false;
        bool stopped=false;
        bool discovered=false;
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
        auto& owner=g_consumer;
        if(g_closeRequested.exchange(false)) { owner.closing=true;owner.available=false; }
        if(owner.stopped || !owner.closing || !MainMapNative::CanOperate())return;
        if(!owner.grid.Session) { owner.closing=false;return; }
        const BC_Ref ref{sizeof(BC_Ref),0,owner.grid.Session,0};
        // Phase 1 issues no interests or leases. Keep this order for the later
        // consumer; Phase 2 must detach/release every UI lease before withdrawal.
        owner.api.WithdrawAll(&ref);
        // Close itself withdraws interest. Only OK acknowledges ownership transfer;
        // BUSY and every rejection preserve the exact old token for later ticks.
        if(owner.api.Close(&ref)!=BC_RESULT_OK)return;
        owner.grid={};owner.context={};owner.closing=false;
    }

    void Tick()
    {
        auto& owner=g_consumer;
        if(owner.stopped)return;
        owner.available=false;
        ServiceClose();
        if(!MainMapNative::CanOperate() || owner.closing)return;
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
            if(result==BC_REJECTED_THREAD_OR_ADMISSION || result==BC_UNAVAILABLE || result==BC_BUSY)return;
            if(result!=BC_RESULT_OK || !ValidateGrid(inspected,context) || !SameIdentity(owner.grid,inspected))
            {
                RequestClose();ServiceClose();return;
            }
            owner.grid=inspected;
            owner.available=(inspected.AdmissionFlags & 2u)!=0;
            return;
        }
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

    void Shutdown(bool earlyEngineShutdown)
    {
        auto& owner=g_consumer;
        if(owner.stopped)return;
        RequestClose();
        // The early engine callback precedes loader PluginShutdown/DLL teardown.
        // At late PluginShutdown the provider's teardown order is unspecified.
        if(earlyEngineShutdown)ServiceClose();
        owner.available=false;owner.stopped=true;
        owner.api={}; // No future export invocation, even with unresolved debt.
        if(owner.grid.Session)
            LOG_ERROR("MainMap BCT1: session %llu closure unacknowledged at shutdown; identity retained, no later provider calls",
                static_cast<unsigned long long>(owner.grid.Session));
    }
}
