#include "BuildingTarget.h"
#include "ProofReflection.h"
#include "plugin_helpers.h"
#include <cmath>

static_assert(sizeof(SDK::FCanvasUVTri) == 0x90);
static_assert(sizeof(SDK::FDrawToRenderTargetContext) == 8);
static_assert(sizeof(SDK::TArray<SDK::FCanvasUVTri>) == 0x10);
static_assert(offsetof(SDK::FCanvasUVTri, V2_Color) == 0x80);

namespace MainMapProof
{
    bool BuildingTarget::ValidateReflection()
    {
        using namespace MainMapProofReflection;
        auto* cls = SDK::UKismetRenderingLibrary::StaticClass();
        return Require(cls, "KismetRenderingLibrary", "CreateRenderTarget2D", 0x30,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"Width",8,4,"IntProperty"},
             {"Height",12,4,"IntProperty"},{"Format",16,1,"ByteProperty"},
             {"ClearColor",20,16,"StructProperty"},{"bAutoGenerateMipMaps",36,1,"BoolProperty"},
             {"bSupportUAVs",37,1,"BoolProperty"},{"ReturnValue",40,8,"ObjectProperty"}}) &&
            Require(cls,"KismetRenderingLibrary","ClearRenderTarget2D",0x20,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"TextureRenderTarget",8,8,"ObjectProperty"},
             {"ClearColor",16,16,"StructProperty"}}) &&
            Require(cls,"KismetRenderingLibrary","BeginDrawCanvasToRenderTarget",0x30,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"TextureRenderTarget",8,8,"ObjectProperty"},
             {"Canvas",16,8,"ObjectProperty",0x180},{"Size",24,16,"StructProperty",0x180},{"Context",40,8,"StructProperty",0x180}}) &&
            Require(cls,"KismetRenderingLibrary","EndDrawCanvasToRenderTarget",0x10,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"Context",8,8,"StructProperty",0x8000182}}) &&
            Require(cls,"KismetRenderingLibrary","ReadRenderTargetRawPixel",0x30,
            {{"WorldContextObject",0,8,"ObjectProperty"},{"TextureRenderTarget",8,8,"ObjectProperty"},
             {"X",16,4,"IntProperty"},{"Y",20,4,"IntProperty"},{"bNormalize",24,1,"BoolProperty"},
             {"ReturnValue",28,16,"StructProperty"}}) &&
            Require(SDK::UCanvas::StaticClass(),"Canvas","K2_DrawMaterialTriangle",0x18,
            {{"RenderMaterial",0,8,"ObjectProperty"},{"Triangles",8,16,"ArrayProperty"}});
    }

    bool BuildingTarget::Create(SDK::UWorld* world, bool measureClear)
    {
        if (!world || !MainMapNative::CanOperate()) return false;
        world_ = world;
        target_ = SDK::UKismetRenderingLibrary::CreateRenderTarget2D(world,2048,2048,
            SDK::ETextureRenderTargetFormat::RTF_RGBA16f,{0,0,0,1},false,false);
        if (!target_ || !lease_.Acquire(target_)) return false;
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
        LOG_INFO("MainMap A2.3: retained target=%p linear 2048x2048 RTF_RGBA16f",target_);
        return !measureClear || Check("clear",16,16,{0,0,0,1},0.001f);
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

    SDK::FLinearColor BuildingTarget::Read(int x,int y)
    {
        return SDK::UKismetRenderingLibrary::ReadRenderTargetRawPixel(world_,target_,x,y,false);
    }

    bool BuildingTarget::Check(const char* name,int x,int y,SDK::FLinearColor expected,float tolerance)
    {
        const auto actual=Read(x,y);
        const bool pass=std::isfinite(actual.R)&&std::isfinite(actual.G)&&std::isfinite(actual.B)&&std::isfinite(actual.A)&&
            std::abs(actual.R-expected.R)<=tolerance&&std::abs(actual.G-expected.G)<=tolerance&&
            std::abs(actual.B-expected.B)<=tolerance&&std::abs(actual.A-expected.A)<=tolerance;
        LOG_INFO("MainMap A2.3: pixel %s (%d,%d) actual=(%.8f,%.8f,%.8f,%.8f) expected=(%.8f,%.8f,%.8f,%.8f) tol=%.4f %s",
            name,x,y,actual.R,actual.G,actual.B,actual.A,expected.R,expected.G,expected.B,expected.A,tolerance,pass?"PASS":"FAIL");
        return pass;
    }

    BuildingTarget::~BuildingTarget()
    {
        End();
        if (!lease_.Reset()) LOG_ERROR("MainMap A2.3: target lease cleanup FAILED");
        // Normal GC/engine resource retirement, never ReleaseRenderTarget2D.
    }
}
