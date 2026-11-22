// Presents BuildingMapTarget's six retained composite assignments on the terrain widget's own canvas.
// Each slot's UImage, MID, and target dependencies remain strongly leased until admitted retirement.
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

    /** Withdraws stale grid publications before Stage 3D-C can clear or redraw their target textures. */
    bool BuildingMapPresentation::PrepareGridUpdateShared(std::uint64_t sessionEpoch,
        const std::array<BuildingMapGridPlanner::SlotAssignment,
            BuildingMapGridPlanner::kPhysicalSlotCount>& assignments,
        bool sourceSnapshotCurrent) noexcept
    {
        return SharedOwner().PrepareGridUpdate(sessionEpoch, assignments, sourceSnapshotCurrent);
    }

    /** Publishes eligible composite targets and updates world geometry only when its input changes. */
    bool BuildingMapPresentation::UpdateGridShared(
        const MainMapBuildingLayer::ConsumerContext& context,
        const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree,
        const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
        bool sourceSnapshotCurrent, bool materialResourcesReady)
    {
        return SharedOwner().UpdateGrid(context, visibleWorld, canvas, tree, targets,
            sourceSnapshotCurrent, materialResourcesReady);
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

    /** Retires stale slots before composition and preserves matching clean publications. */
    bool BuildingMapPresentation::PrepareGridUpdate(std::uint64_t sessionEpoch,
        const std::array<BuildingMapGridPlanner::SlotAssignment,
            BuildingMapGridPlanner::kPhysicalSlotCount>& assignments,
        bool sourceSnapshotCurrent) noexcept
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread() || !sessionEpoch) return false;
        if (epoch_ && epoch_ != sessionEpoch && !Retire(false)) return false;
        epoch_ = sessionEpoch;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
        {
            const auto& assignment = assignments[slotIndex];
            if (!assignment.IsAssigned)
            {
                if (!WithdrawGridSlot(slotIndex)) return false;
                ReleaseGridSlotTargets(slotIndex);
                continue;
            }
            const GridPresentationSlot& slot = gridSlots_[slotIndex];
            const bool assignmentChanged = slot.HasPublication &&
                (slot.SessionEpoch != assignment.SessionEpoch ||
                    slot.AssignmentGeneration != assignment.AssignmentGeneration ||
                    slot.Tile.GroupingN != assignment.Tile.GroupingN ||
                    slot.Tile.X != assignment.Tile.X || slot.Tile.Y != assignment.Tile.Y);
            if (!sourceSnapshotCurrent || assignment.IsDirty || assignmentChanged)
            {
                if (!WithdrawGridSlot(slotIndex)) return false;
            }
        }
        return true;
    }

    /** Publishes only committed clean assignments and remaps geometry after viewport changes. */
    bool BuildingMapPresentation::UpdateGrid(const MainMapBuildingLayer::ConsumerContext& context,
        const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree,
        const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
        bool sourceSnapshotCurrent, bool materialResourcesReady)
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread() ||
            !context.world || !context.epoch || !canvas || !tree) return false;
        if (epoch_ && epoch_ != context.epoch && !Retire(false)) return false;
        if (lastGridCanvasIdentity_ != 0 &&
            (lastGridCanvasIdentity_ != reinterpret_cast<std::uintptr_t>(canvas) ||
                lastGridTreeIdentity_ != reinterpret_cast<std::uintptr_t>(tree)))
        {
            if (!Retire(false)) return false;
        }
        epoch_ = context.epoch;
        if (!sourceSnapshotCurrent || !materialResourcesReady)
        {
            for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
            {
                if (!WithdrawGridSlot(slotIndex)) return false;
            }
            return true;
        }
        bool hasEligibleSlot = false;
        for (const auto& targetInfo : targets)
        {
            if (IsGridTargetEligible(context, targetInfo, sourceSnapshotCurrent))
                hasEligibleSlot = true;
        }
        if (hasEligibleSlot && !EnsureGridParentMaterial(context.world, materialResourcesReady))
        {
#if defined(_DEBUG)
            for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
            {
                if (IsGridTargetEligible(context, targets[slotIndex], sourceSnapshotCurrent))
                {
                    ReportGridFailure(slotIndex, targets[slotIndex].Assignment,
                        targets[slotIndex], 7, "presentation-parent-material-failed");
                    break;
                }
            }
#endif
            return false;
        }
        const bool geometryChanged = HasGridGeometryInputChanged(context, visibleWorld, canvas, tree);
        bool succeeded = true;
        for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
        {
            succeeded = UpdateGridSlot(context, canvas, tree, targets[slotIndex],
                sourceSnapshotCurrent, geometryChanged) && succeeded;
        }
        StoreGridGeometryInput(context, visibleWorld, canvas, tree);
        return succeeded;
    }

    /** Loads the packaged UI parent once without constructing the retired single-image MID. */
    bool BuildingMapPresentation::EnsureGridParentMaterial(SDK::UObject* worldContext,
        bool materialResourcesReady)
    {
        if (!worldContext || !materialResourcesReady) return false;
        if (parentMaterial_ && materialValidated_) return true;
        if (!parentMaterial_)
        {
            if (materialLoadAttempted_) return false;
            materialLoadAttempted_ = true;
            auto* parent = MainMapProof::LoadMaterial(
                "/Game/Mods/MainMap/Materials/M_BuildingPresent.M_BuildingPresent",
                SDK::EMaterialDomain::MD_UI, parentMaterial_);
            if (!parent) return false;
        }
        materialValidated_ = true;
        return true;
    }

    /** Updates one slot and hides it if its source, target, widget, or world mapping is invalid. */
    bool BuildingMapPresentation::UpdateGridSlot(
        const MainMapBuildingLayer::ConsumerContext& context, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree, const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        bool sourceSnapshotCurrent, bool geometryChanged)
    {
        const auto& assignment = targetInfo.Assignment;
        const std::size_t slotIndex = assignment.PhysicalSlotIndex;
        if (slotIndex >= gridSlots_.size()) return false;
        if (!IsGridTargetEligible(context, targetInfo, sourceSnapshotCurrent))
            return WithdrawGridSlot(slotIndex);
        if (gridSlots_[slotIndex].retiredTarget) ReleaseRetiredGridTarget(slotIndex);
        const bool alreadyPublished = MatchesGridPublication(slotIndex, targetInfo);
        if (alreadyPublished && !geometryChanged && ValidateGridAttachment(slotIndex, canvas, tree))
            return MaintainStationaryGridSlot(slotIndex, targetInfo);
        SDK::FVector2D position{};
        SDK::FVector2D size{};
        if (!PrepareGridSlotGeometry(context, canvas, tree, targetInfo, position, size)) return false;
        if (alreadyPublished)
            return UpdatePublishedGridGeometry(slotIndex, targetInfo, position, size);
        if (PublishGridSlot(slotIndex, targetInfo, position, size)) return true;
#if defined(_DEBUG)
        ReportGridFailure(slotIndex, assignment, targetInfo, 4, "target-binding-or-publication-failed");
#endif
        WithdrawGridSlot(slotIndex);
        return false;
    }

    /** Ensures the slot attachment and maps world bounds before a new publication or pan/zoom update. */
    bool BuildingMapPresentation::PrepareGridSlotGeometry(
        const MainMapBuildingLayer::ConsumerContext& context, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree, const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        SDK::FVector2D& position, SDK::FVector2D& size)
    {
        const auto& assignment = targetInfo.Assignment;
        const std::size_t slotIndex = assignment.PhysicalSlotIndex;
        if (!EnsureGridSlotWidgets(slotIndex, canvas, tree, context.world, targetInfo))
        {
#if defined(_DEBUG)
            ReportGridFailure(slotIndex, assignment, targetInfo, 1, "widget-attachment-failed");
#endif
            WithdrawGridSlot(slotIndex);
            return false;
        }
        if (!MapGridBounds(context, canvas, assignment.Bounds, position, size))
        {
#if defined(_DEBUG)
            ReportGridFailure(slotIndex, assignment, targetInfo, 2, "geometry-map-failed");
#endif
            WithdrawGridSlot(slotIndex);
            return false;
        }
        return true;
    }

    /** Rebinds only when necessary and retains a clean stationary slot without geometry writes. */
    bool BuildingMapPresentation::MaintainStationaryGridSlot(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo)
    {
        auto* target = static_cast<SDK::UTextureRenderTarget2D*>(targetInfo.Target);
        if (!IsGridTargetBound(slotIndex, target) && !BindGridTarget(slotIndex, target))
        {
#if defined(_DEBUG)
            ReportGridFailure(slotIndex, targetInfo.Assignment, targetInfo, 4, "target-binding-failed");
#endif
            WithdrawGridSlot(slotIndex);
            return false;
        }
        return ShowGridImage(slotIndex);
    }

    /** Applies changed Canvas geometry while keeping the exact content/assignment publication. */
    bool BuildingMapPresentation::UpdatePublishedGridGeometry(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        const SDK::FVector2D& position, const SDK::FVector2D& size)
    {
        if (ApplyGridGeometry(slotIndex, position, size)) return true;
#if defined(_DEBUG)
        ReportGridFailure(slotIndex, targetInfo.Assignment, targetInfo, 3, "geometry-apply-failed");
#endif
        WithdrawGridSlot(slotIndex);
        return false;
    }

    /** Detects the live geometry inputs that can change a world-tile image's canvas rectangle. */
    bool BuildingMapPresentation::HasGridGeometryInputChanged(
        const MainMapBuildingLayer::ConsumerContext& context,
        const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree) const noexcept
    {
        return !hasGridGeometryInput_ || epoch_ != context.epoch ||
            visibleWorld.minX != lastGridVisibleWorld_.minX ||
            visibleWorld.minY != lastGridVisibleWorld_.minY ||
            visibleWorld.maxX != lastGridVisibleWorld_.maxX ||
            visibleWorld.maxY != lastGridVisibleWorld_.maxY ||
            context.originX != lastGridOriginX_ || context.originY != lastGridOriginY_ ||
            reinterpret_cast<std::uintptr_t>(canvas) != lastGridCanvasIdentity_ ||
            reinterpret_cast<std::uintptr_t>(tree) != lastGridTreeIdentity_;
    }

    /** Stores exact successful world and canvas inputs for stationary-frame geometry suppression. */
    void BuildingMapPresentation::StoreGridGeometryInput(
        const MainMapBuildingLayer::ConsumerContext& context,
        const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree) noexcept
    {
        lastGridVisibleWorld_ = visibleWorld;
        lastGridOriginX_ = context.originX;
        lastGridOriginY_ = context.originY;
        lastGridCanvasIdentity_ = reinterpret_cast<std::uintptr_t>(canvas);
        lastGridTreeIdentity_ = reinterpret_cast<std::uintptr_t>(tree);
        hasGridGeometryInput_ = true;
    }

    /** Creates or validates one stable MID and UImage for a physical composite slot. */
    bool BuildingMapPresentation::EnsureGridSlotWidgets(std::size_t slotIndex,
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree, SDK::UObject* worldContext,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo)
    {
        if (slotIndex >= gridSlots_.size() || !canvas || !tree || !worldContext ||
            !parentMaterial_ || !materialValidated_) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (!slot.materialInstance.Get())
        {
            auto* instance = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(
                worldContext, static_cast<SDK::UMaterialInterface*>(parentMaterial_.Get()), {},
                SDK::EMIDCreationFlags::None);
            if (!instance || !instance->IsA(SDK::UMaterialInstanceDynamic::StaticClass()) ||
                !slot.materialInstance.Acquire(instance)) return false;
        }
        auto* materialObject = slot.materialInstance.Get();
        if (!materialObject || !materialObject->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
            return false;
        auto* material = static_cast<SDK::UMaterialInstanceDynamic*>(materialObject);
        if (!slot.image.Get())
        {
            auto* object = SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), tree);
            if (!object || !object->IsA(SDK::UImage::StaticClass()) || object->Outer != tree ||
                !slot.image.Acquire(object)) return false;
            auto* image = static_cast<SDK::UImage*>(object);
            image->SetVisibility(SDK::ESlateVisibility::Collapsed);
            image->SetColorAndOpacity({1, 1, 1, 1});
        }
        auto* image = static_cast<SDK::UImage*>(slot.image.Get());
        if (!material || !image || image->Outer != tree) return false;
        if (image->Brush.ResourceObject != material) image->SetBrushFromMaterial(material);
        if (image->Brush.ResourceObject != material) return false;
#if defined(_DEBUG)
        const bool wasAttached = image->GetParent() == canvas;
#endif
        if (image->GetParent() != canvas)
        {
            if (image->GetParent() || image->Slot) return false;
            auto* canvasSlot = canvas->AddChildToCanvas(image);
            if (!canvasSlot || !canvasSlot->IsA(SDK::UCanvasPanelSlot::StaticClass()) ||
                image->Slot != canvasSlot) return false;
        }
        slot.WidgetDetached = false;
        const bool attached = ValidateGridAttachment(slotIndex, canvas, tree);
#if defined(_DEBUG)
        if (attached && !wasAttached)
        {
            LogGridSlotEvent(slotIndex, targetInfo.Assignment,
                targetInfo.Assignment.CommittedContentGeneration, targetInfo.Target,
                "slot-widget-attached");
        }
#endif
        return attached;
    }

    /** Confirms the expected slot image, tree, canvas parent, CanvasPanelSlot, and MID brush. */
    bool BuildingMapPresentation::ValidateGridAttachment(std::size_t slotIndex,
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree) const noexcept
    {
        if (slotIndex >= gridSlots_.size() || !canvas || !tree) return false;
        const auto& slot = gridSlots_[slotIndex];
        auto* imageObject = slot.image.Get();
        auto* materialObject = slot.materialInstance.Get();
        if (!imageObject || !imageObject->IsA(SDK::UImage::StaticClass()) ||
            !materialObject || !materialObject->IsA(SDK::UMaterialInstanceDynamic::StaticClass())) return false;
        auto* image = static_cast<SDK::UImage*>(imageObject);
        if (image->Outer != tree || image->GetParent() != canvas || !image->Slot ||
            !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) ||
            image->Brush.ResourceObject != materialObject) return false;
        auto* canvasSlot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        return canvasSlot->Parent == canvas && canvasSlot->Content == image;
    }

    /** Maps exact signed composite bounds from terrain world centimetres into canvas local coordinates. */
    bool BuildingMapPresentation::MapGridBounds(
        const MainMapBuildingLayer::ConsumerContext& context, SDK::UCanvasPanel* canvas,
        const BuildingMapGridPlanner::WorldBounds& bounds,
        SDK::FVector2D& position, SDK::FVector2D& size) const
    {
        if (!canvas || bounds.MaxX <= bounds.MinX || bounds.MaxY <= bounds.MinY ||
            !std::isfinite(context.originX) || !std::isfinite(context.originY)) return false;
        auto* treeObject = canvas->Outer;
        if (!treeObject || !treeObject->IsA(SDK::UWidgetTree::StaticClass())) return false;
        auto* tree = static_cast<SDK::UWidgetTree*>(treeObject);
        auto* terrainObject = tree->Outer;
        if (!terrainObject || !terrainObject->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass())) return false;
        auto* terrain = static_cast<SDK::UCrUW_MapMenuTerrain*>(terrainObject);
        if (terrain->WidgetTree != tree || terrain->CanvasPanelTerrain != canvas) return false;
        const double localX = (static_cast<double>(bounds.MinX) - context.originX) / 100.0;
        const double localY = (static_cast<double>(bounds.MinY) - context.originY) / 100.0;
        const double localWidth = static_cast<double>(bounds.MaxX - bounds.MinX) / 100.0;
        const double localHeight = static_cast<double>(bounds.MaxY - bounds.MinY) / 100.0;
        if (!std::isfinite(localX) || !std::isfinite(localY) ||
            !std::isfinite(localWidth) || !std::isfinite(localHeight) ||
            localWidth <= 0.0 || localHeight <= 0.0) return false;
        return MapAbsoluteBoundsToCanvas(canvas->GetPaintSpaceGeometry(),
            terrain->GetPaintSpaceGeometry(), localX, localY, localWidth, localHeight,
            position, size);
    }

    /** Applies explicit top-left position, world-derived size, and established overlay Z order. */
    bool BuildingMapPresentation::ApplyGridGeometry(std::size_t slotIndex,
        const SDK::FVector2D& position, const SDK::FVector2D& size)
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto* imageObject = gridSlots_[slotIndex].image.Get();
        if (!imageObject || !imageObject->IsA(SDK::UImage::StaticClass())) return false;
        auto* image = static_cast<SDK::UImage*>(imageObject);
        if (!image->Slot || !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return false;
        auto* slot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        const auto currentPosition = slot->GetPosition();
        const auto currentSize = slot->GetSize();
        const auto anchors = slot->GetAnchors();
        const auto alignment = slot->GetAlignment();
        if (!slot->GetAutoSize() && slot->GetZOrder() == 1 &&
            anchors.Minimum.X == 0.0f && anchors.Minimum.Y == 0.0f &&
            anchors.Maximum.X == 0.0f && anchors.Maximum.Y == 0.0f &&
            alignment.X == 0.0f && alignment.Y == 0.0f &&
            currentPosition.X == position.X && currentPosition.Y == position.Y &&
            currentSize.X == size.X && currentSize.Y == size.Y) return true;
        slot->SetAutoSize(false);
        slot->SetAnchors({{0, 0}, {0, 0}});
        slot->SetAlignment({0, 0});
        slot->SetPosition(position);
        slot->SetSize(size);
        slot->SetZOrder(1);
        return slot->GetPosition().X == position.X && slot->GetPosition().Y == position.Y &&
            slot->GetSize().X == size.X && slot->GetSize().Y == size.Y &&
            slot->GetZOrder() == 1 && !slot->GetAutoSize();
    }

    /** Binds one composite target to its slot MID and confirms the exact UObject readback. */
    bool BuildingMapPresentation::BindGridTarget(std::size_t slotIndex,
        SDK::UTextureRenderTarget2D* target)
    {
        if (slotIndex >= gridSlots_.size() || !target) return false;
        auto* materialObject = gridSlots_[slotIndex].materialInstance.Get();
        if (!materialObject || !materialObject->IsA(SDK::UMaterialInstanceDynamic::StaticClass())) return false;
        auto* material = static_cast<SDK::UMaterialInstanceDynamic*>(materialObject);
        const auto parameter = SDK::UKismetStringLibrary::Conv_StringToName(
            SDK::FString(L"BuildingTarget"));
        material->SetTextureParameterValue(parameter, target);
        return material->K2_GetTextureParameterValue(parameter) == target;
    }

    /** Reads the slot's BuildingTarget parameter without changing its binding. */
    bool BuildingMapPresentation::IsGridTargetBound(std::size_t slotIndex,
        SDK::UTextureRenderTarget2D* target) const noexcept
    {
        if (slotIndex >= gridSlots_.size() || !target) return false;
        auto* materialObject = gridSlots_[slotIndex].materialInstance.Get();
        if (!materialObject || !materialObject->IsA(SDK::UMaterialInstanceDynamic::StaticClass())) return false;
        auto* material = static_cast<SDK::UMaterialInstanceDynamic*>(materialObject);
        const auto parameter = SDK::UKismetStringLibrary::Conv_StringToName(
            SDK::FString(L"BuildingTarget"));
        return material->K2_GetTextureParameterValue(parameter) == target;
    }

    /** Hides one slot and detaches its material before target clear, reassignment, or retirement. */
    bool BuildingMapPresentation::WithdrawGridSlot(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        if (!HideGridSlotImage(slotIndex) || !DetachGridSlotMaterial(slotIndex)) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (slot.HasPublication)
        {
#if defined(_DEBUG)
            BuildingMapGridPlanner::SlotAssignment assignment{};
            assignment.PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
            assignment.IsAssigned = true;
            assignment.Tile = slot.Tile;
            assignment.SessionEpoch = slot.SessionEpoch;
            assignment.Bounds = slot.Bounds;
            assignment.AssignmentGeneration = slot.AssignmentGeneration;
            assignment.CommittedContentGeneration = slot.ContentGeneration;
            LogGridSlotEvent(slotIndex, assignment, slot.ContentGeneration,
                slot.target.Get(), "withdrawn");
#endif
            slot.HasPublication = false;
            slot.Tile = {};
            slot.Bounds = {};
            slot.SessionEpoch = 0;
            slot.AssignmentGeneration = 0;
            slot.ContentGeneration = 0;
        }
        return true;
    }

    /** Collapses one slot's UImage and rejects a visibility change that did not take effect. */
    bool BuildingMapPresentation::HideGridSlotImage(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto* imageObject = gridSlots_[slotIndex].image.Get();
        if (!imageObject) return true;
        if (!imageObject->IsA(SDK::UImage::StaticClass()))
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 9, imageObject, "widget-validation-failed");
#endif
            return false;
        }
        auto* image = static_cast<SDK::UImage*>(imageObject);
        if (image->GetVisibility() != SDK::ESlateVisibility::Collapsed)
            image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        if (image->GetVisibility() == SDK::ESlateVisibility::Collapsed) return true;
#if defined(_DEBUG)
        ReportGridSlotEvent(slotIndex, 9, imageObject, "widget-hide-failed");
#endif
        return false;
    }

    /** Clears the active UImage brush and releases its MID before target redraw or retirement. */
    bool BuildingMapPresentation::DetachGridSlotMaterial(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto* previousMaterial = gridSlots_[slotIndex].materialInstance.Get();
        auto* imageObject = gridSlots_[slotIndex].image.Get();
        if (imageObject && !imageObject->IsA(SDK::UImage::StaticClass()))
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 9, imageObject, "widget-validation-failed");
#endif
            return false;
        }
        auto* image = static_cast<SDK::UImage*>(imageObject);
        if (previousMaterial && !previousMaterial->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 10, previousMaterial,
                "presentation-material-validation-failed");
#endif
            return false;
        }
        if (!ClearGridSlotBrush(slotIndex, image) || !ReleaseGridSlotMaterial(slotIndex)) return false;
#if defined(_DEBUG)
        if (previousMaterial)
            ReportGridSlotEvent(slotIndex, 0, nullptr, "presentation-mid-detached");
#endif
        return true;
    }

    /** Clears a slot image's material resource and reports whether an old target remained exposed. */
    bool BuildingMapPresentation::ClearGridSlotBrush(std::size_t slotIndex,
        SDK::UImage* image) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        if (!image || !image->Brush.ResourceObject) return true;
#if defined(_DEBUG)
        SDK::UObject* previousTarget = nullptr;
        auto* materialObject = gridSlots_[slotIndex].materialInstance.Get();
        if (materialObject && materialObject->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
        {
            auto* material = static_cast<SDK::UMaterialInstanceDynamic*>(materialObject);
            const auto parameter = SDK::UKismetStringLibrary::Conv_StringToName(
                SDK::FString(L"BuildingTarget"));
            auto* effectiveTarget = material->K2_GetTextureParameterValue(parameter);
            const auto& slot = gridSlots_[slotIndex];
            if (effectiveTarget == slot.target.Get() || effectiveTarget == slot.candidateTarget.Get() ||
                effectiveTarget == slot.retiredTarget.Get()) previousTarget = effectiveTarget;
        }
#endif
        image->SetBrushFromMaterial(nullptr);
        if (!image->Brush.ResourceObject)
        {
#if defined(_DEBUG)
            if (previousTarget)
                ReportGridSlotEvent(slotIndex, 0, previousTarget, "presentation-brush-detached");
#endif
            return true;
        }
#if defined(_DEBUG)
        ReportGridSlotEvent(slotIndex, 10,
            previousTarget ? previousTarget : image->Brush.ResourceObject,
            previousTarget ? "previous-target-still-effectively-bound" :
                "presentation-brush-withdrawal-failed");
#endif
        return false;
    }

    /** Releases a detached slot MID without discarding ownership after a rejected native release. */
    bool BuildingMapPresentation::ReleaseGridSlotMaterial(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto& materialInstance = gridSlots_[slotIndex].materialInstance;
        if (materialInstance.Reset()) return true;
#if defined(_DEBUG)
        ReportGridSlotEvent(slotIndex, 6, materialInstance.Get(), "material-release-deferred");
#endif
        return false;
    }

    /** Publishes an eligible target after preserving the old lease until its MID no longer uses it. */
    bool BuildingMapPresentation::PublishGridSlot(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        const SDK::FVector2D& position, const SDK::FVector2D& size)
    {
        if (slotIndex >= gridSlots_.size() || !targetInfo.Target) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (slot.retiredTarget && !slot.retiredTarget.Reset()) return false;
        auto* retainedTarget = static_cast<SDK::UTextureRenderTarget2D*>(slot.target.Get());
        auto* pendingTarget = static_cast<SDK::UTextureRenderTarget2D*>(slot.candidateTarget.Get());
        const bool targetAssignmentChanged = retainedTarget == targetInfo.Target &&
            slot.HasTargetIdentity && !MatchesGridTargetIdentity(slot, targetInfo.Assignment, false);
#if defined(_DEBUG)
        const bool recycledPhysicalTarget = targetAssignmentChanged ||
            targetInfo.TargetWasReusedForAssignment;
#endif
        if (targetAssignmentChanged && slot.HasPublication) return false;
        if (pendingTarget == targetInfo.Target && slot.HasCandidateIdentity &&
            !MatchesGridTargetIdentity(slot, targetInfo.Assignment, true)) return false;
        if (retainedTarget != targetInfo.Target && pendingTarget != targetInfo.Target)
        {
            if (slot.candidateTarget && !slot.candidateTarget.Reset()) return false;
            slot.HasCandidateIdentity = false;
            if (!slot.candidateTarget.Acquire(targetInfo.Target)) return false;
            StoreGridTargetIdentity(slot, targetInfo.Assignment, true);
        }
        if (!ApplyGridGeometry(slotIndex, position, size) ||
            !BindGridTarget(slotIndex, targetInfo.Target)) return false;
        if (retainedTarget != targetInfo.Target)
        {
            if (slot.target) slot.retiredTarget = std::move(slot.target);
            slot.target = std::move(slot.candidateTarget);
            StoreGridTargetIdentity(slot, targetInfo.Assignment, false);
            slot.HasCandidateIdentity = false;
        }
        else if (!slot.HasTargetIdentity || targetAssignmentChanged)
        {
            StoreGridTargetIdentity(slot, targetInfo.Assignment, false);
        }
        const bool visible = ShowGridImage(slotIndex);
        if (!visible) return false;
#if defined(_DEBUG)
        StoreGridPublication(slotIndex, targetInfo, recycledPhysicalTarget);
#else
        StoreGridPublication(slotIndex, targetInfo);
#endif
        ReleaseRetiredGridTarget(slotIndex);
        return true;
    }

    /** Releases every unbound target lease for a slot without losing rejected ownership records. */
    bool BuildingMapPresentation::ReleaseGridSlotTargets(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (!slot.candidateTarget.Reset())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 5, slot.candidateTarget.Get(),
                "candidate-target-release-deferred");
#endif
            return false;
        }
        slot.HasCandidateIdentity = false;
        if (!slot.retiredTarget.Reset())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 8, slot.retiredTarget.Get(), "retired-target-release-deferred");
#endif
            return false;
        }
        if (!slot.target.Reset())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 11, slot.target.Get(), "target-lease-release-deferred");
#endif
            return false;
        }
        slot.HasTargetIdentity = false;
#if defined(_DEBUG)
        gridSlots_[slotIndex].HasLastFailure = false;
#endif
        return true;
    }

    /** Releases an old target lease once its MID has been rebound to a different target. */
    bool BuildingMapPresentation::ReleaseRetiredGridTarget(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        if (gridSlots_[slotIndex].retiredTarget.Reset()) return true;
#if defined(_DEBUG)
        BuildingMapGridPlanner::SlotAssignment assignment{};
        assignment.PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
        assignment.SessionEpoch = gridSlots_[slotIndex].SessionEpoch;
        assignment.Tile = gridSlots_[slotIndex].Tile;
        assignment.Bounds = gridSlots_[slotIndex].Bounds;
        assignment.AssignmentGeneration = gridSlots_[slotIndex].AssignmentGeneration;
        if (ShouldLogGridFailure(slotIndex, assignment, 8))
            LogGridSlotEvent(slotIndex, assignment, gridSlots_[slotIndex].ContentGeneration,
                gridSlots_[slotIndex].target.Get(), "retired-target-release-deferred");
#endif
        return false;
    }

    /** Detaches one slot and releases its resources only after every UI reference is cleared. */
    bool BuildingMapPresentation::RetireGridSlot(std::size_t slotIndex,
        bool widgetDestructionStarted) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (!slot.WidgetDetached && widgetDestructionStarted && slot.image.Get())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 12, slot.image.Get(), "presentation-retirement-deferred");
#endif
            return false;
        }
        if (!WithdrawGridSlot(slotIndex)) return false;
        if (!DetachGridSlotWidget(slotIndex)) return false;
        if (slot.materialInstance && !slot.materialInstance.Reset())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 6, slot.materialInstance.Get(), "material-release-deferred");
#endif
            return false;
        }
        if (!ReleaseGridSlotTargets(slotIndex)) return false;
        if (slot.image && !slot.image.Reset())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 13, slot.image.Get(), "image-lease-release-deferred");
#endif
            return false;
        }
        slot.WidgetDetached = false;
        return true;
    }

    /** Removes one image and its brush reference before the slot MID and texture leases retire. */
    bool BuildingMapPresentation::DetachGridSlotWidget(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        auto* imageObject = slot.image.Get();
        if (imageObject && !imageObject->IsA(SDK::UImage::StaticClass()))
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 12, imageObject, "presentation-image-validation-failed");
#endif
            return false;
        }
        if (!slot.WidgetDetached && imageObject && imageObject->IsA(SDK::UImage::StaticClass()))
        {
            auto* image = static_cast<SDK::UImage*>(imageObject);
            image->RemoveFromParent();
            if (image->GetParent() || image->Slot)
            {
#if defined(_DEBUG)
                ReportGridSlotEvent(slotIndex, 12, image, "presentation-detach-failed");
#endif
                return false;
            }
            image->SetBrushFromMaterial(nullptr);
            if (image->Brush.ResourceObject)
            {
#if defined(_DEBUG)
                ReportGridSlotEvent(slotIndex, 12, image->Brush.ResourceObject,
                    "presentation-brush-withdrawal-failed");
#endif
                return false;
            }
            slot.WidgetDetached = true;
        }
        return true;
    }

    /** Retires every presentation slot before the shared parent material can be released. */
    bool BuildingMapPresentation::RetireGridSlots(bool widgetDestructionStarted) noexcept
    {
        bool retired = true;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
        {
            if (!RetireGridSlot(slotIndex, widgetDestructionStarted)) retired = false;
        }
        return retired;
    }

    /** Verifies assignment, world bounds, accepted commit state, and the target's allocation contract. */
    bool BuildingMapPresentation::IsGridTargetEligible(
        const MainMapBuildingLayer::ConsumerContext& context,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        bool sourceSnapshotCurrent) const noexcept
    {
        const auto& assignment = targetInfo.Assignment;
        auto* target = targetInfo.Target;
        if (!sourceSnapshotCurrent || !assignment.IsAssigned || assignment.PhysicalSlotIndex >= gridSlots_.size() ||
            assignment.SessionEpoch != context.epoch || !assignment.HasCommittedContent || assignment.IsDirty ||
            !targetInfo.TargetMatchesAssignment || !target ||
            !target->IsA(SDK::UTextureRenderTarget2D::StaticClass())) return false;
        return target->SizeX == 1024 && target->SizeY == 1024 &&
            target->RenderTargetFormat == SDK::ETextureRenderTargetFormat::RTF_RGBA16f &&
            target->bForceLinearGamma != 0 && target->bAutoGenerateMips == 0 &&
            target->ClearColor.R == 0.0f && target->ClearColor.G == 0.0f &&
            target->ClearColor.B == 0.0f && target->ClearColor.A == 1.0f &&
            assignment.Bounds.MaxX > assignment.Bounds.MinX &&
            assignment.Bounds.MaxY > assignment.Bounds.MinY;
    }

    /** Matches the published world key and CPU content identity to one current target record. */
    bool BuildingMapPresentation::MatchesGridPublication(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo) const noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        const auto& slot = gridSlots_[slotIndex];
        const auto& assignment = targetInfo.Assignment;
        return slot.HasPublication && slot.SessionEpoch == assignment.SessionEpoch &&
            slot.AssignmentGeneration == assignment.AssignmentGeneration &&
            slot.ContentGeneration == assignment.CommittedContentGeneration &&
            slot.Tile.GroupingN == assignment.Tile.GroupingN &&
            slot.Tile.X == assignment.Tile.X && slot.Tile.Y == assignment.Tile.Y &&
            slot.Bounds.MinX == assignment.Bounds.MinX && slot.Bounds.MinY == assignment.Bounds.MinY &&
            slot.Bounds.MaxX == assignment.Bounds.MaxX && slot.Bounds.MaxY == assignment.Bounds.MaxY &&
            slot.target.Get() == targetInfo.Target && slot.HasTargetIdentity &&
            MatchesGridTargetIdentity(slot, assignment, false);
    }

    /** Compares the world/session identity associated with a retained target lease. */
    bool BuildingMapPresentation::MatchesGridTargetIdentity(const GridPresentationSlot& slot,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        bool candidateTarget) const noexcept
    {
        const auto& tile = candidateTarget ? slot.CandidateTile : slot.TargetTile;
        const auto& bounds = candidateTarget ? slot.CandidateBounds : slot.TargetBounds;
        const std::uint64_t epoch = candidateTarget ? slot.CandidateSessionEpoch : slot.TargetSessionEpoch;
        const std::uint64_t generation = candidateTarget
            ? slot.CandidateAssignmentGeneration : slot.TargetAssignmentGeneration;
        return epoch == assignment.SessionEpoch && generation == assignment.AssignmentGeneration &&
            tile.GroupingN == assignment.Tile.GroupingN && tile.X == assignment.Tile.X &&
            tile.Y == assignment.Tile.Y && bounds.MinX == assignment.Bounds.MinX &&
            bounds.MinY == assignment.Bounds.MinY && bounds.MaxX == assignment.Bounds.MaxX &&
            bounds.MaxY == assignment.Bounds.MaxY;
    }

    /** Records the separate assignment identity owned by a current or candidate target lease. */
    void BuildingMapPresentation::StoreGridTargetIdentity(GridPresentationSlot& slot,
        const BuildingMapGridPlanner::SlotAssignment& assignment, bool candidateTarget) noexcept
    {
        if (candidateTarget)
        {
            slot.CandidateTile = assignment.Tile;
            slot.CandidateBounds = assignment.Bounds;
            slot.CandidateSessionEpoch = assignment.SessionEpoch;
            slot.CandidateAssignmentGeneration = assignment.AssignmentGeneration;
            slot.HasCandidateIdentity = true;
            return;
        }
        slot.TargetTile = assignment.Tile;
        slot.TargetBounds = assignment.Bounds;
        slot.TargetSessionEpoch = assignment.SessionEpoch;
        slot.TargetAssignmentGeneration = assignment.AssignmentGeneration;
        slot.HasTargetIdentity = true;
    }

    /** Sets HitTestInvisible on a slot image and confirms Slate accepted the visible state. */
    bool BuildingMapPresentation::ShowGridImage(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto* imageObject = gridSlots_[slotIndex].image.Get();
        if (!imageObject || !imageObject->IsA(SDK::UImage::StaticClass())) return false;
        auto* image = static_cast<SDK::UImage*>(imageObject);
        if (image->GetVisibility() != SDK::ESlateVisibility::HitTestInvisible)
            image->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
        return image->GetVisibility() == SDK::ESlateVisibility::HitTestInvisible;
    }

    /** Stores the exact published assignment and content identity after the slot becomes visible. */
    void BuildingMapPresentation::StoreGridPublication(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo
#if defined(_DEBUG)
        , bool recycledPhysicalTarget
#endif
        ) noexcept
    {
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        const auto& assignment = targetInfo.Assignment;
        slot.Tile = assignment.Tile;
        slot.Bounds = assignment.Bounds;
        slot.SessionEpoch = assignment.SessionEpoch;
        slot.AssignmentGeneration = assignment.AssignmentGeneration;
        slot.ContentGeneration = assignment.CommittedContentGeneration;
        slot.HasPublication = true;
#if defined(_DEBUG)
        const char* event = recycledPhysicalTarget ? "recycled-target-published" :
            (gridSessionRetired_ ? "republished-after-reopen" :
                (firstGridCompositeLogged_ ? "grid-slot-published" : "first-visible-composite"));
        LogGridSlotEvent(slotIndex, assignment, assignment.CommittedContentGeneration,
            slot.target.Get(), event);
        firstGridCompositeLogged_ = true;
        gridSessionRetired_ = false;
        gridSlots_[slotIndex].HasLastFailure = false;
#endif
    }

#if defined(_DEBUG)
    /** Logs a source-independent presentation failure only when its slot identity changes. */
    void BuildingMapPresentation::ReportGridFailure(std::size_t slotIndex,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        std::uint8_t failureKind, const char* event) noexcept
    {
        if (!ShouldLogGridFailure(slotIndex, assignment, failureKind)) return;
        LogGridSlotEvent(slotIndex, assignment, assignment.CommittedContentGeneration,
            targetInfo.Target, event);
    }

    /** Suppresses a repeated failure for the same physical assignment and failure category. */
    bool BuildingMapPresentation::ShouldLogGridFailure(std::size_t slotIndex,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        std::uint8_t failureKind) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto& slot = gridSlots_[slotIndex];
        if (slot.HasLastFailure &&
            slot.LastFailureAssignmentGeneration == assignment.AssignmentGeneration &&
            slot.LastFailureKind == failureKind) return false;
        slot.LastFailureAssignmentGeneration = assignment.AssignmentGeneration;
        slot.LastFailureKind = failureKind;
        slot.HasLastFailure = true;
        return true;
    }

    /** Logs a bounded slot transition with whichever publication or target identity remains retained. */
    void BuildingMapPresentation::ReportGridSlotEvent(std::size_t slotIndex,
        std::uint8_t failureKind, SDK::UObject* target, const char* event) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return;
        const GridPresentationSlot& slot = gridSlots_[slotIndex];
        BuildingMapGridPlanner::SlotAssignment assignment{};
        assignment.PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
        if (slot.HasPublication)
        {
            assignment.IsAssigned = true;
            assignment.Tile = slot.Tile;
            assignment.Bounds = slot.Bounds;
            assignment.SessionEpoch = slot.SessionEpoch;
            assignment.AssignmentGeneration = slot.AssignmentGeneration;
            assignment.CommittedContentGeneration = slot.ContentGeneration;
        }
        else if (slot.HasTargetIdentity)
        {
            assignment.IsAssigned = true;
            assignment.Tile = slot.TargetTile;
            assignment.Bounds = slot.TargetBounds;
            assignment.SessionEpoch = slot.TargetSessionEpoch;
            assignment.AssignmentGeneration = slot.TargetAssignmentGeneration;
        }
        else if (slot.HasCandidateIdentity)
        {
            assignment.IsAssigned = true;
            assignment.Tile = slot.CandidateTile;
            assignment.Bounds = slot.CandidateBounds;
            assignment.SessionEpoch = slot.CandidateSessionEpoch;
            assignment.AssignmentGeneration = slot.CandidateAssignmentGeneration;
        }
        if (failureKind != 0 && !ShouldLogGridFailure(slotIndex, assignment, failureKind)) return;
        LogGridSlotEvent(slotIndex, assignment, assignment.CommittedContentGeneration, target, event);
    }

    /** Emits presentation identity and bounds for an existing slot event. */
    void BuildingMapPresentation::LogGridSlotEvent(std::size_t slotIndex,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        std::uint64_t contentGeneration, SDK::UObject* target, const char* event) noexcept
    {
        LOG_INFO("MainMap grid presentation: event=%s epoch=%llu slot=%u N=%lld tile=%lld,%lld assignment=%llu content=%llu target=%p bounds-cm=%lld,%lld,%lld,%lld",
            event ? event : "state", static_cast<unsigned long long>(assignment.SessionEpoch),
            static_cast<unsigned int>(slotIndex), static_cast<long long>(assignment.Tile.GroupingN),
            static_cast<long long>(assignment.Tile.X), static_cast<long long>(assignment.Tile.Y),
            static_cast<unsigned long long>(assignment.AssignmentGeneration),
            static_cast<unsigned long long>(contentGeneration), static_cast<void*>(target),
            static_cast<long long>(assignment.Bounds.MinX),
            static_cast<long long>(assignment.Bounds.MinY),
            static_cast<long long>(assignment.Bounds.MaxX),
            static_cast<long long>(assignment.Bounds.MaxY));
    }

#endif

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
        return MapAbsoluteBoundsToCanvas(canvasGeometry, terrainGeometry, localX, localY,
            localSize, localSize, position, size);
    }

    /** Converts both world-rectangle corners between Slate geometries and rejects invalid float slot values. */
    bool BuildingMapPresentation::MapAbsoluteBoundsToCanvas(const SDK::FGeometry& canvasGeometry,
        const SDK::FGeometry& terrainGeometry, double localX, double localY,
        double localWidth, double localHeight,
        SDK::FVector2D& position, SDK::FVector2D& size) const
    {
        const double limit = (std::numeric_limits<float>::max)();
        if (!std::isfinite(localX + localWidth) || !std::isfinite(localY + localHeight) ||
            std::abs(localX) > limit || std::abs(localY) > limit ||
            std::abs(localX + localWidth) > limit || std::abs(localY + localHeight) > limit)
            return false;
        const auto absoluteTopLeft = SDK::USlateBlueprintLibrary::LocalToAbsolute(terrainGeometry,
            {static_cast<float>(localX), static_cast<float>(localY)});
        const auto absoluteBottomRight = SDK::USlateBlueprintLibrary::LocalToAbsolute(terrainGeometry,
            {static_cast<float>(localX + localWidth), static_cast<float>(localY + localHeight)});
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
        const std::uint64_t retiringEpoch = epoch_;
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread())
        {
            LogRetirementFailure("native-admission-or-game-thread");
            return false;
        }
        if (!RetireGridSlots(widgetDestructionStarted))
        {
            return false;
        }
        if (!RetireLegacySingleSection(widgetDestructionStarted)) return false;
        ResetPresentationSession();
#if defined(_DEBUG)
        BuildingMapGridPlanner::SlotAssignment assignment{};
        LogGridSlotEvent(gridSlots_.size(), assignment, 0, nullptr, "cleanup-complete");
        gridSessionRetired_ = retiringEpoch != 0;
#endif
        return true;
    }

    /** Detaches and releases the inactive single-image resource set after grid UI withdrawal. */
    bool BuildingMapPresentation::RetireLegacySingleSection(bool widgetDestructionStarted) noexcept
    {
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
        return ReleaseDetachedResources(true);
    }

    /** Clears cached session and geometry identities after all presentation leases are released. */
    void BuildingMapPresentation::ResetPresentationSession() noexcept
    {
        presentedInfo_ = {};
        epoch_ = 0;
        materialLoadAttempted_ = false;
        materialInstanceAttempted_ = false;
        materialValidated_ = false;
        presentedTargetValid_ = false;
        widgetDetached_ = false;
        materialInstanceDetached_ = false;
        retirementFailureStage_ = nullptr;
        hasGridGeometryInput_ = false;
        lastGridCanvasIdentity_ = 0;
        lastGridTreeIdentity_ = 0;
    }
}
