#pragma once
#include "Native/NativeApi.h"
#include "Engine_classes.hpp"

namespace MainMapProof
{
    // Target owner. Methods/destruction require admitted GT operations;
    // the retained target may remain between production rendering operations.
    class BuildingTarget final
    {
    public:
        bool Create(SDK::UWorld* world);
        bool Begin();
        void Draw(SDK::UMaterialInterface* material, SDK::FCanvasUVTri* triangles, int count);
        void End();
        // Explicit retryable retirement for a persistent session. Rejection
        // preserves the lease and context; the owner must remain alive.
        bool Reset();
        SDK::UCanvas* Canvas() const { return begun_ ? canvas_ : nullptr; }
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
