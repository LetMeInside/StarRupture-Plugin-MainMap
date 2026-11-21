// Presents BuildingMapTarget's retained Section target on the terrain widget's own canvas.
// The single UImage and every UObject it references remain strongly leased until admitted retirement.
#include "BuildingMapPresentation.h"

#include "ChimeraUI_classes.hpp"
#include "Engine_classes.hpp"
#include "MaterialProof.h"
#include "SlateCore_structs.hpp"
#include "UMG_classes.hpp"
#include "plugin_helpers.h"

#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace MainMapBuildingMap
{
    /** Returns a process-lifetime owner so rejected UObject releases remain retryable. */
    BuildingMapPresentation& BuildingMapPresentation::SharedOwner() noexcept
    {
        alignas(BuildingMapPresentation) static std::uint8_t storage[sizeof(BuildingMapPresentation)]{};
        static BuildingMapPresentation* presentation = ::new (storage) BuildingMapPresentation();
        return *presentation;
    }

    /** Updates the visible Section presentation without advancing provider or composition work. */
    bool BuildingMapPresentation::UpdateShared(const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree, const SectionTargetInfo& targetInfo,
        bool targetReady, bool materialResourcesReady)
    {
        return SharedOwner().Update(context, canvas, tree, targetInfo, targetReady, materialResourcesReady);
    }

    /** Detaches the image and releases the shared presentation owner through admitted operations. */
    bool BuildingMapPresentation::RetireShared(bool widgetDestructionStarted) noexcept
    {
        return SharedOwner().Retire(widgetDestructionStarted);
    }

    /** Creates or updates one retained UI presentation for the current terrain session. */
    bool BuildingMapPresentation::Update(const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree, const SectionTargetInfo& targetInfo,
        bool targetReady, bool materialResourcesReady)
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread() ||
            !context.world || !context.epoch) return false;
        if (epoch_ && epoch_ != context.epoch && !Retire(false)) return false;
        epoch_ = context.epoch;
        if (!targetReady || !targetInfo.Target || !materialResourcesReady)
            return WithdrawTarget();
        if (!canvas || !tree || !EnsureMaterial(context.world, materialResourcesReady) ||
            !EnsureImage(canvas, tree))
        {
            HideImage();
            return false;
        }
        if (!ValidateAttachment(canvas, tree))
        {
            HideImage();
            return false;
        }
        SDK::FVector2D position{};
        SDK::FVector2D size{};
        if (!MapSectionGeometry(context, canvas, targetInfo, position, size))
        {
            HideImage();
            return false;
        }
        return SynchronizeTarget(targetInfo, position, size);
    }

    /** Loads the packaged UI material once after the existing embedded mount is accepted. */
    bool BuildingMapPresentation::EnsureMaterial(SDK::UObject* worldContext, bool materialResourcesReady)
    {
        if (!materialResourcesReady) return false;
        if (materialInstance_ && materialInstanceDetached_)
        {
            if (!materialInstance_.Reset())
            {
                LogRetirementFailure("withdraw-material-instance-lease-release");
                return false;
            }
            materialInstanceDetached_ = false;
            materialInstanceAttempted_ = false;
            retirementFailureStage_ = nullptr;
        }
        if (materialValidated_ && parentMaterial_ && materialInstance_) return true;
        if (!parentMaterial_)
        {
            if (materialLoadAttempted_) return false;
            materialLoadAttempted_ = true;
            auto* parent = MainMapProof::LoadMaterial(
                "/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
                SDK::EMaterialDomain::MD_UI, parentMaterial_);
            if (!parent) return false;
        }
        if (materialInstanceAttempted_) return false;
        materialInstanceAttempted_ = true;
        auto* instance = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(
            worldContext, static_cast<SDK::UMaterialInterface*>(parentMaterial_.Get()), {},
            SDK::EMIDCreationFlags::None);
        if (!instance || !instance->IsA(SDK::UMaterialInstanceDynamic::StaticClass()) ||
            !materialInstance_.Acquire(instance)) return false;
        materialInstanceDetached_ = false;
        materialValidated_ = true;
        return true;
    }

    /** Creates the one retained UImage with the terrain WidgetTree as its Outer and attaches it. */
    bool BuildingMapPresentation::EnsureImage(SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree)
    {
        auto* image = Image();
        if (image)
        {
            if (image->Outer != tree) return false;
            if (image->GetParent() == canvas)
            {
                auto* material = MaterialInstance();
                if (!material) return false;
                if (image->Brush.ResourceObject != material) image->SetBrushFromMaterial(material);
                return ValidateAttachment(canvas, tree);
            }
            if (image->GetParent() || image->Slot) return false;
        }
        else
        {
            auto* object = SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), tree);
            if (!object || !object->IsA(SDK::UImage::StaticClass()) || object->Outer != tree ||
                !image_.Acquire(object)) return false;
            image = static_cast<SDK::UImage*>(object);
            image->SetVisibility(SDK::ESlateVisibility::Collapsed);
            image->SetColorAndOpacity({1, 1, 1, 1});
        }
        auto* material = MaterialInstance();
        if (!material) return false;
        image->SetBrushFromMaterial(material);
        if (image->Brush.ResourceObject != material) return false;
        auto* slot = canvas->AddChildToCanvas(image);
        if (!slot || !slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) || image->Slot != slot) return false;
        const bool attached = ValidateAttachment(canvas, tree);
        if (attached && !firstAttachmentLogged_)
        {
            LOG_INFO("MainMap diagnostics: first presentation attachment established");
            firstAttachmentLogged_ = true;
        }
        return attached;
    }

    /** Confirms WidgetTree Outer, UImage type, parent, CanvasPanelSlot, and material brush identity. */
    bool BuildingMapPresentation::ValidateAttachment(SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree) const noexcept
    {
        auto* image = Image();
        auto* material = MaterialInstance();
        if (!canvas || !tree || !image || !material || !image->IsA(SDK::UImage::StaticClass()) ||
            image->Outer != tree || image->GetParent() != canvas || !image->Slot ||
            !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) || image->Brush.ResourceObject != material)
            return false;
        auto* slot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        return slot->Parent == canvas && slot->Content == image;
    }

    /** Converts terrain-local Section bounds to CanvasPanelTerrain-local coordinates with Slate geometry. */
    bool BuildingMapPresentation::MapSectionGeometry(const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas, const SectionTargetInfo& targetInfo,
        SDK::FVector2D& position, SDK::FVector2D& size) const
    {
        if (!canvas || !std::isfinite(context.originX) || !std::isfinite(context.originY) ||
            !std::isfinite(targetInfo.XMapUnits) || !std::isfinite(targetInfo.YMapUnits) ||
            !std::isfinite(targetInfo.SectionSizeMapUnits) || targetInfo.SectionSizeMapUnits <= 0.0)
            return false;
        if (!canvas->Outer || !canvas->Outer->IsA(SDK::UWidgetTree::StaticClass())) return false;
        auto* tree = static_cast<SDK::UWidgetTree*>(canvas->Outer);
        auto* terrainObject = tree ? tree->Outer : nullptr;
        if (!terrainObject || !terrainObject->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass())) return false;
        auto* terrain = static_cast<SDK::UCrUW_MapMenuTerrain*>(terrainObject);
        if (terrain->WidgetTree != tree || terrain->CanvasPanelTerrain != canvas) return false;
        const auto terrainGeometry = terrain->GetPaintSpaceGeometry();
        const auto canvasGeometry = canvas->GetPaintSpaceGeometry();
        const double localX = (targetInfo.XMapUnits - context.originX) / 100.0;
        const double localY = (targetInfo.YMapUnits - context.originY) / 100.0;
        const double localSize = targetInfo.SectionSizeMapUnits / 100.0;
        if (!std::isfinite(localX) || !std::isfinite(localY) || !std::isfinite(localSize) || localSize <= 0.0)
            return false;
        return MapAbsoluteBoundsToCanvas(canvasGeometry, terrainGeometry, localX, localY, localSize, position, size);
    }

    /** Converts both Section corners between Slate geometries and rejects invalid float slot values. */
    bool BuildingMapPresentation::MapAbsoluteBoundsToCanvas(const SDK::FGeometry& canvasGeometry,
        const SDK::FGeometry& terrainGeometry, double localX, double localY, double localSize,
        SDK::FVector2D& position, SDK::FVector2D& size) const
    {
        const double limit = (std::numeric_limits<float>::max)();
        if (!std::isfinite(localX + localSize) || !std::isfinite(localY + localSize) ||
            std::abs(localX) > limit || std::abs(localY) > limit ||
            std::abs(localX + localSize) > limit || std::abs(localY + localSize) > limit)
            return false;
        const auto absoluteTopLeft = SDK::USlateBlueprintLibrary::LocalToAbsolute(terrainGeometry,
            {static_cast<float>(localX), static_cast<float>(localY)});
        const auto absoluteBottomRight = SDK::USlateBlueprintLibrary::LocalToAbsolute(terrainGeometry,
            {static_cast<float>(localX + localSize), static_cast<float>(localY + localSize)});
        const auto canvasTopLeft = SDK::USlateBlueprintLibrary::AbsoluteToLocal(canvasGeometry, absoluteTopLeft);
        const auto canvasBottomRight = SDK::USlateBlueprintLibrary::AbsoluteToLocal(canvasGeometry, absoluteBottomRight);
        const double width = static_cast<double>(canvasBottomRight.X) - canvasTopLeft.X;
        const double height = static_cast<double>(canvasBottomRight.Y) - canvasTopLeft.Y;
        const float slotWidth = static_cast<float>(width);
        const float slotHeight = static_cast<float>(height);
        if (!std::isfinite(canvasTopLeft.X) || !std::isfinite(canvasTopLeft.Y) ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0 ||
            std::abs(static_cast<double>(canvasTopLeft.X)) > limit ||
            std::abs(static_cast<double>(canvasTopLeft.Y)) > limit || width > limit || height > limit ||
            !std::isfinite(slotWidth) || !std::isfinite(slotHeight) || slotWidth <= 0.0f || slotHeight <= 0.0f)
            return false;
        position = {canvasTopLeft.X, canvasTopLeft.Y};
        size = {slotWidth, slotHeight};
        return true;
    }

    /** Sets top-left anchors, zero alignment, explicit position/size, and the chosen canvas Z order. */
    bool BuildingMapPresentation::ApplyGeometry(const SDK::FVector2D& position, const SDK::FVector2D& size)
    {
        auto* image = Image();
        if (!image || !image->Slot || !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return false;
        auto* slot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        const auto currentPosition = slot->GetPosition();
        const auto currentSize = slot->GetSize();
        const auto currentAnchors = slot->GetAnchors();
        const auto currentAlignment = slot->GetAlignment();
        if (!slot->GetAutoSize() && slot->GetZOrder() == 1 &&
            currentAnchors.Minimum.X == 0.0f && currentAnchors.Minimum.Y == 0.0f &&
            currentAnchors.Maximum.X == 0.0f && currentAnchors.Maximum.Y == 0.0f &&
            currentAlignment.X == 0.0f && currentAlignment.Y == 0.0f &&
            currentPosition.X == position.X && currentPosition.Y == position.Y &&
            currentSize.X == size.X && currentSize.Y == size.Y) return true;
        slot->SetAutoSize(false);
        slot->SetAnchors({{0, 0}, {0, 0}});
        slot->SetAlignment({0, 0});
        slot->SetPosition(position);
        slot->SetSize(size);
        slot->SetZOrder(1);
        const auto actualPosition = slot->GetPosition();
        const auto actualSize = slot->GetSize();
        const auto anchors = slot->GetAnchors();
        const auto alignment = slot->GetAlignment();
        return slot->GetAutoSize() == false && slot->GetZOrder() == 1 &&
            anchors.Minimum.X == 0.0f && anchors.Minimum.Y == 0.0f &&
            anchors.Maximum.X == 0.0f && anchors.Maximum.Y == 0.0f &&
            alignment.X == 0.0f && alignment.Y == 0.0f &&
            actualPosition.X == position.X && actualPosition.Y == position.Y &&
            actualSize.X == size.X && actualSize.Y == size.Y;
    }

    /** Binds BuildingTarget and validates the dynamic material's observable texture readback. */
    bool BuildingMapPresentation::BindTarget(SDK::UTextureRenderTarget2D* target)
    {
        auto* material = MaterialInstance();
        if (!material || !target) return false;
        const auto parameter = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"BuildingTarget"));
        material->SetTextureParameterValue(parameter, target);
        return material->K2_GetTextureParameterValue(parameter) == target;
    }

    /** Reads back the active BuildingTarget without rewriting an unchanged material parameter. */
    bool BuildingMapPresentation::IsTargetBound(SDK::UTextureRenderTarget2D* target) const noexcept
    {
        auto* material = MaterialInstance();
        if (!material) return false;
        const auto parameter = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"BuildingTarget"));
        return material->K2_GetTextureParameterValue(parameter) == target;
    }

    /** Rebinds the last valid target after a candidate parameter update fails. */
    bool BuildingMapPresentation::RestoreCurrentTarget() noexcept
    {
        auto* material = MaterialInstance();
        if (!material || !presentedTargetValid_) return false;
        auto* target = static_cast<SDK::UTextureRenderTarget2D*>(target_.Get());
        if (!target) return false;
        const auto parameter = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"BuildingTarget"));
        material->SetTextureParameterValue(parameter, target);
        return material->K2_GetTextureParameterValue(parameter) == target;
    }

    /** Reuses a target identity or transactionally changes the MID binding while retaining both sides. */
    bool BuildingMapPresentation::SynchronizeTarget(const SectionTargetInfo& targetInfo,
        const SDK::FVector2D& position, const SDK::FVector2D& size)
    {
        auto* image = Image();
        auto* candidate = static_cast<SDK::UTextureRenderTarget2D*>(targetInfo.Target);
        if (!image || !candidate || !candidate->IsA(SDK::UTextureRenderTarget2D::StaticClass()) ||
            targetInfo.Width <= 0 || targetInfo.Height <= 0 || candidate->SizeX != targetInfo.Width ||
            candidate->SizeY != targetInfo.Height ||
            candidate->RenderTargetFormat != SDK::ETextureRenderTargetFormat::RTF_RGBA16f ||
            candidate->bForceLinearGamma == 0) return false;
        if (candidateTarget_)
        {
            if (!presentedTargetValid_)
            {
                WithdrawTarget();
                return false;
            }
            if (!RestoreCurrentTarget())
            {
                HideImage();
                return false;
            }
            if (!candidateTarget_.Reset())
            {
                LogRetirementFailure("candidate-target-lease-release");
                return false;
            }
            retirementFailureStage_ = nullptr;
        }
        if (retiredTarget_) RetryRetiredTarget();
        if (candidate == target_.Get())
        {
            if (presentedTargetValid_ &&
                (targetInfo.GridX != presentedInfo_.GridX || targetInfo.GridY != presentedInfo_.GridY ||
                 targetInfo.XMapUnits != presentedInfo_.XMapUnits ||
                 targetInfo.YMapUnits != presentedInfo_.YMapUnits ||
                 targetInfo.SectionSizeMapUnits != presentedInfo_.SectionSizeMapUnits ||
                 targetInfo.Width != presentedInfo_.Width || targetInfo.Height != presentedInfo_.Height))
                return false;
            if (!IsTargetBound(candidate) && !BindTarget(candidate))
            {
                const bool restored = RestoreCurrentTarget();
                if (restored && presentedTargetValid_) ShowImage();
                else HideImage();
                return false;
            }
            if (!ApplyGeometry(position, size))
            {
                const bool restored = presentedTargetValid_ && RestoreDisplayedGeometry();
                if (restored) ShowImage();
                else HideImage();
                return false;
            }
            presentedInfo_ = targetInfo;
            presentedTargetValid_ = true;
            return ShowImage();
        }
        if (candidateTarget_ || retiredTarget_ || !candidateTarget_.Acquire(candidate)) return false;
        if (!BindTarget(candidate) || !ApplyGeometry(position, size))
        {
            const bool restored = RestoreCurrentTarget() &&
                (!presentedTargetValid_ || RestoreDisplayedGeometry());
            if (restored && presentedTargetValid_)
                image->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
            else
                HideImage();
            if (!restored && !presentedTargetValid_)
            {
                WithdrawTarget();
                return false;
            }
            if (restored)
            {
                if (!candidateTarget_.Reset())
                    LogRetirementFailure("candidate-target-lease-release");
                else
                    retirementFailureStage_ = nullptr;
            }
            return false;
        }
        const SectionTargetInfo oldInfo = presentedInfo_;
        const bool hadOldTarget = presentedTargetValid_;
        if (target_) retiredTarget_ = std::move(target_);
        target_ = std::move(candidateTarget_);
        presentedInfo_ = targetInfo;
        presentedTargetValid_ = true;
        if (hadOldTarget)
        {
            LOG_INFO("MainMap diagnostics: presentation target changed old=%lld,%lld revision=%llu new=%lld,%lld revision=%llu",
                static_cast<long long>(oldInfo.GridX), static_cast<long long>(oldInfo.GridY),
                static_cast<unsigned long long>(oldInfo.Revision),
                static_cast<long long>(targetInfo.GridX), static_cast<long long>(targetInfo.GridY),
                static_cast<unsigned long long>(targetInfo.Revision));
        }
        else
        {
            LOG_INFO("MainMap diagnostics: presentation target bound grid=%lld,%lld revision=%llu",
                static_cast<long long>(targetInfo.GridX), static_cast<long long>(targetInfo.GridY),
                static_cast<unsigned long long>(targetInfo.Revision));
        }
        if (!ShowImage()) return false;
        RetryRetiredTarget();
        return true;
    }

    /** Restores the previous Section's canvas rectangle after an uncommitted candidate update. */
    bool BuildingMapPresentation::RestoreDisplayedGeometry()
    {
        if (!presentedTargetValid_) return false;
        MainMapBuildingLayer::ConsumerContext context{};
        if (!MainMapBuildingLayer::TryGetConsumerContext(context) || !context.epoch || context.epoch != epoch_)
            return false;
        SDK::UCanvasPanel* canvas = nullptr;
        SDK::UWidgetTree* tree = nullptr;
        if (!MainMapBuildingLayer::TryGetTilePresentationContext(canvas, tree) ||
            !ValidateAttachment(canvas, tree)) return false;
        SDK::FVector2D position{};
        SDK::FVector2D size{};
        return MapSectionGeometry(context, canvas, presentedInfo_, position, size) && ApplyGeometry(position, size);
    }

    /** Hides the image and clears the material target before releasing the current target lease. */
    bool BuildingMapPresentation::WithdrawTarget() noexcept
    {
        HideImage();
        auto* image = Image();
        if (image)
        {
            image->SetBrushFromMaterial(nullptr);
            if (image->Brush.ResourceObject)
            {
                LogRetirementFailure("withdraw-image-brush-still-bound");
                return false;
            }
        }
        materialInstanceDetached_ = true;
        if (materialInstance_ && !materialInstance_.Reset())
        {
            LogRetirementFailure("withdraw-material-instance-lease-release");
            return false;
        }
        materialInstanceDetached_ = false;
        materialInstanceAttempted_ = false;
        materialValidated_ = false;
        presentedTargetValid_ = false;
        presentedInfo_ = {};
        const bool released = ReleaseDetachedResources(false);
        if (released) retirementFailureStage_ = nullptr;
        return released;
    }

    /** Retries release of a target whose material binding has already been replaced or cleared. */
    bool BuildingMapPresentation::RetryRetiredTarget() noexcept
    {
        if (retiredTarget_.Reset())
        {
            retirementFailureStage_ = nullptr;
            return true;
        }
        LogRetirementFailure("retired-target-lease-release");
        return false;
    }

    /** Collapses the image without releasing any object that may still be referenced by Slate. */
    void BuildingMapPresentation::HideImage() noexcept
    {
        auto* image = Image();
        if (!image || image->GetVisibility() == SDK::ESlateVisibility::Collapsed) return;
        image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        LOG_INFO("MainMap diagnostics: presentation hidden or withdrawn");
    }

    /** Sets HitTestInvisible only when needed and confirms the widget accepted visibility. */
    bool BuildingMapPresentation::ShowImage() noexcept
    {
        auto* image = Image();
        if (!image) return false;
        const bool becomingVisible = image->GetVisibility() != SDK::ESlateVisibility::HitTestInvisible;
        if (becomingVisible)
            image->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
        const bool visible = image->GetVisibility() == SDK::ESlateVisibility::HitTestInvisible;
        if (visible && becomingVisible)
        {
            LOG_INFO("MainMap diagnostics: presentation became visible grid=%lld,%lld revision=%llu",
                static_cast<long long>(presentedInfo_.GridX), static_cast<long long>(presentedInfo_.GridY),
                static_cast<unsigned long long>(presentedInfo_.Revision));
        }
        return visible;
    }

    /** Returns the retained dynamic material instance, if available. */
    SDK::UMaterialInstanceDynamic* BuildingMapPresentation::MaterialInstance() const noexcept
    {
        auto* object = materialInstance_.Get();
        return object && object->IsA(SDK::UMaterialInstanceDynamic::StaticClass())
            ? static_cast<SDK::UMaterialInstanceDynamic*>(object) : nullptr;
    }

    /** Returns the retained image widget, if available. */
    SDK::UImage* BuildingMapPresentation::Image() const noexcept
    {
        auto* object = image_.Get();
        return object && object->IsA(SDK::UImage::StaticClass()) ? static_cast<SDK::UImage*>(object) : nullptr;
    }

    /** Logs a changed retirement failure stage once and suppresses identical retry messages. */
    void BuildingMapPresentation::LogRetirementFailure(const char* stage) noexcept
    {
        if (retirementFailureStage_ == stage) return;
        retirementFailureStage_ = stage;
        LOG_WARN("MainMap diagnostics: presentation retirement failed stage=%s epoch=%llu section=%d grid=%lld,%lld revision=%llu",
            stage, static_cast<unsigned long long>(epoch_), presentedTargetValid_ ? 1 : 0,
            static_cast<long long>(presentedInfo_.GridX), static_cast<long long>(presentedInfo_.GridY),
            static_cast<unsigned long long>(presentedInfo_.Revision));
    }

    /** Releases the MID before target leases after its brush reference has been removed. */
    bool BuildingMapPresentation::ReleaseDetachedResources(bool fullRetirement) noexcept
    {
        if (materialInstance_ && !materialInstance_.Reset())
        {
            LogRetirementFailure("material-instance-lease-release");
            return false;
        }
        materialInstanceDetached_ = false;
        retirementFailureStage_ = nullptr;
        if (!target_.Reset())
        {
            LogRetirementFailure("current-target-lease-release");
            return false;
        }
        retirementFailureStage_ = nullptr;
        if (!candidateTarget_.Reset())
        {
            LogRetirementFailure("candidate-target-lease-release");
            return false;
        }
        retirementFailureStage_ = nullptr;
        if (!retiredTarget_.Reset())
        {
            LogRetirementFailure("retired-target-lease-release");
            return false;
        }
        retirementFailureStage_ = nullptr;
        if (fullRetirement && !image_.Reset())
        {
            LogRetirementFailure("image-lease-release");
            return false;
        }
        retirementFailureStage_ = nullptr;
        if (fullRetirement && !parentMaterial_.Reset())
        {
            LogRetirementFailure("parent-material-lease-release");
            return false;
        }
        retirementFailureStage_ = nullptr;
        return true;
    }

    /** Detaches widgets before releasing leases and retains retry state after any failed release. */
    bool BuildingMapPresentation::Retire(bool widgetDestructionStarted) noexcept
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread())
        {
            LogRetirementFailure("native-admission-or-game-thread");
            return false;
        }
        if (!widgetDetached_ && widgetDestructionStarted)
        {
            LogRetirementFailure("widget-destruction-started-before-detachment");
            return false;
        }
        if (!widgetDetached_)
        {
            HideImage();
            auto* image = Image();
            if (image)
            {
                image->RemoveFromParent();
                if (image->GetParent())
                {
                    LogRetirementFailure("image-parent-not-cleared-after-remove");
                    return false;
                }
                if (image->Slot)
                {
                    LogRetirementFailure("image-slot-not-cleared-after-remove");
                    return false;
                }
                image->SetBrushFromMaterial(nullptr);
                if (image->Brush.ResourceObject)
                {
                    LogRetirementFailure("image-brush-still-references-material");
                    return false;
                }
            }
            widgetDetached_ = true;
            materialInstanceDetached_ = true;
        }
        if (!ReleaseDetachedResources(true)) return false;
        presentedInfo_ = {};
        epoch_ = 0;
        materialLoadAttempted_ = false;
        materialInstanceAttempted_ = false;
        materialValidated_ = false;
        presentedTargetValid_ = false;
        widgetDetached_ = false;
        materialInstanceDetached_ = false;
        retirementFailureStage_ = nullptr;
        return true;
    }
}
