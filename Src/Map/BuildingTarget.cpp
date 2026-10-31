#include "BuildingTarget.h"
#include "plugin_helpers.h"

static_assert(sizeof(SDK::FCanvasUVTri) == 0x90);
static_assert(sizeof(SDK::FDrawToRenderTargetContext) == 8);
static_assert(sizeof(SDK::TArray<SDK::FCanvasUVTri>) == 0x10);
static_assert(offsetof(SDK::FCanvasUVTri, V2_Color) == 0x80);

namespace MainMapProof
{
    bool BuildingTarget::Create(SDK::UWorld* world)
    {
        if (!world || target_ || lease_ || !MainMapNative::CanOperate()) return false;
        auto* candidate = SDK::UKismetRenderingLibrary::CreateRenderTarget2D(world,2048,2048,
            SDK::ETextureRenderTargetFormat::RTF_RGBA16f,{0,0,0,1},false,false);
        if (!candidate || !lease_.Acquire(candidate)) return false;
        target_ = candidate;
        world_ = world;
        if (!target_->bForceLinearGamma || target_->bAutoGenerateMips || target_->bSupportsUAV ||
            target_->SizeX != 2048 || target_->SizeY != 2048 ||
            target_->RenderTargetFormat != SDK::ETextureRenderTargetFormat::RTF_RGBA16f)
        {
            LOG_ERROR("MainMap A2.3: target format/linear/mip/UAV gate FAILED");
            return false;
        }
        target_->Filter = SDK::ETextureFilter::TF_Bilinear;
        target_->AddressX = SDK::ETextureAddress::TA_Clamp;
        target_->AddressY = SDK::ETextureAddress::TA_Clamp;
        SDK::UKismetRenderingLibrary::ClearRenderTarget2D(world,target_,{0,0,0,1});
        return true;
    }

    bool BuildingTarget::Begin()
    {
        if (begun_ || !target_ || !MainMapNative::CanOperate()) return false;
        SDK::FVector2D size{};
        SDK::UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(world_,target_,&canvas_,&size,&context_);
        // A nonzero context must be consumed even if another output fails validation.
        begun_ = context_.RenderTarget != nullptr;
        if (!canvas_ || context_.RenderTarget != target_ || size.X != 2048 || size.Y != 2048)
        {
            End();
            return false;
        }
        return begun_;
    }

    void BuildingTarget::Draw(SDK::UMaterialInterface* material, SDK::FCanvasUVTri* triangles, int count)
    {
        if (!begun_ || !material || !triangles || count <= 0) return;
        const SDK::TArray<SDK::FCanvasUVTri> borrowed(triangles,count,count);
        canvas_->K2_DrawMaterialTriangle(material,borrowed);
    }

    void BuildingTarget::End()
    {
        if (!begun_) return;
        SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(world_,context_);
        context_ = {};
        canvas_ = nullptr;
        begun_ = false;
    }

    bool BuildingTarget::Reset()
    {
        if (!target_ && !begun_) return true;
        if (!MainMapNative::CanOperate()) return false;
        End();
        if (!lease_.Reset()) return false;
        target_ = nullptr;
        world_ = nullptr;
        return true;
    }

    BuildingTarget::~BuildingTarget()
    {
        if (!Reset()) LOG_ERROR("MainMap A2.3: target lease cleanup FAILED");
        // Normal GC/engine resource retirement, never ReleaseRenderTarget2D.
    }
}
