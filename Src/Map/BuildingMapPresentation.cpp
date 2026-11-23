// Presents six retained composite assignments as cropped images inside native terrain segments.
// Segment associations control native fog visibility and remain leased through admitted withdrawal and retirement.
#include "BuildingMapPresentation.h"

#include "ChimeraUI_classes.hpp"
#include "Config/Config.h"
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "MaterialProof.h"
#include "SlateCore_structs.hpp"
#include "UMG_classes.hpp"
#include "plugin_helpers.h"

#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <algorithm>

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

    /** Publishes eligible composites after reconciling changed segment and target bounds. */
    bool BuildingMapPresentation::UpdateGridShared(
        const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree,
        const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
        bool sourceSnapshotCurrent, bool materialResourcesReady)
    {
        return SharedOwner().UpdateGrid(context, canvas, tree, targets,
            sourceSnapshotCurrent, materialResourcesReady);
    }

    /** Maintains only the native fog preference while the building overlay is disabled. */
    bool BuildingMapPresentation::UpdateNativeFogShared(
        const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree)
    {
        return SharedOwner().UpdateNativeFog(context, canvas, tree);
    }

    /** Detaches owned overlap images and releases the presentation owner through admitted operations. */
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

    /** Publishes clean composites after reconciling segment associations and changed target bounds. */
    bool BuildingMapPresentation::UpdateGrid(const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas,
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
        if (!ReconcilePresentationGeometry(context, canvas, tree, targets)) return false;
        if (!sourceSnapshotCurrent || !materialResourcesReady)
        {
            const bool withdrawn = WithdrawAllGridSlots();
            if (withdrawn) ReportGridConsumerCounts();
            return withdrawn;
        }
        if (!EnsureParentForEligibleTargets(context, targets, sourceSnapshotCurrent, materialResourcesReady))
            return false;
        bool succeeded = true;
        for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
        {
            succeeded = UpdateGridSlot(context, canvas, tree, targets[slotIndex],
                sourceSnapshotCurrent) && succeeded;
        }
        StoreGridAttachmentIdentity(canvas, tree);
        ReportGridConsumerCounts();
        return succeeded;
    }

    /** Validates the current native segment set and applies HideFoW without building work. */
    bool BuildingMapPresentation::UpdateNativeFog(
        const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree)
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread() ||
            !context.world || !context.epoch || !canvas || !tree) return false;
        if (epoch_ && epoch_ != context.epoch && !Retire(false)) return false;
        epoch_ = context.epoch;
        if (!ReconcileNativeSegments(context, canvas, tree))
        {
            RestoreNativeFogPresentation();
            return false;
        }
        StoreGridAttachmentIdentity(canvas, tree);
        return ApplyNativeFogPreference(Config::Get().HideFoW);
    }

    /** Reconciles native and overlap geometry, reporting an invalid segment association once per failure period. */
    bool BuildingMapPresentation::ReconcilePresentationGeometry(
        const MainMapBuildingLayer::ConsumerContext& context, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree,
        const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets)
    {
        if (!ReconcileNativeSegments(context, canvas, tree))
        {
            RestoreNativeFogPresentation();
            if (!WithdrawAllGridSlots()) return false;
            if (!nativeAssociationFailureLogged_)
            {
                auto* terrainObject = tree->Outer;
                std::int32_t count = -1;
                if (terrainObject && terrainObject->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass()))
                    count = static_cast<SDK::UCrUW_MapMenuTerrain*>(terrainObject)->TerrainSegments.Num();
                LOG_ERROR("MainMap Building Map: native segment association rejected terrain=%p count=%d segment=%d",
                    terrainObject, count, rejectedSegmentIndex_);
                nativeAssociationFailureLogged_ = true;
            }
            return false;
        }
        nativeAssociationFailureLogged_ = false;
        ApplyNativeFogPreference(Config::Get().HideFoW);
        return ReconcileOverlapGeometry(targets);
    }

    /** Hides and detaches every physical target's consumers before composition eligibility is lost. */
    bool BuildingMapPresentation::WithdrawAllGridSlots() noexcept
    {
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
            if (!WithdrawGridSlot(slotIndex)) return false;
        return true;
    }

    /** Loads the shared parent material only if a current committed target is eligible for display. */
    bool BuildingMapPresentation::EnsureParentForEligibleTargets(
        const MainMapBuildingLayer::ConsumerContext& context,
        const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
        bool sourceSnapshotCurrent, bool materialResourcesReady)
    {
        bool hasEligibleTarget = false;
        for (const auto& targetInfo : targets)
            if (IsGridTargetEligible(context, targetInfo, sourceSnapshotCurrent)) hasEligibleTarget = true;
        if (!hasEligibleTarget || EnsureGridParentMaterial(context.world, materialResourcesReady)) return true;
#if defined(_DEBUG)
        for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
        {
            if (!IsGridTargetEligible(context, targets[slotIndex], sourceSnapshotCurrent)) continue;
            ReportGridFailure(slotIndex, targets[slotIndex].Assignment,
                targets[slotIndex], 7, "presentation-parent-material-failed");
            break;
        }
#endif
        return false;
    }

    /** Associates each live segment with terrain data and validates both untouched native image parents. */
    bool BuildingMapPresentation::ReconcileNativeSegments(
        const MainMapBuildingLayer::ConsumerContext& context,
        SDK::UCanvasPanel* terrainCanvas, SDK::UWidgetTree* terrainTree)
    {
        auto* terrainObject = terrainTree ? terrainTree->Outer : nullptr;
        auto* settings = SDK::UCrMapMenuDevSettings::GetDefaultObj();
        auto* data = settings ? settings->TerrainData.Get() : nullptr;
        if (!terrainObject || !terrainObject->IsA(SDK::UCrUW_MapMenuTerrain::StaticClass()) ||
            !data || !data->IsA(SDK::UCrMapMenuTerrainData::StaticClass())) return false;
        auto* terrain = static_cast<SDK::UCrUW_MapMenuTerrain*>(terrainObject);
        const double originX = data->MapTerrainTopLeftPivotPoint.X;
        const double originY = data->MapTerrainTopLeftPivotPoint.Y;
        const double segmentWidth = data->MapTerrainSegmentSize.X;
        const double segmentHeight = data->MapTerrainSegmentSize.Y;
        if (terrain->WidgetTree != terrainTree || terrain->CanvasPanelTerrain != terrainCanvas ||
            data->TerrainSegmentsData.Num() != terrain->TerrainSegments.Num() ||
            terrain->TerrainSegments.Num() <= 0 || terrain->TerrainSegments.Num() > 36 ||
            context.epoch != epoch_ || context.originX != originX || context.originY != originY ||
            !std::isfinite(originX) || !std::isfinite(originY) || segmentWidth <= 0 || segmentHeight <= 0)
            return false;
        if (AreNativeSegmentAssociationsCurrent(terrain, data, terrainTree, context,
            originX, originY, segmentWidth, segmentHeight)) return true;
        rejectedSegmentIndex_ = -1;
        if (!RetireNativeSegmentAssociations()) return false;
        nativeSegments_.reserve(static_cast<std::size_t>(terrain->TerrainSegments.Num()));
        for (std::int32_t i = 0; i < terrain->TerrainSegments.Num(); ++i)
        {
            auto* segment = terrain->TerrainSegments[i];
            if (!segment || !TryAssociateNativeSegment(segment, terrainCanvas, data,
                originX, originY, segmentWidth, segmentHeight, nativeSegments_))
            {
                rejectedSegmentIndex_ = i;
                return false;
            }
        }
        for (auto& overlap : overlapImages_) overlap.geometryReady = false;
        nativeSegmentWidth_ = segmentWidth;
        nativeSegmentHeight_ = segmentHeight;
        overlapGeometryReady_ = false;
        rejectedSegmentIndex_ = -1;
        LOG_INFO("MainMap Building Map: native segment associations=%llu",
            static_cast<unsigned long long>(nativeSegments_.size()));
        return true;
    }

    /** Reuses the cached association only while every native segment and its two original images remain linked. */
    bool BuildingMapPresentation::AreNativeSegmentAssociationsCurrent(
        SDK::UCrUW_MapMenuTerrain* terrain, SDK::UCrMapMenuTerrainData* data,
        SDK::UWidgetTree* tree, const MainMapBuildingLayer::ConsumerContext& context,
        double originX, double originY, double segmentWidth, double segmentHeight) const
    {
        if (nativeSegments_.size() != static_cast<std::size_t>(terrain->TerrainSegments.Num()) ||
            lastGridTreeIdentity_ != reinterpret_cast<std::uintptr_t>(tree) ||
            context.originX != originX || context.originY != originY ||
            nativeSegmentWidth_ != segmentWidth || nativeSegmentHeight_ != segmentHeight) return false;
        for (std::int32_t i = 0; i < terrain->TerrainSegments.Num(); ++i)
        {
            auto* segment = terrain->TerrainSegments[i];
            const NativeSegmentAssociation* associationForSegment = nullptr;
            for (const auto& association : nativeSegments_)
                if (association.segment.Get() == segment) associationForSegment = &association;
            if (!associationForSegment || !segment || !segment->ImageTerrain || !segment->ImageFow ||
                !segment->ImageTerrain->Slot || !segment->ImageFow->Slot ||
                !segment->ImageTerrain->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) ||
                !segment->ImageFow->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) ||
                segment->ImageTerrain->GetParent() != segment->ImageFow->GetParent() ||
                !segment->ImageTerrain->GetParent() ||
                !segment->ImageTerrain->GetParent()->IsA(SDK::UCanvasPanel::StaticClass()) ||
                segment->GetParent() != terrain->CanvasPanelTerrain) return false;
            if (associationForSegment->canvas.Get() != segment->ImageTerrain->GetParent()) return false;
            if (associationForSegment->terrainImage.Get() != segment->ImageTerrain ||
                associationForSegment->fogImage.Get() != segment->ImageFow ||
                associationForSegment->segmentTree.Get() != segment->WidgetTree ||
                associationForSegment->segmentSlot.Get() != segment->Slot ||
                associationForSegment->terrainImageSlot.Get() != segment->ImageTerrain->Slot ||
                associationForSegment->fogImageSlot.Get() != segment->ImageFow->Slot) return false;
            if (!segment->Slot || !segment->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return false;
            auto* segmentSlot = static_cast<SDK::UCanvasPanelSlot*>(segment->Slot);
            const auto position = segmentSlot->GetPosition();
            const auto size = segmentSlot->GetSize();
            const double gridX = associationForSegment->gridX * segmentWidth;
            const double gridY = associationForSegment->gridY * segmentHeight;
            const bool slotMatchesData = std::abs(position.X - gridX) <= 0.01 &&
                std::abs(position.Y - gridY) <= 0.01 &&
                std::abs(size.X - segmentWidth) <= 0.01 &&
                std::abs(size.Y - segmentHeight) <= 0.01;
            if (!slotMatchesData) return false;
        }
        return data->TerrainSegmentsData.Num() == terrain->TerrainSegments.Num();
    }

    /** Withdraws all target consumers and retires only owned images before a segment tree is replaced. */
    bool BuildingMapPresentation::RetireNativeSegmentAssociations() noexcept
    {
        if (nativeSegments_.empty()) return true;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
            if (!WithdrawGridSlot(slotIndex)) return false;
        for (auto& overlap : overlapImages_)
        {
            auto* image = static_cast<SDK::UImage*>(overlap.image.Get());
            if (image)
            {
                image->SetVisibility(SDK::ESlateVisibility::Collapsed);
                image->SetBrushFromMaterial(nullptr);
                if (image->GetVisibility() != SDK::ESlateVisibility::Collapsed || image->Brush.ResourceObject)
                    return false;
                if (image->GetParent() && !image->GetParent()->RemoveChild(image)) return false;
                if (image->Slot || image->GetParent()) return false;
            }
            if (!overlap.image.Reset() || !overlap.outerTree.Reset() || !overlap.canvas.Reset()) return false;
        }
        if (!ReleaseNativeSegmentAssociations()) return false;
        for (auto& slot : gridSlots_) slot.overlapMembers.clear();
        overlapGeometryReady_ = false;
        return true;
    }

    /** Releases each retained native object explicitly so rejected releases stay retryable. */
    bool BuildingMapPresentation::ReleaseNativeSegmentAssociations() noexcept
    {
        if (!RestoreNativeFogPresentation()) return false;
        for (auto& association : nativeSegments_)
        {
            if (!association.terrainImage.Reset() || !association.fogImage.Reset() ||
                !association.terrainImageSlot.Reset() || !association.fogImageSlot.Reset() ||
                !association.segmentSlot.Reset() || !association.canvas.Reset() ||
                !association.segmentTree.Reset() ||
                !association.segment.Reset()) return false;
        }
        nativeSegments_.clear();
        nativeFogPreferenceObserved_ = false;
        return true;
    }

    /** Applies HideFoW to the validated native fog images and skips unchanged preferences. */
    bool BuildingMapPresentation::ApplyNativeFogPreference(bool hideFog) noexcept
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread()) return false;
        if (nativeFogPreferenceObserved_ && nativeFogPreference_ == hideFog) return true;
        if (!hideFog)
        {
            if (!RestoreNativeFogPresentation()) return false;
            nativeFogPreference_ = false;
            nativeFogPreferenceObserved_ = true;
            return true;
        }

        for (auto& association : nativeSegments_)
        {
            if (association.hasOriginalFogVisibility) continue;
            auto* object = association.fogImage.Get();
            if (!IsLiveNativeFogImage(object)) return false;
            auto* image = static_cast<SDK::UImage*>(object);
            association.originalFogVisibility = image->GetVisibility();
            association.hasOriginalFogVisibility = true;
            if (association.originalFogVisibility != SDK::ESlateVisibility::Hidden)
                image->SetVisibility(SDK::ESlateVisibility::Hidden);
            if (image->GetVisibility() != SDK::ESlateVisibility::Hidden)
            {
                association.hasOriginalFogVisibility = false;
                return false;
            }
        }
        nativeFogPreference_ = true;
        nativeFogPreferenceObserved_ = true;
        return true;
    }

    /** Restores every still-live fog image without dereferencing destroyed UObjects. */
    bool BuildingMapPresentation::RestoreNativeFogPresentation() noexcept
    {
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread()) return false;
        for (auto& association : nativeSegments_)
        {
            if (!association.hasOriginalFogVisibility) continue;
            auto* object = association.fogImage.Get();
            if (!IsLiveNativeFogImage(object))
            {
                association.hasOriginalFogVisibility = false;
                continue;
            }
            auto* image = static_cast<SDK::UImage*>(object);
            if (image->GetVisibility() != association.originalFogVisibility)
                image->SetVisibility(association.originalFogVisibility);
            if (image->GetVisibility() != association.originalFogVisibility) return false;
            association.hasOriginalFogVisibility = false;
        }
        nativeFogPreference_ = false;
        nativeFogPreferenceObserved_ = false;
        return true;
    }

    /** Confirms a retained fog image is a live UImage before using its typed visibility API. */
    bool BuildingMapPresentation::IsLiveNativeFogImage(SDK::UObject* object) const noexcept
    {
        if (!object) return false;
        const auto invalid = SDK::EObjectFlags::ClassDefaultObject | SDK::EObjectFlags::ArchetypeObject |
            SDK::EObjectFlags::InheritableComponentTemplate | SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed;
        auto* current = object;
        for (int depth = 0; current && depth < 32; ++depth, current = current->Outer)
            if ((static_cast<unsigned>(current->Flags) & static_cast<unsigned>(invalid)) != 0)
                return false;
        if (current) return false;
        return object->IsA(SDK::UImage::StaticClass());
    }

    /** Matches one live segment's actual canvas slot against its native grid record without using array order. */
    bool BuildingMapPresentation::TryAssociateNativeSegment(
        SDK::UCrUW_MapMenuTerrainSegment* segment, SDK::UCanvasPanel* terrainCanvas,
        SDK::UCrMapMenuTerrainData* data, double originX, double originY,
        double segmentWidth, double segmentHeight,
        std::vector<NativeSegmentAssociation>& associations)
    {
        if (!segment->IsA(SDK::UCrUW_MapMenuTerrainSegment::StaticClass()) ||
            !segment->ImageTerrain || !segment->ImageFow || !segment->ImageTerrain->Slot ||
            !segment->ImageFow->Slot ||
            !segment->ImageTerrain->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) ||
            !segment->ImageFow->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()) ||
            segment->ImageTerrain->GetParent() != segment->ImageFow->GetParent() ||
            !segment->ImageTerrain->GetParent() ||
            !segment->ImageTerrain->GetParent()->IsA(SDK::UCanvasPanel::StaticClass()) ||
            segment->GetParent() != terrainCanvas || !segment->Slot ||
            !segment->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return false;
        auto* slot = static_cast<SDK::UCanvasPanelSlot*>(segment->Slot);
        const auto position = slot->GetPosition();
        const auto size = slot->GetSize();
        for (std::int32_t i = 0; i < data->TerrainSegmentsData.Num(); ++i)
        {
            const auto& record = data->TerrainSegmentsData[i];
            const double expectedX = record.TerrainSegmentGridIndex.X * segmentWidth;
            const double expectedY = record.TerrainSegmentGridIndex.Y * segmentHeight;
            if (std::abs(position.X - expectedX) > 0.01 || std::abs(position.Y - expectedY) > 0.01 ||
                std::abs(size.X - segmentWidth) > 0.01 || std::abs(size.Y - segmentHeight) > 0.01) continue;
            return StoreNativeSegmentAssociation(segment, record.TerrainSegmentGridIndex.X,
                record.TerrainSegmentGridIndex.Y, originX, originY, expectedX, expectedY,
                segmentWidth, segmentHeight, associations);
        }
        return false;
    }

    /** Retains the matched native segment and stores its world rectangle from the loaded terrain data. */
    bool BuildingMapPresentation::StoreNativeSegmentAssociation(
        SDK::UCrUW_MapMenuTerrainSegment* segment, std::int32_t gridX, std::int32_t gridY,
        double originX, double originY, double expectedX, double expectedY,
        double segmentWidth, double segmentHeight,
        std::vector<NativeSegmentAssociation>& associations)
    {
        for (const auto& existing : associations)
            if (existing.gridX == gridX && existing.gridY == gridY) return false;
        associations.emplace_back();
        auto& association = associations.back();
        if (!association.segment.Acquire(segment) ||
            !association.segmentTree.Acquire(segment->WidgetTree) ||
            !association.terrainImage.Acquire(segment->ImageTerrain) ||
            !association.fogImage.Acquire(segment->ImageFow) ||
            !association.segmentSlot.Acquire(segment->Slot) ||
            !association.terrainImageSlot.Acquire(segment->ImageTerrain->Slot) ||
            !association.fogImageSlot.Acquire(segment->ImageFow->Slot) ||
            !association.canvas.Acquire(segment->ImageTerrain->GetParent())) return false;
        association.gridX = gridX;
        association.gridY = gridY;
        association.bounds = {originX + expectedX * 100.0, originY + expectedY * 100.0,
            originX + (expectedX + segmentWidth) * 100.0,
            originY + (expectedY + segmentHeight) * 100.0};
        return true;
    }

    /** Rebuilds pooled overlap geometry only when segment associations or physical assignments change. */
    bool BuildingMapPresentation::ReconcileOverlapGeometry(
        const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets)
    {
        bool changed = !overlapGeometryReady_;
        for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
        {
            const auto& assignment = targets[slotIndex].Assignment;
            if (assignment.IsAssigned && assignment.PhysicalSlotIndex != slotIndex) return false;
            const bool assigned = assignment.IsAssigned;
            const auto& bounds = assignment.Bounds;
            if (overlapTargetAssigned_[slotIndex] != assigned ||
                (assigned && (overlapTargetBounds_[slotIndex].MinX != bounds.MinX ||
                    overlapTargetBounds_[slotIndex].MinY != bounds.MinY ||
                    overlapTargetBounds_[slotIndex].MaxX != bounds.MaxX ||
                    overlapTargetBounds_[slotIndex].MaxY != bounds.MaxY))) changed = true;
        }
        if (!changed) return true;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
            if (gridSlots_[slotIndex].HasPublication && !WithdrawGridSlot(slotIndex)) return false;
        for (auto& slot : gridSlots_) slot.overlapMembers.clear();
        for (auto& overlap : overlapImages_)
        {
            overlap.active = false;
            overlap.geometryReady = false;
            overlap.layoutApplied = false;
        }
        std::size_t poolIndex = 0;
        for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
        {
            const auto& assignment = targets[slotIndex].Assignment;
            if (assignment.IsAssigned && assignment.PhysicalSlotIndex != slotIndex) return false;
            if (!assignment.IsAssigned) continue;
            if (!AddTargetOverlapMembership(slotIndex, assignment, poolIndex)) return false;
            overlapTargetAssigned_[slotIndex] = true;
            overlapTargetBounds_[slotIndex] = assignment.Bounds;
        }
        for (std::size_t slotIndex = 0; slotIndex < targets.size(); ++slotIndex)
            if (!targets[slotIndex].Assignment.IsAssigned) overlapTargetAssigned_[slotIndex] = false;
        overlapGeometryReady_ = true;
        return true;
    }

    /** Adds each nonempty half-open intersection to the bounded pool and target membership list. */
    bool BuildingMapPresentation::AddTargetOverlapMembership(std::size_t slotIndex,
        const BuildingMapGridPlanner::SlotAssignment& assignment, std::size_t& poolIndex)
    {
        for (std::size_t segmentIndex = 0; segmentIndex < nativeSegments_.size(); ++segmentIndex)
        {
            const auto& segment = nativeSegments_[segmentIndex];
            const double minX = (std::max)(static_cast<double>(assignment.Bounds.MinX), segment.bounds.minX);
            const double minY = (std::max)(static_cast<double>(assignment.Bounds.MinY), segment.bounds.minY);
            const double maxX = (std::min)(static_cast<double>(assignment.Bounds.MaxX), segment.bounds.maxX);
            const double maxY = (std::min)(static_cast<double>(assignment.Bounds.MaxY), segment.bounds.maxY);
            if (maxX <= minX || maxY <= minY) continue;
            if (poolIndex == overlapImages_.size()) overlapImages_.emplace_back();
            auto& overlap = overlapImages_[poolIndex];
            if (overlap.targetSlot != slotIndex || overlap.segmentIndex != segmentIndex)
                overlap.geometryReady = false;
            overlap.targetSlot = slotIndex;
            overlap.segmentIndex = segmentIndex;
            overlap.active = true;
            gridSlots_[slotIndex].overlapMembers.push_back(poolIndex++);
        }
        return true;
    }

    /** Creates or reuses a MainMap image and applies the half-open world intersection's crop and anchors. */
    bool BuildingMapPresentation::ApplyOverlap(OverlapImage& overlap, GridPresentationSlot& targetSlot,
        const NativeSegmentAssociation& segment, const BuildingMapGridPlanner::WorldBounds& targetBounds,
        const char*& failureOperation)
    {
        failureOperation = nullptr;
        auto* segmentObject = static_cast<SDK::UCrUW_MapMenuTerrainSegment*>(segment.segment.Get());
        auto* segmentTree = segmentObject ? segmentObject->WidgetTree : nullptr;
        auto* canvas = static_cast<SDK::UCanvasPanel*>(segment.canvas.Get());
        if (!segmentObject || !segmentTree || !canvas || !targetSlot.materialInstance.Get() ||
            !EnsureOverlapImage(overlap, segmentTree, segment, failureOperation))
        {
            if (!failureOperation) failureOperation = "segment-widget-or-target-mid-validation";
            return false;
        }
        if (!CacheOverlapGeometry(overlap, segment, targetBounds))
        {
            failureOperation = "overlap-geometry-validation";
            return false;
        }
        auto* image = static_cast<SDK::UImage*>(overlap.image.Get());
        auto* slot = static_cast<SDK::UCanvasPanelSlot*>(image->Slot);
        if (!ApplyOverlapLayout(overlap, slot, failureOperation)) return false;
        auto* material = targetSlot.materialInstance.Get();
        const bool consumerStateChanged = image->Brush.ResourceObject != material ||
            image->GetVisibility() != SDK::ESlateVisibility::HitTestInvisible;
        if (!SetOverlapBrush(overlap, image, material))
        {
            failureOperation = "shared-mid-brush-or-uv-writeback";
            return false;
        }
        overlap.layoutApplied = true;
        image->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
        if (image->GetVisibility() != SDK::ESlateVisibility::HitTestInvisible)
        {
            failureOperation = "overlap-image-visibility";
            return false;
        }
        if (consumerStateChanged) consumerCountReportPending_ = true;
        failureOperation = nullptr;
        return true;
    }

    /** Applies and verifies the complete stretch layout expected for one segment overlap. */
    bool BuildingMapPresentation::ApplyOverlapLayout(OverlapImage& overlap,
        SDK::UCanvasPanelSlot* slot, const char*& failureOperation)
    {
        if (!overlap.layoutApplied)
        {
            slot->SetLayout({{}, {{overlap.anchorMinimumX, overlap.anchorMinimumY},
                {overlap.anchorMaximumX, overlap.anchorMaximumY}}, {0, 0}});
            slot->SetZOrder(0);
            slot->SetAutoSize(false);
            slot->SetAlignment({0, 0});
        }
        const auto anchors = slot->GetAnchors();
        const auto offsets = slot->GetOffsets();
        const auto alignment = slot->GetAlignment();
        if (slot->GetZOrder() == 0 && !slot->GetAutoSize() &&
            anchors.Minimum.X == overlap.anchorMinimumX &&
            anchors.Minimum.Y == overlap.anchorMinimumY &&
            anchors.Maximum.X == overlap.anchorMaximumX &&
            anchors.Maximum.Y == overlap.anchorMaximumY && offsets.Left == 0 && offsets.Top == 0 &&
            offsets.Right == 0 && offsets.Bottom == 0 && alignment.X == 0 && alignment.Y == 0)
            return true;
        failureOperation = "canvas-slot-layout-writeback";
        return false;
    }

    /** Sets a complete white image brush and verifies its shared MID and crop survived SetBrush. */
    bool BuildingMapPresentation::SetOverlapBrush(
        OverlapImage& overlap, SDK::UImage* image, SDK::UObject* material)
    {
        if (!image || !material || !overlap.geometryReady) return false;
        SDK::FBox2f uvRegion{};
        uvRegion.Min = {overlap.uvMinimumX, overlap.uvMinimumY};
        uvRegion.Max = {overlap.uvMaximumX, overlap.uvMaximumY};
        uvRegion.bIsValid = true;
        SDK::FSlateBrush brush = image->Brush;
        brush.ResourceObject = material;
        brush.DrawAs = SDK::ESlateBrushDrawType::Image;
        brush.Tiling = SDK::ESlateBrushTileType::NoTile;
        brush.Mirroring = SDK::ESlateBrushMirrorType::NoMirror;
        brush.ImageType = SDK::ESlateBrushImageType::FullColor;
        brush.ImageSize.X = 1024;
        brush.ImageSize.Y = 1024;
        brush.Margin = {};
        brush.TintColor.SpecifiedColor = {1, 1, 1, 1};
        brush.TintColor.ColorUseRule = SDK::ESlateColorStylingMode::UseColor_Specified;
        brush.UVRegion = uvRegion;
        image->SetBrush(brush);
        image->SetColorAndOpacity({1, 1, 1, 1});
        return image->Brush.ResourceObject == material && image->Brush.UVRegion.bIsValid &&
            image->Brush.UVRegion.Min.X == overlap.uvMinimumX &&
            image->Brush.UVRegion.Min.Y == overlap.uvMinimumY &&
            image->Brush.UVRegion.Max.X == overlap.uvMaximumX &&
            image->Brush.UVRegion.Max.Y == overlap.uvMaximumY;
    }

    /** Caches normalized segment anchors and composite UV bounds for one half-open intersection. */
    bool BuildingMapPresentation::CacheOverlapGeometry(OverlapImage& overlap,
        const NativeSegmentAssociation& segment,
        const BuildingMapGridPlanner::WorldBounds& targetBounds)
    {
        if (overlap.geometryReady) return true;
        const double minX = (std::max)(static_cast<double>(targetBounds.MinX), segment.bounds.minX);
        const double minY = (std::max)(static_cast<double>(targetBounds.MinY), segment.bounds.minY);
        const double maxX = (std::min)(static_cast<double>(targetBounds.MaxX), segment.bounds.maxX);
        const double maxY = (std::min)(static_cast<double>(targetBounds.MaxY), segment.bounds.maxY);
        if (maxX <= minX || maxY <= minY || targetBounds.MaxX <= targetBounds.MinX ||
            targetBounds.MaxY <= targetBounds.MinY) return false;
        overlap.anchorMinimumX = static_cast<float>((minX - segment.bounds.minX) /
            (segment.bounds.maxX - segment.bounds.minX));
        overlap.anchorMinimumY = static_cast<float>((minY - segment.bounds.minY) /
            (segment.bounds.maxY - segment.bounds.minY));
        overlap.anchorMaximumX = static_cast<float>((maxX - segment.bounds.minX) /
            (segment.bounds.maxX - segment.bounds.minX));
        overlap.anchorMaximumY = static_cast<float>((maxY - segment.bounds.minY) /
            (segment.bounds.maxY - segment.bounds.minY));
        overlap.uvMinimumX = static_cast<float>((minX - targetBounds.MinX) /
            (static_cast<double>(targetBounds.MaxX) - targetBounds.MinX));
        overlap.uvMinimumY = static_cast<float>((minY - targetBounds.MinY) /
            (static_cast<double>(targetBounds.MaxY) - targetBounds.MinY));
        overlap.uvMaximumX = static_cast<float>((maxX - targetBounds.MinX) /
            (static_cast<double>(targetBounds.MaxX) - targetBounds.MinX));
        overlap.uvMaximumY = static_cast<float>((maxY - targetBounds.MinY) /
            (static_cast<double>(targetBounds.MaxY) - targetBounds.MinY));
        overlap.geometryReady = true;
        return true;
    }

    /** Retains a pooled UImage under the segment tree and appends it to the native child canvas. */
    bool BuildingMapPresentation::EnsureOverlapImage(OverlapImage& overlap, SDK::UWidgetTree* tree,
        const NativeSegmentAssociation& segment, const char*& failureOperation)
    {
        auto* canvasObject = segment.canvas.Get();
        if (!tree || tree != segment.segmentTree.Get() || !canvasObject ||
            !canvasObject->IsA(SDK::UCanvasPanel::StaticClass()))
        {
            failureOperation = "destination-segment-tree-or-canvas-validation";
            return false;
        }
        auto* canvas = static_cast<SDK::UCanvasPanel*>(canvasObject);
        if (!overlap.image.Get() && !CreateOverlapImage(overlap, tree, failureOperation)) return false;
        auto* image = static_cast<SDK::UImage*>(overlap.image.Get());
        auto* originalOuter = overlap.outerTree.Get();
        if (!originalOuter && image->Outer && image->Outer->IsA(SDK::UWidgetTree::StaticClass()))
        {
            if (!overlap.outerTree.Acquire(image->Outer))
            {
                failureOperation = "original-widget-tree-retention";
                return false;
            }
            originalOuter = overlap.outerTree.Get();
        }
        if (!originalOuter || image->Outer != originalOuter ||
            !originalOuter->IsA(SDK::UWidgetTree::StaticClass()))
        {
            failureOperation = "mainmap-image-outer-ownership-validation";
            return false;
        }
        return AttachOverlapImage(overlap, image, canvas, failureOperation);
    }

    /** Creates a pooled image once and retains its MainMap lease independently of segment attachment. */
    bool BuildingMapPresentation::CreateOverlapImage(OverlapImage& overlap, SDK::UWidgetTree* tree,
        const char*& failureOperation)
    {
        overlap.layoutApplied = false;
        auto* object = SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), tree);
        if (!object || !object->IsA(SDK::UImage::StaticClass()) || object->Outer != tree ||
            !overlap.image.Acquire(object))
        {
            failureOperation = "mainmap-image-creation-or-retention";
            return false;
        }
        auto* image = static_cast<SDK::UImage*>(object);
        image->SetColorAndOpacity({1, 1, 1, 1});
        image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        return true;
    }

    /** Attaches a detached pooled image and transfers its retained canvas identity. */
    bool BuildingMapPresentation::AttachOverlapImage(OverlapImage& overlap, SDK::UImage* image,
        SDK::UCanvasPanel* canvas, const char*& failureOperation)
    {
        if (!MoveOverlapImageToCanvas(overlap, image, canvas, failureOperation)) return false;
        if (overlap.canvas.Get() != canvas)
        {
            if (overlap.canvas && !overlap.canvas.Reset())
            {
                failureOperation = "release-previous-canvas-lease";
                return false;
            }
            if (!overlap.canvas.Acquire(canvas))
            {
                failureOperation = "retain-destination-canvas";
                return false;
            }
        }
        if (image->GetParent() == canvas && image->Slot &&
            image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass())) return true;
        failureOperation = "destination-parent-and-slot-validation";
        return false;
    }

    /** Moves only a brush-detached image and verifies the new panel and slot identities. */
    bool BuildingMapPresentation::MoveOverlapImageToCanvas(OverlapImage& overlap, SDK::UImage* image,
        SDK::UCanvasPanel* canvas, const char*& failureOperation)
    {
        if (image->GetParent() == canvas) return true;
        if (image->Brush.ResourceObject)
        {
            failureOperation = "detach-shared-mid-before-reparent";
            return false;
        }
        image->SetVisibility(SDK::ESlateVisibility::Collapsed);
        if (image->GetVisibility() != SDK::ESlateVisibility::Collapsed)
        {
            failureOperation = "hide-before-reparent";
            return false;
        }
        auto* oldParent = image->GetParent();
        if (oldParent && !oldParent->RemoveChild(image))
        {
            failureOperation = "remove-from-previous-panel";
            return false;
        }
        if (image->GetParent() || image->Slot)
        {
            failureOperation = "verify-previous-parent-and-slot-cleared";
            return false;
        }
        if (!canvas->AddChildToCanvas(image) || !image->Slot ||
            !image->Slot->IsA(SDK::UCanvasPanelSlot::StaticClass()))
        {
            failureOperation = "attach-to-destination-canvas";
            return false;
        }
        overlap.layoutApplied = false;
        return true;
    }

    /** Hides and clears all brush consumers before releasing a target MID. */
    bool BuildingMapPresentation::WithdrawOverlapImages(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        bool withdrawn = true;
        for (const auto index : gridSlots_[slotIndex].overlapMembers)
        {
            if (index >= overlapImages_.size()) { withdrawn = false; continue; }
            auto* imageObject = overlapImages_[index].image.Get();
            if (!imageObject || !imageObject->IsA(SDK::UImage::StaticClass())) continue;
            auto* image = static_cast<SDK::UImage*>(imageObject);
            if (image->GetVisibility() != SDK::ESlateVisibility::Collapsed || image->Brush.ResourceObject)
                consumerCountReportPending_ = true;
            image->SetVisibility(SDK::ESlateVisibility::Collapsed);
            image->SetBrushFromMaterial(nullptr);
            if (image->GetVisibility() != SDK::ESlateVisibility::Collapsed || image->Brush.ResourceObject)
                withdrawn = false;
        }
        return withdrawn;
    }

    /** Removes and releases only MainMap-owned overlap images before native segment destruction. */
    bool BuildingMapPresentation::RetireOverlapImages() noexcept
    {
        bool retired = true;
        for (auto& overlap : overlapImages_)
        {
            auto* image = static_cast<SDK::UImage*>(overlap.image.Get());
            if (image)
            {
                image->SetVisibility(SDK::ESlateVisibility::Collapsed);
                image->SetBrushFromMaterial(nullptr);
                if (image->GetVisibility() != SDK::ESlateVisibility::Collapsed) retired = false;
                auto* parent = image->GetParent();
                if (parent && !parent->RemoveChild(image)) retired = false;
                if (image->Brush.ResourceObject || image->GetParent() || image->Slot) retired = false;
            }
            if (!retired) continue;
            if (!overlap.image.Reset() || !overlap.outerTree.Reset() || !overlap.canvas.Reset()) retired = false;
        }
        if (retired)
        {
            if (!ReleaseNativeSegmentAssociations()) return false;
            overlapImages_.clear();
            for (auto& slot : gridSlots_) slot.overlapMembers.clear();
            overlapGeometryReady_ = false;
            loggedActiveOverlapCount_ = 0;
            hasLoggedActiveOverlapCount_ = false;
            hasLoggedConsumerCount_.fill(false);
            consumerCountReportPending_ = false;
            nativeAssociationFailureLogged_ = false;
        }
        return retired;
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

    /** Updates one slot and hides its consumers if source or target publication checks fail. */
    bool BuildingMapPresentation::UpdateGridSlot(
        const MainMapBuildingLayer::ConsumerContext& context, SDK::UCanvasPanel* canvas,
        SDK::UWidgetTree* tree, const BuildingMapTarget::CompositeTargetInfo& targetInfo,
        bool sourceSnapshotCurrent)
    {
        const auto& assignment = targetInfo.Assignment;
        const std::size_t slotIndex = assignment.PhysicalSlotIndex;
        if (slotIndex >= gridSlots_.size()) return false;
        if (!IsGridTargetEligible(context, targetInfo, sourceSnapshotCurrent))
            return WithdrawGridSlot(slotIndex);
        if (gridSlots_[slotIndex].retiredTarget) ReleaseRetiredGridTarget(slotIndex);
        const bool alreadyPublished = MatchesGridPublication(slotIndex, targetInfo);
        if (alreadyPublished)
            return MaintainStationaryGridSlot(slotIndex, targetInfo);
        if (!EnsureGridSlotWidgets(slotIndex, canvas, tree, context.world, targetInfo)) return false;
        if (PublishGridSlot(slotIndex, targetInfo)) return true;
#if defined(_DEBUG)
        ReportGridFailure(slotIndex, assignment, targetInfo, 4, "target-binding-or-publication-failed");
#endif
        if (!WithdrawGridSlot(slotIndex)) return false;
        return false;
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
        return IsGridTargetBound(slotIndex, target);
    }

    /** Caches native parent identities for terrain reconstruction detection. */
    void BuildingMapPresentation::StoreGridAttachmentIdentity(
        SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree) noexcept
    {
        lastGridCanvasIdentity_ = reinterpret_cast<std::uintptr_t>(canvas);
        lastGridTreeIdentity_ = reinterpret_cast<std::uintptr_t>(tree);
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
        return material && slot.materialInstance.Get() == material && slot.materialInstance;
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
            slot.HasPublication = false;
            slot.Tile = {};
            slot.Bounds = {};
            slot.SessionEpoch = 0;
            slot.AssignmentGeneration = 0;
            slot.ContentGeneration = 0;
        }
        return true;
    }

    /** Hides every overlap image for a physical slot and verifies the full consumer fan-out. */
    bool BuildingMapPresentation::HideGridSlotImage(std::size_t slotIndex) noexcept
    {
        if (WithdrawOverlapImages(slotIndex))
        {
            return true;
        }
#if defined(_DEBUG)
        ReportGridSlotEvent(slotIndex, 9, gridSlots_[slotIndex].target.Get(), "overlap-image-withdrawal-failed");
#endif
        return false;
    }

    /** Clears the active UImage brush and releases its MID before target redraw or retirement. */
    bool BuildingMapPresentation::DetachGridSlotMaterial(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto* previousMaterial = gridSlots_[slotIndex].materialInstance.Get();
        if (previousMaterial && !previousMaterial->IsA(SDK::UMaterialInstanceDynamic::StaticClass()))
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 10, previousMaterial,
                "presentation-material-validation-failed");
#endif
            return false;
        }
        if (!ClearGridSlotBrush(slotIndex) || !ReleaseGridSlotMaterial(slotIndex)) return false;
        return true;
    }

    /** Clears a slot image's material resource and reports whether an old target remained exposed. */
    bool BuildingMapPresentation::ClearGridSlotBrush(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
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
        if (WithdrawOverlapImages(slotIndex))
        {
            return true;
        }
#if defined(_DEBUG)
        ReportGridSlotEvent(slotIndex, 10,
            previousTarget ? previousTarget : nullptr,
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
        const BuildingMapTarget::CompositeTargetInfo& targetInfo)
    {
        if (slotIndex >= gridSlots_.size() || !targetInfo.Target) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (slot.retiredTarget && !slot.retiredTarget.Reset()) return false;
        auto* retainedTarget = static_cast<SDK::UTextureRenderTarget2D*>(slot.target.Get());
        auto* pendingTarget = static_cast<SDK::UTextureRenderTarget2D*>(slot.candidateTarget.Get());
        const bool targetAssignmentChanged = retainedTarget == targetInfo.Target &&
            slot.HasTargetIdentity && !MatchesGridTargetIdentity(slot, targetInfo.Assignment, false);
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
        if (!BindGridTarget(slotIndex, targetInfo.Target)) return false;
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
        if (!BindGridOverlapConsumers(slotIndex, targetInfo) || !ShowGridImage(slotIndex)) return false;
        StoreGridPublication(slotIndex, targetInfo);
        ReleaseRetiredGridTarget(slotIndex);
        return true;
    }

    /** Binds every overlap image to this slot MID after its new target publication is committed. */
    bool BuildingMapPresentation::BindGridOverlapConsumers(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo)
    {
        if (slotIndex >= gridSlots_.size()) return false;
        auto& slot = gridSlots_[slotIndex];
        for (const auto overlapIndex : slot.overlapMembers)
        {
            if (overlapIndex >= overlapImages_.size()) return false;
            auto& overlap = overlapImages_[overlapIndex];
            const auto segmentIndex = overlap.segmentIndex;
            if (segmentIndex >= nativeSegments_.size()) return false;
            const auto& segment = nativeSegments_[segmentIndex];
            const char* failureOperation = nullptr;
            if (!ApplyOverlap(overlap, slot, segment, targetInfo.Assignment.Bounds, failureOperation))
            {
                if (overlap.lastFailureSessionEpoch != targetInfo.Assignment.SessionEpoch ||
                    overlap.lastFailureAssignmentGeneration != targetInfo.Assignment.AssignmentGeneration ||
                    overlap.lastFailureSegmentX != segment.gridX ||
                    overlap.lastFailureSegmentY != segment.gridY ||
                    overlap.lastFailureOperation != failureOperation)
                {
                    LOG_ERROR("MainMap Building Map: overlap consumer failed target-slot=%llu segment-grid=(%d,%d) assignment=%llu operation=%s",
                        static_cast<unsigned long long>(slotIndex), segment.gridX, segment.gridY,
                        static_cast<unsigned long long>(targetInfo.Assignment.AssignmentGeneration),
                        failureOperation ? failureOperation : "unknown-overlap-operation");
                    overlap.lastFailureSessionEpoch = targetInfo.Assignment.SessionEpoch;
                    overlap.lastFailureAssignmentGeneration = targetInfo.Assignment.AssignmentGeneration;
                    overlap.lastFailureSegmentX = segment.gridX;
                    overlap.lastFailureSegmentY = segment.gridY;
                    overlap.lastFailureOperation = failureOperation;
                }
                return false;
            }
            overlap.lastFailureOperation = nullptr;
        }
        return !slot.overlapMembers.empty();
    }

    /** Reports consumer counts only after all target slots reach the update's final state. */
    void BuildingMapPresentation::ReportGridConsumerCounts() noexcept
    {
        if (!consumerCountReportPending_) return;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
            ReportTargetConsumerCount(slotIndex);
        ReportActiveOverlapCount();
        consumerCountReportPending_ = false;
    }

    /** Logs the visible MainMap overlap image count only when it changes. */
    void BuildingMapPresentation::ReportActiveOverlapCount() noexcept
    {
        std::size_t activeCount = 0;
        for (const auto& overlap : overlapImages_)
        {
            auto* imageObject = overlap.image.Get();
            if (!overlap.active || !imageObject || !imageObject->IsA(SDK::UImage::StaticClass())) continue;
            auto* image = static_cast<SDK::UImage*>(imageObject);
            if (image->Brush.ResourceObject && image->GetVisibility() == SDK::ESlateVisibility::HitTestInvisible)
                ++activeCount;
        }
        if (hasLoggedActiveOverlapCount_ && loggedActiveOverlapCount_ == activeCount) return;
        LOG_INFO("MainMap Building Map: active overlap images=%llu",
            static_cast<unsigned long long>(activeCount));
        loggedActiveOverlapCount_ = activeCount;
        hasLoggedActiveOverlapCount_ = true;
    }

    /** Counts images that visibly reference one physical slot's shared presentation MID. */
    void BuildingMapPresentation::ReportTargetConsumerCount(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return;
        std::size_t consumerCount = 0;
        auto* material = gridSlots_[slotIndex].materialInstance.Get();
        for (const auto index : gridSlots_[slotIndex].overlapMembers)
        {
            if (index >= overlapImages_.size()) continue;
            auto* imageObject = overlapImages_[index].image.Get();
            if (!imageObject || !imageObject->IsA(SDK::UImage::StaticClass())) continue;
            auto* image = static_cast<SDK::UImage*>(imageObject);
            if (material && image->Brush.ResourceObject == material &&
                image->GetVisibility() == SDK::ESlateVisibility::HitTestInvisible) ++consumerCount;
        }
        if (hasLoggedConsumerCount_[slotIndex] && loggedConsumerCounts_[slotIndex] == consumerCount) return;
        LOG_INFO("MainMap Building Map: target slot=%llu attached consumers=%llu",
            static_cast<unsigned long long>(slotIndex), static_cast<unsigned long long>(consumerCount));
        loggedConsumerCounts_[slotIndex] = consumerCount;
        hasLoggedConsumerCount_[slotIndex] = true;
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
    bool BuildingMapPresentation::RetireGridSlot(std::size_t slotIndex) noexcept
    {
        if (slotIndex >= gridSlots_.size()) return false;
        GridPresentationSlot& slot = gridSlots_[slotIndex];
        if (!WithdrawGridSlot(slotIndex)) return false;
        if (slot.materialInstance && !slot.materialInstance.Reset())
        {
#if defined(_DEBUG)
            ReportGridSlotEvent(slotIndex, 6, slot.materialInstance.Get(), "material-release-deferred");
#endif
            return false;
        }
        if (!ReleaseGridSlotTargets(slotIndex)) return false;
        return true;
    }

    /** Retires every presentation slot before the shared parent material can be released. */
    bool BuildingMapPresentation::RetireGridSlots() noexcept
    {
        bool withdrawn = true;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
            if (!WithdrawGridSlot(slotIndex)) withdrawn = false;
        if (!withdrawn || !RetireOverlapImages()) return false;
        bool retired = true;
        for (std::size_t slotIndex = 0; slotIndex < gridSlots_.size(); ++slotIndex)
        {
            if (!RetireGridSlot(slotIndex)) retired = false;
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
        if (slotIndex >= gridSlots_.size() || gridSlots_[slotIndex].overlapMembers.empty()) return false;
        for (const auto index : gridSlots_[slotIndex].overlapMembers)
        {
            if (index >= overlapImages_.size()) return false;
            auto* imageObject = overlapImages_[index].image.Get();
            if (!imageObject || !imageObject->IsA(SDK::UImage::StaticClass())) return false;
            auto* image = static_cast<SDK::UImage*>(imageObject);
            if (image->Brush.ResourceObject != gridSlots_[slotIndex].materialInstance.Get()) return false;
            image->SetVisibility(SDK::ESlateVisibility::HitTestInvisible);
            if (image->GetVisibility() != SDK::ESlateVisibility::HitTestInvisible) return false;
        }
        return true;
    }

    /** Stores the exact published assignment and content identity after the slot becomes visible. */
    void BuildingMapPresentation::StoreGridPublication(std::size_t slotIndex,
        const BuildingMapTarget::CompositeTargetInfo& targetInfo) noexcept
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
        if (!firstGridCompositeLogged_)
        {
            LogGridSlotEvent(slotIndex, assignment, assignment.CommittedContentGeneration,
                slot.target.Get(), "first-visible-composite");
            firstGridCompositeLogged_ = true;
        }
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
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread())
        {
            LogRetirementFailure("native-admission-or-game-thread");
            return false;
        }
        if (!RetireGridSlots())
        {
            return false;
        }
        if (!RetireLegacySingleSection(widgetDestructionStarted)) return false;
        ResetPresentationSession();
#if defined(_DEBUG)
        BuildingMapGridPlanner::SlotAssignment assignment{};
        LogGridSlotEvent(gridSlots_.size(), assignment, 0, nullptr, "cleanup-complete");
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
        lastGridCanvasIdentity_ = 0;
        lastGridTreeIdentity_ = 0;
    }
}
