#include "BuildingComplexConsumer.h"
#include "BuildingLayer.h"
#include "Native/NativeApi.h"
#include "MaterialProof.h"
#include "plugin_helpers.h"
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"
#include "SlateCore_structs.hpp"
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
BC_LAYOUT(BC_Open,56); BC_LAYOUT(BC_Grid,80); BC_LAYOUT(BC_Demand,64);
BC_LAYOUT(BC_Status,192); BC_LAYOUT(BC_Lease,40); BC_LAYOUT(BC_Policy,64); BC_LAYOUT(BC_Api,112);
#undef BC_LAYOUT
static_assert(BC_VERSION==1 && BC_CONTRACT_ID==0x42435431u);
static_assert(sizeof(void*)==8);
static_assert(offsetof(BC_Key,Y)==8 && offsetof(BC_Rect,MaxY)==24);
static_assert(offsetof(BC_Ref,Session)==8 && offsetof(BC_Ref,Id)==16);
static_assert(offsetof(BC_Open,World)==8 && offsetof(BC_Open,OriginX)==16 && offsetof(BC_Open,GridSchema)==48);
static_assert(offsetof(BC_Grid,Session)==16 && offsetof(BC_Grid,GridId)==24 && offsetof(BC_Grid,OriginX)==32 && offsetof(BC_Grid,CellPixels)==64);
static_assert(offsetof(BC_Demand,Session)==8 && offsetof(BC_Demand,TileX)==24 && offsetof(BC_Demand,GridId)==48);
static_assert(offsetof(BC_Status,RequestId)==16 && offsetof(BC_Status,PublicationId)==24 &&
    offsetof(BC_Status,GridId)==32 && offsetof(BC_Status,TileX)==56 && offsetof(BC_Status,WorldBounds)==72 && offsetof(BC_Status,InteriorSide)==104 &&
    offsetof(BC_Status,UVScaleX)==120 && offsetof(BC_Status,ReadinessRevision)==152 && offsetof(BC_Status,FailedCells)==184);
static_assert(offsetof(BC_Lease,Session)==8 && offsetof(BC_Lease,LeaseId)==16 && offsetof(BC_Lease,PublicationId)==24 && offsetof(BC_Lease,Texture)==32);
static_assert(offsetof(BC_Policy,Sides)==16 && offsetof(BC_Policy,DowngradeFraction)==48 && offsetof(BC_Policy,MaxInteriorSide)==56);
static_assert(offsetof(BC_Api,Open)==16 && offsetof(BC_Api,InspectSession)==24 && offsetof(BC_Api,Submit)==32 &&
    offsetof(BC_Api,InspectRequest)==40 && offsetof(BC_Api,InspectPublication)==48 && offsetof(BC_Api,Acquire)==56 &&
    offsetof(BC_Api,Release)==64 && offsetof(BC_Api,Withdraw)==72 && offsetof(BC_Api,WithdrawAll)==80 &&
    offsetof(BC_Api,Close)==88 && offsetof(BC_Api,GetPolicy)==96 && offsetof(BC_Api,Service)==104);
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
static_assert(std::is_same_v<BC_ServiceFn,uint32_t (BC_CALL *)(const BC_Ref*)>);

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
        uint64_t diagnosticSignature=(std::numeric_limits<uint64_t>::max)();
        uint32_t bindDiagnostic=(std::numeric_limits<uint32_t>::max)();
        MainMapNative::StrongObjectLease imageOwner,materialOwner,textureOwner;
        SDK::UImage* image=nullptr;
        SDK::UMaterialInstanceDynamic* material=nullptr;
        bool attached=false,brushBound=false,presented=false,visible=false,midBound=false,geometryValid=false;
        bool wanted=false,available=false,submitFailed=false,releasePending=false;
    };
    struct Consumer final
    {
        BC_Api api{};
        BC_Grid grid{}; // Provider identity, never MainMap's epoch.
        MainMapBuildingLayer::ConsumerContext context{};
        BC_Policy policy{}; // Valid only for grid.Session / ABI v1.
        MainMapNative::StrongObjectLease presentParentOwner;
        SDK::UMaterial* presentParent=nullptr;
        std::map<TileKey,Tile> tiles;
        TileKey serviceAfter{},cleanupAfter{};
        TileKey diagnosticTile{};
        bool hasDiagnosticTile=false,diagnosticTileSelected=false,diagnosticTileCompleted=false;
        bool serviceCursor=false,cleanupCursor=false;
        uint64_t nextDemand=1,detailEpoch=0;
        uint32_t side=0;
        bool closing=false;
        bool available=false;
        bool stopped=false;
        bool discovered=false;
        bool policyRejected=false;
        bool presentationRejected=false;
        bool presentationVisible=false;
        uint32_t diagnosticOpenResult=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticInspectResult=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticPolicyResult=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticWithdrawResult=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticCloseResult=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticContextState=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticMaterialsMounted=(std::numeric_limits<uint32_t>::max)();
        uint32_t diagnosticVisibleGate=(std::numeric_limits<uint32_t>::max)();
    } g_consumer;
    std::atomic_bool g_closeRequested{false};

    void LogProviderResult(const char* stage,uint32_t result,uint32_t& previous)
    {
        if(previous==result)return;
        previous=result;
        LOG_INFO("MainMap diagnostics: BCT1 %s result=%u",stage,result);
    }

    void LogVisibleGate(uint32_t state,const char* reason)
    {
        auto& owner=g_consumer;
        if(owner.diagnosticVisibleGate==state)return;
        owner.diagnosticVisibleGate=state;
        LOG_INFO("MainMap diagnostics: BCT1 visible tile demand=%s reason=%s",
            state==0?"admitted":"suspended",reason);
    }

    bool IsDiagnosticTile(const Tile& tile) noexcept
    {
        return g_consumer.hasDiagnosticTile && tile.demand.TileX==g_consumer.diagnosticTile.first &&
            tile.demand.TileY==g_consumer.diagnosticTile.second;
    }

    void LogTileStatusChange(Tile& tile,const char* boundary)
    {
        if(!IsDiagnosticTile(tile))return;
        uint64_t signature=1469598103934665603ULL;
        const auto add=[&](uint64_t value) { signature^=value;signature*=1099511628211ULL; };
        add(tile.demand.InteriorSide);add(tile.demand.ClientDemandId);add(tile.request);
        add(tile.requestResult);add(tile.status.State);add(tile.status.Reason);add(tile.status.Flags);
        add(tile.status.ReadinessRevision);add(tile.status.PublicationId);
        add(tile.status.RequiredCells);add(tile.status.ReadyCells);add(tile.status.PendingCells);add(tile.status.FailedCells);
        add(tile.publicationResult);add(tile.publication.State);add(tile.publication.Reason);add(tile.publication.Flags);
        add(tile.publication.PublicationId);add(tile.acquireResult);add(tile.lease.LeaseId);
        if(signature==tile.diagnosticSignature)return;
        tile.diagnosticSignature=signature;
        LOG_INFO("MainMap diagnostics: BCT1 tile boundary=%s xy=%lld,%lld side=%u demand=%llu request=%llu submitOrInspect=%u state=%u reason=%u flags=0x%X cells=%llu/%llu pending=%llu failed=%llu publication=%llu publicationResult=%u publicationState=%u publicationReason=%u acquireResult=%u lease=%llu",
            boundary,static_cast<long long>(tile.demand.TileX),static_cast<long long>(tile.demand.TileY),tile.demand.InteriorSide,
            static_cast<unsigned long long>(tile.demand.ClientDemandId),static_cast<unsigned long long>(tile.request),tile.requestResult,
            tile.status.State,tile.status.Reason,tile.status.Flags,static_cast<unsigned long long>(tile.status.ReadyCells),
            static_cast<unsigned long long>(tile.status.RequiredCells),static_cast<unsigned long long>(tile.status.PendingCells),
            static_cast<unsigned long long>(tile.status.FailedCells),static_cast<unsigned long long>(tile.status.PublicationId),
            tile.publicationResult,tile.publication.State,tile.publication.Reason,tile.acquireResult,
            static_cast<unsigned long long>(tile.lease.LeaseId));
    }

    void LogBindState(Tile& tile,uint32_t state,const char* reason)
    {
        if(!IsDiagnosticTile(tile)||tile.bindDiagnostic==state)return;
        tile.bindDiagnostic=state;
        LOG_INFO("MainMap diagnostics: BCT1 BindPresentation xy=%lld,%lld side=%u demand=%llu request=%llu publication=%llu result=%s image=%u attached=%u brushMID=%u textureBound=%u geometryValid=%u visible=%u",
            static_cast<long long>(tile.demand.TileX),static_cast<long long>(tile.demand.TileY),tile.demand.InteriorSide,
            static_cast<unsigned long long>(tile.demand.ClientDemandId),static_cast<unsigned long long>(tile.request),
            static_cast<unsigned long long>(tile.publication.PublicationId),reason,unsigned(tile.image!=nullptr),
            unsigned(tile.attached),unsigned(tile.brushBound),unsigned(tile.midBound),
            unsigned(tile.geometryValid),unsigned(tile.visible));
    }

    bool ValidateGrid(const BC_Grid& grid,const MainMapBuildingLayer::ConsumerContext& context)
    {
        return grid.StructSize==sizeof(BC_Grid) && grid.Version==BC_VERSION && grid.ContractId==BC_CONTRACT_ID &&
            grid.Session && grid.GridId &&
            grid.OriginX==context.originX && grid.OriginY==context.originY &&
            grid.CellSizeCm==12500.0 && grid.TileSizeCm==50000.0 && grid.CellPixels==256 &&
            grid.GroupWidth==4 && grid.MaxInteriorSide==1024 && grid.PixelConvention==BC_PIXEL_CONVENTION;
    }

    bool SameIdentity(const BC_Grid& a,const BC_Grid& b)
    {
        return a.Session==b.Session && a.GridId==b.GridId;
    }

    bool Temporary(uint32_t result)
    {
        return result==BC_REJECTED_THREAD_OR_ADMISSION || result==BC_UNAVAILABLE || result==BC_BUSY;
    }

    bool ServiceProvider()
    {
        auto& owner=g_consumer;
        if(!owner.available||!owner.grid.Session||owner.closing||g_closeRequested.load()||!owner.api.Service)return false;
        const BC_Ref ref{sizeof(BC_Ref),0,owner.grid.Session,0};
        const auto result=owner.api.Service(&ref);
        if(result==BC_STALE_SESSION||result==BC_CLOSED||(!Temporary(result)&&result!=BC_RESULT_OK))
            MainMapBuildingComplex::RequestClose();
        return result==BC_RESULT_OK;
    }

    bool FiniteColor(const SDK::FLinearColor& c)
    { return std::isfinite(c.R)&&std::isfinite(c.G)&&std::isfinite(c.B)&&std::isfinite(c.A); }

    void SetTileVisible(Tile& tile,bool visible)
    {
        if(tile.image&&tile.visible!=visible)
        {
            tile.image->SetVisibility(visible?SDK::ESlateVisibility::HitTestInvisible:SDK::ESlateVisibility::Collapsed);
            tile.visible=visible;
            if(IsDiagnosticTile(tile))
                LOG_INFO("MainMap diagnostics: BCT1 tile widget visibility xy=%lld,%lld request=%llu visible=%u",
                    static_cast<long long>(tile.demand.TileX),static_cast<long long>(tile.demand.TileY),
                    static_cast<unsigned long long>(tile.request),unsigned(visible));
        }
    }

    bool EnsurePresentationMaterial()
    {
        auto& owner=g_consumer;
        if(owner.presentParent)return true;
        if(owner.presentationRejected||!MainMapNative::CanOperate())return false;
        owner.presentParent=MainMapProof::LoadMaterial(
            "/Game/Mods/MainMap/Materials/M_BuildingComplexPresent.M_BuildingComplexPresent",
            SDK::EMaterialDomain::MD_UI,owner.presentParentOwner);
        if(!owner.presentParent)
        {
            owner.presentationRejected=true;
            LOG_ERROR("MainMap BCT1: presentation material unavailable or incompatible");
            return false;
        }
        LOG_INFO("MainMap diagnostics: BCT1 presentation material ready path=/Game/Mods/MainMap/Materials/M_BuildingComplexPresent.M_BuildingComplexPresent");
        return true;
    }

    bool UpdateTileGeometry(Tile& tile,const BC_Status& status)
    {
        if(!tile.image||!tile.attached||!tile.image->Slot||!tile.image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()))return false;
        auto* slot=static_cast<SDK::UCanvasPanelSlot*>(tile.image->Slot);
        const auto& grid=g_consumer.grid;
        const double x=(status.WorldBounds.MinX-grid.OriginX)/100.0;
        const double y=(status.WorldBounds.MinY-grid.OriginY)/100.0;
        const double tileLocalSize=g_consumer.grid.TileSizeCm/100.0;
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(x+tileLocalSize)||!std::isfinite(y+tileLocalSize)||
            std::abs(x)>(std::numeric_limits<float>::max)()||std::abs(y)>(std::numeric_limits<float>::max)())return false;
        slot->SetAutoSize(false);
        slot->SetAlignment({0,0});
        slot->SetAnchors({{0,0},{0,0}});
        slot->SetPosition({static_cast<float>(x),static_cast<float>(y)});
        slot->SetSize({static_cast<float>(tileLocalSize),static_cast<float>(tileLocalSize)});
        slot->SetZOrder(1);
        return slot->Parent && slot->Content==tile.image;
    }

    bool ClearPresentation(Tile& tile)
    {
        tile.presented=false;
        tile.visible=false;
        tile.midBound=false;
        tile.geometryValid=false;
        auto* image=tile.image;
        auto* material=tile.material;
        if(image)
        {
            image->RemoveFromParent();
            if(image->Slot)
            {
                image->SetVisibility(SDK::ESlateVisibility::Collapsed);
                return false;
            }
            tile.attached=false;
            image->SetBrushFromMaterial(nullptr);
            if(image->Brush.ResourceObject==material)return false;
            tile.brushBound=false;
        }
        if(material)
        {
            const auto textureName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"TileTexture"));
            material->SetTextureParameterValue(textureName,nullptr);
            if(material->K2_GetTextureParameterValue(textureName))return false;
        }
        if(!tile.textureOwner.Reset())return false;
        if(!tile.imageOwner.Reset())return false;
        tile.image=nullptr;
        if(!tile.materialOwner.Reset())return false;
        tile.material=nullptr;
        return true;
    }

    bool BindPresentation(Tile& tile)
    {
        auto& owner=g_consumer;
        if(tile.presented)
        {
            SDK::UCanvasPanel* canvas=nullptr;SDK::UWidgetTree* tree=nullptr;
            if(!tile.image||!tile.attached||!tile.image->Slot||
                !tile.image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())||
                !MainMapBuildingLayer::TryGetTilePresentationContext(canvas,tree))
            { LogBindState(tile,1,"existing-presentation-context-invalid");return false; }
            (void)tree;
            auto* slot=static_cast<SDK::UCanvasPanelSlot*>(tile.image->Slot);
            const bool valid=slot->Parent==canvas&&slot->Content==tile.image;
            LogBindState(tile,valid?0u:2u,valid?"success-existing":"existing-canvas-slot-invalid");
            return valid;
        }
        if(!tile.available||!tile.lease.Texture||!EnsurePresentationMaterial())
        { LogBindState(tile,3,"tile-lease-or-presentation-material-unavailable");return false; }
        if(!MainMapNative::CanOperate())
        { LogBindState(tile,4,"native-admission-closed");return false; }
        SDK::UCanvasPanel* canvas=nullptr;SDK::UWidgetTree* tree=nullptr;
        if(!MainMapBuildingLayer::TryGetTilePresentationContext(canvas,tree)||!canvas||!tree)
        { LogBindState(tile,5,"tile-canvas-context-unavailable");return false; }
        auto* textureObject=static_cast<SDK::UObject*>(tile.lease.Texture);
        if(!textureObject||!textureObject->IsA(SDK::UTexture2D::StaticClass())||!tile.textureOwner.Acquire(textureObject))
        { LogBindState(tile,6,"texture-identity-or-retention-failed");return false; }
        auto* texture=static_cast<SDK::UTexture2D*>(textureObject);
        if(texture->SRGB||texture->Blueprint_GetSizeX()!=static_cast<int32_t>(tile.publication.TextureWidth)||
            texture->Blueprint_GetSizeY()!=static_cast<int32_t>(tile.publication.TextureHeight))
        { LogBindState(tile,7,"texture-sRGB-or-size-validation-failed");return false; }
        auto* mid=SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(
            static_cast<SDK::UObject*>(owner.context.world),owner.presentParent,{},SDK::EMIDCreationFlags::None);
        if(!mid||!tile.materialOwner.Acquire(mid))
        { LogBindState(tile,8,"MID-creation-or-retention-failed");return false; }
        tile.material=mid;
        const auto textureName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"TileTexture"));
        const auto scaleName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"UVScale"));
        const auto biasName=SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"UVBias"));
        const SDK::FLinearColor scale{static_cast<float>(tile.publication.UVScaleX),static_cast<float>(tile.publication.UVScaleY),0,0};
        const SDK::FLinearColor bias{static_cast<float>(tile.publication.UVBiasX),static_cast<float>(tile.publication.UVBiasY),0,0};
        if(!std::isfinite(tile.publication.UVScaleX)||!std::isfinite(tile.publication.UVScaleY)||
            !std::isfinite(tile.publication.UVBiasX)||!std::isfinite(tile.publication.UVBiasY))
        { LogBindState(tile,9,"UV-parameters-nonfinite");return false; }
        mid->SetTextureParameterValue(textureName,texture);
        mid->SetVectorParameterValue(scaleName,scale);
        mid->SetVectorParameterValue(biasName,bias);
        const auto actualScale=mid->K2_GetVectorParameterValue(scaleName);
        const auto actualBias=mid->K2_GetVectorParameterValue(biasName);
        if(mid->K2_GetTextureParameterValue(textureName)!=texture||!FiniteColor(actualScale)||!FiniteColor(actualBias)||
            std::abs(actualScale.R-scale.R)>0.00001f||std::abs(actualScale.G-scale.G)>0.00001f||
            std::abs(actualBias.R-bias.R)>0.00001f||std::abs(actualBias.G-bias.G)>0.00001f)
        { LogBindState(tile,10,"MID-texture-or-UV-readback-mismatch");return false; }
        tile.midBound=true;
        auto* imageObject=SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(),tree);
        if(!imageObject||!tile.imageOwner.Acquire(imageObject))
        { LogBindState(tile,11,"UImage-creation-or-retention-failed");return false; }
        if(!imageObject->IsA(SDK::UImage::StaticClass())||imageObject->Outer!=tree)
        { LogBindState(tile,12,"UImage-identity-validation-failed");return false; }
        tile.image=static_cast<SDK::UImage*>(imageObject);
        tile.image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        tile.image->SetColorAndOpacity({1,1,1,1});
        tile.image->SetBrushFromMaterial(mid);
        tile.brushBound=true;
        if(tile.image->Brush.ResourceObject!=mid)
        { LogBindState(tile,13,"UImage-MID-brush-binding-failed");return false; }
        auto* slot=canvas->AddChildToCanvas(tile.image);
        tile.attached=tile.image->Slot!=nullptr;
        if(!slot||!slot->IsA(SDK::UCanvasPanelSlot::StaticClass())||tile.image->Slot!=slot||
            slot->Parent!=canvas||slot->Content!=tile.image)
        { LogBindState(tile,14,"UImage-canvas-attachment-failed");return false; }
        tile.presented=true;
        if(!UpdateTileGeometry(tile,tile.publication))
        { LogBindState(tile,15,"tile-geometry-validation-failed");return false; }
        tile.geometryValid=true;
        LogBindState(tile,0,"success");
        return true;
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
        // Release is acknowledged only after every widget/material/retained
        // UObject reference to the provider texture has been cleared.
        return ClearPresentation(tile);
    }

    void StopDemand()
    {
        auto& owner=g_consumer;
        owner.available=false;
        for(auto& [key,tile]:owner.tiles) { (void)key;tile.wanted=false;MakeUnavailable(tile); }
    }

    // Every rejected cleanup keeps the exact issued tokens. Withdraw never
    // substitutes for Release while the provider session is closing.
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
                if(!ClearPresentation(tile))continue;
                owner.tiles.erase(current);
            }
        }
    }

    void SuspendTiles()
    {
        auto& owner=g_consumer;
        owner.available=false;
        for(auto& [key,tile]:owner.tiles) { (void)key;MakeUnavailable(tile); }
        for(auto& [key,tile]:owner.tiles) { (void)key;SetTileVisible(tile,false); }
        owner.presentationVisible=false;
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
        if(s.StructSize!=sizeof(s)||s.State<BC_PENDING||s.State>BC_STALE||(s.Flags&~63u)||
            s.Reason>BC_STALE_OR_REVOKED_IDENTITY||s.GridId!=grid.GridId||s.TileX!=d.TileX||s.TileY!=d.TileY||
            s.InteriorSide!=d.InteriorSide||s.WorldBounds.MinX!=x||s.WorldBounds.MinY!=y||
            s.WorldBounds.MaxX!=maxX||s.WorldBounds.MaxY!=maxY||x>=maxX||y>=maxY)return false;
        return publication ? s.PublicationId==tile.status.PublicationId :
            s.RequestId==tile.request&&s.DetailEpoch==d.DetailEpoch;
    }

    bool Drawable(const BC_Status& s)
    {
        const auto gutter=static_cast<uint64_t>(s.Gutter);
        const auto width=static_cast<uint64_t>(s.TextureWidth),height=static_cast<uint64_t>(s.TextureHeight);
        return s.State==BC_READY && (s.Flags&(BC_CURRENT|BC_NATIVE_READY|BC_COMPLETE))==
            (BC_CURRENT|BC_NATIVE_READY|BC_COMPLETE) && !(s.Flags&BC_HAS_FAILURE) &&
            s.PublicationId && s.CertificateId && s.Gutter==g_consumer.policy.Gutter && gutter>0 &&
            s.ReadyCells==s.RequiredCells && !s.PendingCells && !s.FailedCells &&
            width>2*gutter && height>2*gutter && width-2*gutter==s.InteriorSide && height-2*gutter==s.InteriorSide &&
            s.UVScaleX==double(s.InteriorSide)/double(width) && s.UVScaleY==double(s.InteriorSide)/double(height) &&
            s.UVBiasX==double(s.Gutter)/double(width) && s.UVBiasY==double(s.Gutter)/double(height);
    }

    void SubmitTiles(unsigned& calls)
    {
        auto& owner=g_consumer;
        auto it=owner.serviceCursor?owner.tiles.upper_bound(owner.serviceAfter):owner.tiles.begin();
        if(it==owner.tiles.end())it=owner.tiles.begin();
        size_t remaining=owner.tiles.size();
        while(remaining-- && it!=owner.tiles.end() && calls)
        {
            auto& tile=it->second;
            owner.serviceAfter=it->first;owner.serviceCursor=true;++it;
            if(it==owner.tiles.end() && remaining)it=owner.tiles.begin();
            if(!tile.wanted||tile.request||tile.submitFailed)continue;
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
            LogTileStatusChange(tile,"submit");
            if(result==BC_STALE_SESSION||result==BC_CLOSED) { MainMapBuildingComplex::RequestClose();return; }
        }
    }

    void InspectTiles(unsigned& calls)
    {
        auto& owner=g_consumer;
        auto it=owner.serviceCursor?owner.tiles.upper_bound(owner.serviceAfter):owner.tiles.begin();
        if(it==owner.tiles.end())it=owner.tiles.begin();
        size_t remaining=owner.tiles.size();
        while(remaining-- && it!=owner.tiles.end() && calls>=3)
        {
            auto& tile=it->second;
            owner.serviceAfter=it->first;owner.serviceCursor=true;++it;
            if(it==owner.tiles.end() && remaining)it=owner.tiles.begin();
            if(!tile.wanted||!tile.request)continue;
            BC_Status status{};status.StructSize=sizeof(status);
            const BC_Ref request{sizeof(BC_Ref),0,owner.grid.Session,tile.request};
            --calls;
            const auto result=owner.api.InspectRequest(&request,&status);
            tile.requestResult=result;
            MakeUnavailable(tile);
            if(result!=BC_RESULT_OK)LogTileStatusChange(tile,"request-inspect");
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
            LogTileStatusChange(tile,"request-inspect");
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
                LogTileStatusChange(tile,"publication-inspect");
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
                    LogTileStatusChange(tile,"acquire");
                    if(tile.acquireResult==BC_STALE_SESSION||tile.acquireResult==BC_CLOSED)
                    { MainMapBuildingComplex::RequestClose();return; }
                    if(!Temporary(tile.acquireResult))
                    { tile.blockedPublication=status.PublicationId;tile.blockedReadiness=status.ReadinessRevision; }
                    continue;
                }
                tile.lease=lease; // Record ownership BEFORE validating metadata.
                LogTileStatusChange(tile,"acquire");
                if(lease.StructSize!=sizeof(lease)||lease.Reserved||lease.Session!=owner.grid.Session||
                    !lease.LeaseId||lease.PublicationId!=status.PublicationId||!lease.Texture)
                { MainMapBuildingComplex::RequestClose();return; }
            }
            tile.available=true;
            LogTileStatusChange(tile,"lease-available");
        }
    }

    void SyncPresentations()
    {
        auto& owner=g_consumer;
        for(auto& [key,tile]:owner.tiles)
        {
            (void)key;
            if(tile.available&&!tile.releasePending)
            {
                if(!BindPresentation(tile))
                {
                    tile.blockedPublication=tile.status.PublicationId;
                    tile.blockedReadiness=tile.status.ReadinessRevision;
                    MakeUnavailable(tile);
                    tile.releasePending=true;
                    ClearPresentation(tile);
                }
                else
                {
                    SetTileVisible(tile,owner.presentationVisible&&tile.wanted);
                }
            }
            else SetTileVisible(tile,false);
        }
    }

    bool ReconcileVisible(unsigned& calls)
    {
        auto& owner=g_consumer;
        MainMapBuildingLayer::WorldRect visible{};TileRange range{};double pixels=0;
        if(!MainMapBuildingLayer::TryGetVisibleWorldRect(visible))
        {
            // Layout failure never means Empty. Retain interests, suspend new
            // demand/acquisition and clear consumer availability.
            LogVisibleGate(1,"visible-world-rectangle-unavailable");
            SuspendTiles();
            return false;
        }
        if(!MainMapBuildingLayer::TryGetGroupedTilePixelSpan(pixels))
        {
            LogVisibleGate(2,"grouped-tile-pixel-span-unavailable");
            SuspendTiles();
            return false;
        }
        if(!VisibleRange(visible,range))
        {
            LogVisibleGate(3,"visible-tile-range-invalid");
            SuspendTiles();
            return false;
        }
        LogVisibleGate(0,"visible-world-rectangle-pixel-span-and-tile-range-valid");
        const auto side=SelectSide(pixels);
        if(side!=owner.side)
        {
            if(owner.detailEpoch==(std::numeric_limits<uint64_t>::max)()) { MainMapBuildingComplex::RequestClose();return false; }
            ++owner.detailEpoch;owner.side=side;
            LOG_INFO("MainMap diagnostics: BCT1 visible tile resolution selected side=%u detailEpoch=%llu projectedTilePixels=%.3f",
                side,static_cast<unsigned long long>(owner.detailEpoch),pixels);
        }
        for(auto& [key,tile]:owner.tiles)
        {
            tile.wanted=key.first>=range.minX&&key.first<=range.maxX&&key.second>=range.minY&&key.second<=range.maxY&&
                tile.demand.InteriorSide==side&&tile.demand.DetailEpoch==owner.detailEpoch;
            if(!tile.wanted)MakeUnavailable(tile);
        }
        bool diagnosticStillVisible=false;
        if(owner.hasDiagnosticTile)
        {
            const auto current=owner.tiles.find(owner.diagnosticTile);
            diagnosticStillVisible=current!=owner.tiles.end()&&current->second.wanted;
        }
        if(!diagnosticStillVisible)
        {
            owner.hasDiagnosticTile=false;
            if(owner.diagnosticTileSelected&&!owner.diagnosticTileCompleted)
            {
                LOG_INFO("MainMap diagnostics: BCT1 representative visible tile left demand; tile sampling complete for this session");
                owner.diagnosticTileCompleted=true;
            }
            else if(!owner.diagnosticTileSelected) for(auto& [key,tile]:owner.tiles)
            {
                if(!tile.wanted)continue;
                owner.diagnosticTile=key;owner.hasDiagnosticTile=true;
                owner.diagnosticTileSelected=true;
                tile.diagnosticSignature=(std::numeric_limits<uint64_t>::max)();
                tile.bindDiagnostic=(std::numeric_limits<uint32_t>::max)();
                LOG_INFO("MainMap diagnostics: BCT1 representative visible tile selected xy=%lld,%lld side=%u",
                    static_cast<long long>(key.first),static_cast<long long>(key.second),tile.demand.InteriorSide);
                break;
            }
        }
        unsigned cleanup=kServiceCalls/2;CleanupTiles(cleanup);
        // Allocate at most one service batch per tick; old rejected ownership
        // consumes capacity until acknowledged. No offscreen request is added.
        unsigned added=0;
        for(int64_t y=range.minY;y<=range.maxY&&added<kServiceCalls&&owner.tiles.size()<kMaxTiles;++y)
            for(int64_t x=range.minX;x<=range.maxX&&added<kServiceCalls&&owner.tiles.size()<kMaxTiles;++x)
            {
                if(owner.tiles.contains({x,y}))continue;
                try
                {
                    auto inserted=owner.tiles.try_emplace(TileKey{x,y});
                    if(inserted.second)
                    {
                        auto& tile=inserted.first->second;tile.wanted=true;
                        tile.demand={sizeof(BC_Demand),side,owner.grid.Session,0,x,y,1,0,owner.grid.GridId,owner.detailEpoch};
                        ++added;
                    }
                }
                // Preserve progress inspecting existing visible ownership even
                // if allocating bookkeeping for a new tile fails.
                catch(...) { added=kServiceCalls;break; }
            }
        calls=kServiceCalls-kServiceCalls/2+cleanup;
        SubmitTiles(calls);
        return true;
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
        const auto withdrawResult=owner.api.WithdrawAll(&ref);
        LogProviderResult("WithdrawAll",withdrawResult,owner.diagnosticWithdrawResult);
        if(withdrawResult!=BC_RESULT_OK)return;
        // Close OK alone acknowledges closure; every rejection retains ownership.
        const auto closeResult=owner.api.Close(&ref);
        if(closeResult!=BC_RESULT_OK)
        {
            LogProviderResult("Close",closeResult,owner.diagnosticCloseResult);
            return;
        }
        const uint64_t closedSession=owner.grid.Session;
        owner.grid={};owner.context={};owner.policy={};owner.closing=false;
        if(owner.presentParentOwner && owner.presentParentOwner.Reset())owner.presentParent=nullptr;
        else if(owner.presentParentOwner)
            LOG_WARN("MainMap BCT1: presentation material retention reset deferred after provider close");
        owner.presentationVisible=false;
        owner.side=0;owner.detailEpoch=0;owner.nextDemand=1;
        owner.serviceCursor=false;owner.cleanupCursor=false;
        owner.hasDiagnosticTile=false;
        owner.diagnosticTileSelected=false;
        owner.diagnosticTileCompleted=false;
        owner.diagnosticOpenResult=(std::numeric_limits<uint32_t>::max)();
        owner.diagnosticInspectResult=(std::numeric_limits<uint32_t>::max)();
        owner.diagnosticPolicyResult=(std::numeric_limits<uint32_t>::max)();
        owner.diagnosticWithdrawResult=(std::numeric_limits<uint32_t>::max)();
        owner.diagnosticCloseResult=(std::numeric_limits<uint32_t>::max)();
        LogProviderResult("Close",closeResult,owner.diagnosticCloseResult);
        LOG_INFO("MainMap diagnostics: BCT1 session closed session=%llu tilesReleased=1 leasesReleased=1",
            static_cast<unsigned long long>(closedSession));
    }

    void Discover()
    {
        auto& owner=g_consumer;
        if(owner.discovered)return;
        owner.discovered=true;
        // Borrowed module only. Startup-only policy supplies logical lifetime;
        // no Windows reference/pin can enforce that policy or prevent shutdown.
        const auto module=GetModuleHandleW(L"MiniMap.dll");
        if(!module)
        {
            LOG_INFO("MainMap diagnostics: BCT1 provider discovery=failed reason=MiniMap-module-not-loaded");
            return;
        }
        const auto getter=reinterpret_cast<Getter>(GetProcAddress(module,"MiniMap_GetBuildingComplexApi"));
        BC_Api api{};api.StructSize=sizeof(api);
        const auto result=getter?getter(BC_VERSION,&api):BC_UNAVAILABLE;
        if(!getter || result!=BC_RESULT_OK || api.StructSize!=112 ||
            api.Version!=1 || api.ContractId!=0x42435431u || api.Reserved ||
            !api.Open || !api.InspectSession || !api.Submit || !api.InspectRequest || !api.InspectPublication ||
            !api.Acquire || !api.Release || !api.Withdraw || !api.WithdrawAll || !api.Close || !api.GetPolicy || !api.Service)
        {
            LOG_INFO("MainMap diagnostics: BCT1 provider discovery=failed getter=%u result=%u size=%u version=%u contract=0x%X open=%u inspect=%u submit=%u publication=%u acquire=%u release=%u close=%u policy=%u",
                unsigned(getter!=nullptr),result,api.StructSize,api.Version,api.ContractId,unsigned(api.Open!=nullptr),
                unsigned(api.InspectSession!=nullptr),unsigned(api.Submit!=nullptr),unsigned(api.InspectPublication!=nullptr),
                unsigned(api.Acquire!=nullptr),unsigned(api.Release!=nullptr),unsigned(api.Close!=nullptr),unsigned(api.GetPolicy!=nullptr));
            return;
        }
        owner.api=api;
        LOG_INFO("MainMap diagnostics: BCT1 provider discovery=success version=%u contract=0x%X",api.Version,api.ContractId);
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

    void SetPresentationVisible(bool visible)
    {
        auto& owner=g_consumer;
        if(!MainMapNative::CanOperate()) { owner.presentationVisible=false;return; }
        owner.presentationVisible=visible&&owner.available&&!owner.closing&&!owner.presentationRejected;
        for(auto& [key,tile]:owner.tiles)
        {
            (void)key;
            SetTileVisible(tile,owner.presentationVisible&&tile.wanted&&tile.presented&&tile.available&&
                !tile.releasePending&&tile.lease.LeaseId!=0);
        }
    }

    void ServiceClose()
    {
        ServiceCloseWithBudget(kServiceCalls);
    }

    void Tick(bool materialContainerMounted,bool mainMapVisible)
    {
        auto& owner=g_consumer;
        if(owner.stopped)return;
        owner.available=false;
        if(owner.closing||g_closeRequested.load()) { ServiceClose();return; }
        if(!mainMapVisible)
        {
            if(owner.grid.Session||!owner.tiles.empty())
            {
                RequestClose();
                ServiceClose();
            }
            return;
        }
        if(!MainMapNative::CanOperate() || owner.closing || owner.policyRejected||owner.presentationRejected)return;
        Discover(); // First admitted tick, after startup plugin initialization.
        if(!owner.api.Open)return;
        MainMapBuildingLayer::ConsumerContext context{};
        if(!MainMapBuildingLayer::TryGetConsumerContext(context))
        {
            if(owner.diagnosticContextState!=0)
            {
                owner.diagnosticContextState=0;
                LOG_INFO("MainMap diagnostics: BCT1 session context=unavailable active terrain/menu/world admission not satisfied");
            }
            RequestClose();ServiceClose();return;
        }
        if(owner.diagnosticContextState!=1)
        {
            owner.diagnosticContextState=1;
            LOG_INFO("MainMap diagnostics: BCT1 session context=available world=%p epoch=%llu origin=%.3f,%.3f",
                context.world,static_cast<unsigned long long>(context.epoch),context.originX,context.originY);
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
            LogProviderResult("InspectSession",result,owner.diagnosticInspectResult);
            owner.available=false;
            if(Temporary(result)) { SuspendTiles();return; }
            if(result!=BC_RESULT_OK || !ValidateGrid(inspected,context) || !SameIdentity(owner.grid,inspected))
            {
                RequestClose();ServiceClose();return;
            }
            owner.grid=inspected;
            owner.available=true;
        }
        else
        {
            BC_Open request{};request.StructSize=sizeof(request);
            request.World=context.world;
            request.OriginX=context.originX;request.OriginY=context.originY;
            request.CellSizeCm=12500.0;request.CellPixels=256;request.GroupWidth=4;request.GridSchema=BC_SCHEMA;
            BC_Grid opened{};opened.StructSize=sizeof(opened);
            const auto result=owner.api.Open(&request,&opened);
            if(result!=BC_RESULT_OK)
            {
                LogProviderResult("Open",result,owner.diagnosticOpenResult);
                return;
            }
            // Record ownership before validating success metadata. An incompatible
            // descriptor is not permission to leak an issued session.
            owner.grid=opened;owner.context=context;
            LogProviderResult("Open",result,owner.diagnosticOpenResult);
            if(!ValidateGrid(opened,context))
            {
                LOG_INFO("MainMap diagnostics: BCT1 Open returned invalid grid session=%llu gridValidation=failed",
                    static_cast<unsigned long long>(opened.Session));
                RequestClose();ServiceClose();
                // Success without an issued token is a broken provider contract.
                if(!opened.Session) { owner.api={};owner.discovered=true; }
                return;
            }
            owner.available=true;
            owner.diagnosticOpenResult=(std::numeric_limits<uint32_t>::max)();
            owner.diagnosticInspectResult=(std::numeric_limits<uint32_t>::max)();
            owner.diagnosticPolicyResult=(std::numeric_limits<uint32_t>::max)();
            owner.hasDiagnosticTile=false;
            owner.diagnosticTileSelected=false;
            owner.diagnosticTileCompleted=false;
            owner.diagnosticVisibleGate=(std::numeric_limits<uint32_t>::max)();
            LOG_INFO("MainMap diagnostics: BCT1 session opened session=%llu grid=%llu gridValidation=success",
                static_cast<unsigned long long>(owner.grid.Session),static_cast<unsigned long long>(owner.grid.GridId));
        }
        if(!owner.policy.PolicyId)
        {
            BC_Policy policy{};policy.StructSize=sizeof(policy);
            const BC_Ref ref{sizeof(BC_Ref),0,owner.grid.Session,0};
            const auto result=owner.api.GetPolicy(&ref,&policy);
            LogProviderResult("GetPolicy",result,owner.diagnosticPolicyResult);
            if(Temporary(result)) { owner.available=false;return; }
            if(result!=BC_RESULT_OK||!ValidatePolicy(policy))
            {
                LOG_INFO("MainMap diagnostics: BCT1 policy validation failed result=%u policy=%u classes=%u gutter=%u maxSide=%u flags=0x%X",
                    result,policy.PolicyId,policy.ClassCount,policy.Gutter,policy.MaxInteriorSide,policy.Flags);
                if(result==BC_RESULT_OK||result==BC_VERSION_MISMATCH)owner.policyRejected=true;
                RequestClose();ServiceClose();return;
            }
            owner.policy=policy;
            LOG_INFO("MainMap diagnostics: BCT1 policy accepted session=%llu policy=%u classes=%u gutter=%u maxSide=%u flags=0x%X",
                static_cast<unsigned long long>(owner.grid.Session),policy.PolicyId,policy.ClassCount,policy.Gutter,
                policy.MaxInteriorSide,policy.Flags);
        }
        // Keep the provider session serviceable, but do not request/acquire
        // unpresentable tiles until the one-time resource mount has succeeded.
        if(!materialContainerMounted)
        {
            if(owner.diagnosticMaterialsMounted!=0)
            {
                owner.diagnosticMaterialsMounted=0;
                LOG_INFO("MainMap diagnostics: BCT1 tile demand suspended reason=material-container-not-mounted");
            }
            SuspendTiles();
            ServiceProvider();
            if(g_closeRequested.load())ServiceClose();
            return;
        }
        if(owner.diagnosticMaterialsMounted!=1)
        {
            owner.diagnosticMaterialsMounted=1;
            LOG_INFO("MainMap diagnostics: BCT1 material-container availability=mounted");
        }
        if(owner.available&&!EnsurePresentationMaterial())
        {
            RequestClose();ServiceClose();return;
        }
        unsigned calls=0;
        const bool reconciled=owner.available?ReconcileVisible(calls):false;
        if(!owner.available)SuspendTiles();
        if(g_closeRequested.load()) { ServiceClose();return; }
        const bool serviced=ServiceProvider();
        if(g_closeRequested.load()) { ServiceClose();return; }
        if(reconciled&&serviced)InspectTiles(calls);
        SyncPresentations();
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
        if(MainMapNative::CanOperate())
            for(auto& [key,tile]:owner.tiles) { (void)key;ClearPresentation(tile); }
        if(owner.tiles.empty()&&owner.presentParentOwner.Reset())owner.presentParent=nullptr;
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
