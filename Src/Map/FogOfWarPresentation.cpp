// Reparents the game's existing FoW images above composite imagery and restores native ownership at teardown.
#include "FogOfWarPresentation.h"

#include "ChimeraUI_classes.hpp"
#include "Engine_classes.hpp"
#include "plugin_helpers.h"

#include <cmath>
#include <new>

namespace MainMapBuildingMap
{
    /** Returns the single process-lifetime FoW owner used by terrain callbacks and the admitted tick. */
    FogOfWarPresentation& FogOfWarPresentation::SharedOwner() noexcept
    {
        alignas(FogOfWarPresentation) static std::uint8_t storage[sizeof(FogOfWarPresentation)]{};
        static FogOfWarPresentation* presentation = ::new (storage) FogOfWarPresentation();
        return *presentation;
    }

    /** Attaches the existing native FoW widgets after game construction and validates every segment identity. */
    bool FogOfWarPresentation::AttachShared(SDK::UCrUW_MapMenuTerrain* terrain)
    {
        return SharedOwner().Attach(terrain);
    }

    /** Reconciles the active native segment geometry and visibility with the higher FoW layer. */
    FogOfWarMaintenanceResult FogOfWarPresentation::MaintainShared(SDK::UCrUW_MapMenuTerrain* terrain,
        SDK::UCanvasPanel* terrainCanvas)
    {
        return SharedOwner().Maintain(terrain, terrainCanvas);
    }

    /** Restores native image parentage before terrain destruction and releases MainMap-owned presentation objects. */
    bool FogOfWarPresentation::RestoreShared(bool widgetDestructionStarted) noexcept
    {
        return SharedOwner().Restore(widgetDestructionStarted);
    }

    /** Creates a terrain-sized, non-interactive sibling canvas above the Building Map at Z=2. */
    bool FogOfWarPresentation::CreateCanvas(SDK::UWidgetTree* tree,
        SDK::UCanvasPanel* terrainCanvas)
    {
        if (!tree || !terrainCanvas || fowCanvas_) return false;
        auto* object = SDK::UGameplayStatics::SpawnObject(SDK::UCanvasPanel::StaticClass(), tree);
        auto* fowCanvas = static_cast<SDK::UCanvasPanel*>(object);
        if (!fowCanvas || !fowCanvas_.Acquire(fowCanvas)) return false;
        fowCanvas->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
        auto* slot = terrainCanvas->AddChildToCanvas(fowCanvas);
        if (!slot || !fowCanvasSlot_.Acquire(slot)) return false;
        SDK::FAnchorData layout{};
        layout.Anchors = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        layout.Offsets = {};
        layout.Alignment = {0.0f, 0.0f};
        slot->SetLayout(layout);
        slot->SetZOrder(2);
        if (slot->GetZOrder() != 2 || slot->GetAutoSize() ||
            slot->GetAnchors().Maximum.X != 1.0f || slot->GetAnchors().Maximum.Y != 1.0f)
            return false;
        if (!tree_.Acquire(tree) || !terrainCanvas_.Acquire(terrainCanvas)) return false;
        return true;
    }

    /** Captures one native image's original canvas slot, then moves that same image into the FoW layer. */
    bool FogOfWarPresentation::MoveImage(SDK::UCrUW_MapMenuTerrain* terrain, std::size_t index)
    {
        if (!terrain || index >= static_cast<std::size_t>(terrain->TerrainSegments.Num())) return false;
        auto* segment = terrain->TerrainSegments[static_cast<std::int32_t>(index)];
        if (!segment || !segment->ImageFow || !segment->ImageFow->Slot ||
            !segment->ImageFow->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return false;
        auto* image = segment->ImageFow;
        auto* parent = image->GetParent();
        if (!parent) return false;
        auto* originalSlot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        auto& state = images_[index];
        if (!state.segment.Acquire(segment)) return false;
        if (!state.image.Acquire(image) || !state.originalParent.Acquire(parent))
        {
            state.image.Reset();
            state.segment.Reset();
            state.originalParent.Reset();
            return false;
        }
        state.originalLayout = originalSlot->GetLayout();
        state.originalZOrder = originalSlot->GetZOrder();
        state.originalAutoSize = originalSlot->GetAutoSize();
        state.originalVisibility = image->GetVisibility();
        if (!fowCanvas_.Get() || !fowCanvas_.Get()->IsA(SDK::UCanvasPanel::StaticClass())) return false;
        auto* fowCanvas = static_cast<SDK::UCanvasPanel*>(fowCanvas_.Get());
        if (!fowCanvas->AddChildToCanvas(image)) return false;
        ++imageCount_;
        auto* layerSlot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        if (!layerSlot) return false;
        layerSlot->SetAutoSize(false);
        layerSlot->SetAnchors({{0.0f, 0.0f}, {0.0f, 0.0f}});
        layerSlot->SetAlignment({0.0f, 0.0f});
        layerSlot->SetZOrder(0);
        if (layerSlot->GetZOrder() != 0 || layerSlot->GetAutoSize()) return false;
        image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        state.imposedVisibility = SDK::ESlateVisibility::Collapsed;
        state.hasImposedVisibility = true;
        return image->GetVisibility() == SDK::ESlateVisibility::Collapsed;
    }

    /** Attaches the FoW canvas once per terrain identity and rolls back partial moves on failure. */
    bool FogOfWarPresentation::Attach(SDK::UCrUW_MapMenuTerrain* terrain)
    {
        failureLogged_ = false;
        if (!MainMapNative::CanOperate() || !terrain || !terrain->WidgetTree ||
            !terrain->CanvasPanelTerrain || terrain->TerrainSegments.Num() <= 0 ||
            terrain->TerrainSegments.Num() > static_cast<std::int32_t>(images_.size()))
        {
            ReportFailure("attachment-context-invalid");
            return false;
        }
        if (terrain_.Get() == terrain && fowCanvas_) return true;
        if (fowCanvas_ && !Restore(false))
        {
            ReportFailure("previous-presentation-restoration-failed");
            return false;
        }
        if (!terrain_.Acquire(terrain) || !CreateCanvas(terrain->WidgetTree, terrain->CanvasPanelTerrain))
        {
            ReportFailure("layer-creation-or-retention-failed");
            Restore(false);
            return false;
        }
        for (std::int32_t index = 0; index < terrain->TerrainSegments.Num(); ++index)
        {
            if (!MoveImage(terrain, static_cast<std::size_t>(index)))
            {
                ReportFailure("native-image-or-slot-invalid", index);
                Restore(false);
                return false;
            }
        }
        return true;
    }

    /** Synchronizes native widgets, deferring only unavailable Slate geometry. */
    FogOfWarMaintenanceResult FogOfWarPresentation::Maintain(SDK::UCrUW_MapMenuTerrain* terrain,
        SDK::UCanvasPanel* terrainCanvas)
    {
        if (!MainMapNative::CanOperate())
        {
            ReportFailure("native-operation-admission-unavailable");
            return FogOfWarMaintenanceResult::Failed;
        }
        if (!terrain || terrain_.Get() != terrain)
        {
            ReportFailure("terrain-identity-changed");
            return FogOfWarMaintenanceResult::Failed;
        }
        if (!terrainCanvas || terrainCanvas_.Get() != terrainCanvas)
        {
            ReportFailure("terrain-canvas-identity-changed");
            return FogOfWarMaintenanceResult::Failed;
        }
        if (terrain->TerrainSegments.Num() != static_cast<std::int32_t>(imageCount_))
        {
            ReportFailure("terrain-segment-count-changed");
            return FogOfWarMaintenanceResult::Failed;
        }
        bool geometryPending = false;
        for (std::size_t index = 0; index < imageCount_; ++index)
        {
            const auto result = MaintainImage(terrain, terrainCanvas, index);
            if (result == FogOfWarMaintenanceResult::Failed) return result;
            if (result == FogOfWarMaintenanceResult::GeometryPending) geometryPending = true;
        }
        return geometryPending ? FogOfWarMaintenanceResult::GeometryPending
            : FogOfWarMaintenanceResult::Synchronized;
    }

    /** Validates one moved native image and defers it while its Slate geometry is unavailable. */
    FogOfWarMaintenanceResult FogOfWarPresentation::MaintainImage(SDK::UCrUW_MapMenuTerrain* terrain,
        SDK::UCanvasPanel* terrainCanvas, std::size_t index)
    {
        auto& state = images_[index];
        auto* segment = static_cast<SDK::UCrUW_MapMenuTerrainSegment*>(state.segment.Get());
        auto* image = static_cast<SDK::UImage*>(state.image.Get());
        const auto segmentIndex = static_cast<std::int32_t>(index);
        if (!segment || !image || terrain->TerrainSegments[segmentIndex] != segment)
        {
            ReportFailure("native-segment-or-image-identity-invalid", segmentIndex);
            return FogOfWarMaintenanceResult::Failed;
        }
        if (segment->GetParent() != terrainCanvas || image->GetParent() != fowCanvas_.Get())
        {
            ReportFailure("native-widget-parent-changed", segmentIndex);
            return FogOfWarMaintenanceResult::Failed;
        }
        if (!image->Slot || !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()))
        {
            ReportFailure("relocated-image-canvas-slot-invalid", segmentIndex);
            return FogOfWarMaintenanceResult::Failed;
        }
        const auto geometryResult = UpdateImageGeometry(state, terrainCanvas);
        if (geometryResult == GeometryResult::Failed)
        {
            ReportFailure("segment-geometry-slot-synchronization-failed", segmentIndex);
            return FogOfWarMaintenanceResult::Failed;
        }
        if (geometryResult == GeometryResult::Unavailable)
        {
            state.geometryReady = false;
            if (!HideImageUntilGeometry(state))
            {
                ReportFailure("image-hide-before-layout-failed", segmentIndex);
                return FogOfWarMaintenanceResult::Failed;
            }
            return FogOfWarMaintenanceResult::GeometryPending;
        }
        state.geometryReady = true;
        if (!UpdateImageVisibility(state, segment->IsVisible() && segment->IsRendered()))
        {
            ReportFailure("native-effective-visibility-synchronization-failed", segmentIndex);
            return FogOfWarMaintenanceResult::Failed;
        }
        return FogOfWarMaintenanceResult::Synchronized;
    }

    /** Converts native segment bounds and updates the slot only when its geometry has changed. */
    FogOfWarPresentation::GeometryResult FogOfWarPresentation::UpdateImageGeometry(
        ImageState& state, SDK::UCanvasPanel* terrainCanvas)
    {
        auto* segment = static_cast<SDK::UCrUW_MapMenuTerrainSegment*>(state.segment.Get());
        auto* image = static_cast<SDK::UImage*>(state.image.Get());
        if (!segment || !image || !terrainCanvas || !image->Slot ||
            !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return GeometryResult::Failed;
        const auto segmentGeometry = segment->GetPaintSpaceGeometry();
        const auto canvasGeometry = terrainCanvas->GetPaintSpaceGeometry();
        const auto segmentSize = SDK::USlateBlueprintLibrary::GetLocalSize(segmentGeometry);
        const auto canvasSize = SDK::USlateBlueprintLibrary::GetLocalSize(canvasGeometry);
        if (!std::isfinite(segmentSize.X) || !std::isfinite(segmentSize.Y) ||
            !std::isfinite(canvasSize.X) || !std::isfinite(canvasSize.Y) ||
            segmentSize.X <= 0.0f || segmentSize.Y <= 0.0f ||
            canvasSize.X <= 0.0f || canvasSize.Y <= 0.0f) return GeometryResult::Unavailable;
        const auto absoluteTopLeft = SDK::USlateBlueprintLibrary::LocalToAbsolute(segmentGeometry, {0.0f, 0.0f});
        const auto absoluteBottomRight = SDK::USlateBlueprintLibrary::LocalToAbsolute(segmentGeometry, segmentSize);
        const auto topLeft = SDK::USlateBlueprintLibrary::AbsoluteToLocal(canvasGeometry, absoluteTopLeft);
        const auto bottomRight = SDK::USlateBlueprintLibrary::AbsoluteToLocal(canvasGeometry, absoluteBottomRight);
        const SDK::FVector2D position{topLeft.X, topLeft.Y};
        const SDK::FVector2D size{bottomRight.X - topLeft.X, bottomRight.Y - topLeft.Y};
        if (!std::isfinite(position.X) || !std::isfinite(position.Y) ||
            !std::isfinite(size.X) || !std::isfinite(size.Y) || size.X <= 0.0f || size.Y <= 0.0f)
            return GeometryResult::Unavailable;
        auto* slot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        const auto currentPosition = slot->GetPosition();
        const auto currentSize = slot->GetSize();
        const bool positionChanged = currentPosition.X != position.X || currentPosition.Y != position.Y;
        const bool sizeChanged = currentSize.X != size.X || currentSize.Y != size.Y;
        if (positionChanged) slot->SetPosition(position);
        if (sizeChanged) slot->SetSize(size);
        if (slot->GetPosition().X != position.X || slot->GetPosition().Y != position.Y ||
            slot->GetSize().X != size.X || slot->GetSize().Y != size.Y) return GeometryResult::Failed;
        return GeometryResult::Ready;
    }

    /** Hides an image until geometry is valid while retaining its native local visibility value. */
    bool FogOfWarPresentation::HideImageUntilGeometry(ImageState& state)
    {
        auto* image = static_cast<SDK::UImage*>(state.image.Get());
        if (!image) return false;
        const auto currentVisibility = image->GetVisibility();
        if (state.hasImposedVisibility && currentVisibility != state.imposedVisibility)
            state.originalVisibility = currentVisibility;
        if (currentVisibility != SDK::ESlateVisibility::Collapsed)
            image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        state.imposedVisibility = SDK::ESlateVisibility::Collapsed;
        state.hasImposedVisibility = true;
        return image->GetVisibility() == SDK::ESlateVisibility::Collapsed;
    }

    /** Applies native local visibility after geometry is valid and mirrors segment visibility. */
    bool FogOfWarPresentation::UpdateImageVisibility(ImageState& state, bool segmentVisible)
    {
        auto* image = static_cast<SDK::UImage*>(state.image.Get());
        if (!image || !state.geometryReady) return false;
        const auto currentVisibility = image->GetVisibility();
        if (state.hasImposedVisibility && currentVisibility != state.imposedVisibility)
            state.originalVisibility = currentVisibility;
        const auto desiredVisibility = segmentVisible
            ? state.originalVisibility : SDK::ESlateVisibility::Collapsed;
        if (currentVisibility != desiredVisibility) image->SetVisibility(desiredVisibility);
        state.imposedVisibility = desiredVisibility;
        state.hasImposedVisibility = true;
        return image->GetVisibility() == desiredVisibility;
    }

    /** Emits the first concise failure and names the affected native terrain segment when known. */
    void FogOfWarPresentation::ReportFailure(const char* condition, std::int32_t segmentIndex) noexcept
    {
        if (failureLogged_) return;
        failureLogged_ = true;
        if (segmentIndex >= 0)
            LOG_ERROR("MainMap FoW: %s segment=%d; reparenting disabled for this terrain session",
                condition, segmentIndex);
        else
            LOG_ERROR("MainMap FoW: %s; reparenting disabled for this terrain session", condition);
    }

    /** Restores the image to its original parent and recreates the captured native canvas slot layout. */
    bool FogOfWarPresentation::RestoreImage(ImageState& state) noexcept
    {
        auto* image = static_cast<SDK::UWidget*>(state.image.Get());
        auto* originalParent = static_cast<SDK::UPanelWidget*>(state.originalParent.Get());
        if (!image)
        {
            const bool imageReleased = state.image.Reset();
            const bool segmentReleased = state.segment.Reset();
            const bool parentReleased = state.originalParent.Reset();
            return imageReleased && segmentReleased && parentReleased;
        }
        if (!originalParent) return false;
        if (image->GetParent() != originalParent)
        {
            auto* currentParent = image->GetParent();
            if (currentParent && !currentParent->RemoveChild(image)) return false;
            if (!originalParent->AddChild(image)) return false;
        }
        if (!image->Slot || !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return false;
        auto* originalSlot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        originalSlot->SetLayout(state.originalLayout);
        originalSlot->SetAutoSize(state.originalAutoSize);
        originalSlot->SetZOrder(state.originalZOrder);
        image->SetVisibility(state.originalVisibility);
        if (image->GetParent() != originalParent || image->GetVisibility() != state.originalVisibility) return false;
        const bool imageReleased = state.image.Reset();
        const bool segmentReleased = state.segment.Reset();
        const bool parentReleased = state.originalParent.Reset();
        state.hasImposedVisibility = false;
        return imageReleased && segmentReleased && parentReleased;
    }

    /** Restores all moved images before removing the layer and releasing MainMap-owned references. */
    bool FogOfWarPresentation::Restore(bool widgetDestructionStarted) noexcept
    {
        if (!MainMapNative::CanOperate())
        {
            if (!restorationFailureLogged_)
            {
                LOG_ERROR("MainMap FoW: native restoration admission unavailable; retained leases remain owned");
                restorationFailureLogged_ = true;
            }
            return false;
        }
        bool restored = true;
        for (std::size_t index = 0; index < images_.size(); ++index)
            if (!RestoreImage(images_[index])) restored = false;
        if (!restored || !RetireCanvas(widgetDestructionStarted))
        {
            if (!restorationFailureLogged_)
            {
                LOG_ERROR("MainMap FoW: native widget restoration failed; retained leases will be retried during cleanup");
                restorationFailureLogged_ = true;
            }
            return false;
        }
        imageCount_ = 0;
        restorationFailureLogged_ = false;
        return true;
    }

    /** Removes the empty FoW container and releases only MainMap-owned widget references. */
    bool FogOfWarPresentation::RetireCanvas(bool widgetDestructionStarted) noexcept
    {
        auto* canvas = static_cast<SDK::UCanvasPanel*>(fowCanvas_.Get());
        if (canvas && !widgetDestructionStarted && canvas->GetParent())
            if (!canvas->GetParent()->RemoveChild(canvas)) return false;
        if (!fowCanvasSlot_.Reset() || !fowCanvas_.Reset() || !terrainCanvas_.Reset() ||
            !tree_.Reset() || !terrain_.Reset()) return false;
        return true;
    }
}
