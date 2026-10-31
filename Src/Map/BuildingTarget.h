#pragma once
#include "Native/NativeApi.h"
#include "Engine_classes.hpp"

namespace MainMapProof
{
    // Proof-stage target owner. Methods/destruction require admitted GT operations;
    // an immutable submitted target may remain retained between operations.
    class BuildingTarget final
    {
    public:
        static bool ValidateReflection();
        bool Create(SDK::UWorld* world, bool measureClear = true);
        bool Begin();
        void Draw(SDK::UMaterialInterface* material, SDK::FCanvasUVTri* triangles, int count);
        void End();
        SDK::FLinearColor Read(int x, int y);
        bool Check(const char* name, int x, int y, SDK::FLinearColor expected, float tolerance);
        SDK::UTextureRenderTarget2D* Get() const { return target_; }
        ~BuildingTarget();
    private:
        MainMapNative::StrongObjectLease lease_;
        SDK::UWorld* world_ = nullptr;
        SDK::UTextureRenderTarget2D* target_ = nullptr;
        SDK::UCanvas* canvas_ = nullptr;
        SDK::FDrawToRenderTargetContext context_{};
        bool begun_ = false;
    };
}
