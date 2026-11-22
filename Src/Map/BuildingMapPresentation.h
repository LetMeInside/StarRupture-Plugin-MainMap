// Owns six terrain-canvas images that publish BuildingMapTarget's assigned composite targets.
// Each slot retains its exact widget, material, and target identities until admitted cleanup.
#pragma once

#include "BuildingLayer.h"
#include "BuildingMapTarget.h"
#include "Native/NativeApi.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace SDK
{
    class UCanvasPanel;
    class UImage;
    class UMaterialInstanceDynamic;
    class UTextureRenderTarget2D;
    class UWidgetTree;
    struct FGeometry;
    struct FVector2D;
}

namespace MainMapBuildingMap
{
    /** Publishes assigned composite targets on the existing terrain canvas without owning their composition. */
    class BuildingMapPresentation final
    {
    public:
        /** Withdraws invalid, dirty, or retired slots before target preparation can overwrite pixels. */
        static bool PrepareGridUpdateShared(std::uint64_t sessionEpoch,
            const std::array<BuildingMapGridPlanner::SlotAssignment,
                BuildingMapGridPlanner::kPhysicalSlotCount>& assignments,
            bool sourceSnapshotCurrent) noexcept;
        /** Publishes only current clean composite targets into the six physical terrain-canvas slots. */
        static bool UpdateGridShared(const MainMapBuildingLayer::ConsumerContext& context,
            const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree,
            const std::array<BuildingMapTarget::CompositeTargetInfo,
                BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
            bool sourceSnapshotCurrent, bool materialResourcesReady);
        /** Detaches the image and releases its retained objects in dependency order. */
        static bool RetireShared(bool widgetDestructionStarted = false) noexcept;

    private:
        /** Holds one slot's retained UI resources and separate target/publication identities. */
        struct GridPresentationSlot;
        /** Restricts ownership to the one process-lifetime instance used by BuildingLayer. */
        BuildingMapPresentation() = default;
        /** Prevents copying the UObject lease owner or duplicating its widget. */
        BuildingMapPresentation(const BuildingMapPresentation&) = delete;
        /** Prevents copying the UObject lease owner or duplicating its widget. */
        BuildingMapPresentation& operator=(const BuildingMapPresentation&) = delete;
        /** Returns the persistent owner so rejected releases remain recorded for later cleanup. */
        static BuildingMapPresentation& SharedOwner() noexcept;
        /** Runs one presentation update for the supplied session and completed target. */
        bool Update(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree, const SectionTargetInfo& targetInfo,
            bool targetReady, bool materialResourcesReady);
        /** Prepares the existing slot widgets for the given plan before composition mutates targets. */
        bool PrepareGridUpdate(std::uint64_t sessionEpoch,
            const std::array<BuildingMapGridPlanner::SlotAssignment,
                BuildingMapGridPlanner::kPhysicalSlotCount>& assignments,
            bool sourceSnapshotCurrent) noexcept;
        /** Publishes each committed assignment after source, target, session, and geometry validation. */
        bool UpdateGrid(const MainMapBuildingLayer::ConsumerContext& context,
            const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree,
            const std::array<BuildingMapTarget::CompositeTargetInfo,
                BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
            bool sourceSnapshotCurrent, bool materialResourcesReady);
        /** Updates one slot and suppresses its publication whenever eligibility or geometry fails. */
        bool UpdateGridSlot(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            bool sourceSnapshotCurrent, bool geometryChanged);
        /** Ensures one widget attachment and maps the assigned world bounds into Canvas geometry. */
        bool PrepareGridSlotGeometry(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            SDK::FVector2D& position, SDK::FVector2D& size);
        /** Revalidates a stationary slot binding/visibility without recomputing its Canvas geometry. */
        bool MaintainStationaryGridSlot(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo);
        /** Applies changed geometry to an already published assignment or withdraws on failure. */
        bool UpdatePublishedGridGeometry(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            const SDK::FVector2D& position, const SDK::FVector2D& size);
        /** Detects pan, zoom, origin, canvas, tree, or session changes that require geometry mapping. */
        bool HasGridGeometryInputChanged(const MainMapBuildingLayer::ConsumerContext& context,
            const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree) const noexcept;
        /** Stores the latest view inputs; unpublished slots still retry mapping on later ticks. */
        void StoreGridGeometryInput(const MainMapBuildingLayer::ConsumerContext& context,
            const MainMapBuildingLayer::WorldRect& visibleWorld, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree) noexcept;
        /** Ensures the shared packaged presentation parent is retained without creating a legacy MID. */
        bool EnsureGridParentMaterial(SDK::UObject* worldContext, bool materialResourcesReady);
        /** Creates or validates one slot's MID, UImage, brush, and CanvasPanel attachment. */
        bool EnsureGridSlotWidgets(std::size_t slotIndex, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree, SDK::UObject* worldContext,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo);
        /** Confirms that the slot image still belongs to its expected tree, canvas, and MID. */
        bool ValidateGridAttachment(std::size_t slotIndex, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree) const noexcept;
        /** Maps exact signed composite world bounds through terrain and canvas Slate geometry. */
        bool MapGridBounds(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, const BuildingMapGridPlanner::WorldBounds& bounds,
            SDK::FVector2D& position, SDK::FVector2D& size) const;
        /** Applies top-left placement and the established terrain overlay Z order to one image. */
        bool ApplyGridGeometry(std::size_t slotIndex, const SDK::FVector2D& position,
            const SDK::FVector2D& size);
        /** Binds and reads back a composite target on one slot's dedicated presentation MID. */
        bool BindGridTarget(std::size_t slotIndex, SDK::UTextureRenderTarget2D* target);
        /** Confirms one slot's MID currently references the specified composite target. */
        bool IsGridTargetBound(std::size_t slotIndex,
            SDK::UTextureRenderTarget2D* target) const noexcept;
        /** Hides one slot and detaches its MID brush before target mutation or admitted retirement. */
        bool WithdrawGridSlot(std::size_t slotIndex) noexcept;
        /** Collapses the slot image and confirms the visibility change before target mutation. */
        bool HideGridSlotImage(std::size_t slotIndex) noexcept;
        /** Clears the image brush and releases its MID before any retained target can be modified. */
        bool DetachGridSlotMaterial(std::size_t slotIndex) noexcept;
        /** Clears the direct UImage brush reference and confirms no material remains attached. */
        bool ClearGridSlotBrush(std::size_t slotIndex, SDK::UImage* image) noexcept;
        /** Releases the detached MID lease and retains it for retry if native release is rejected. */
        bool ReleaseGridSlotMaterial(std::size_t slotIndex) noexcept;
        /** Publishes one checked assignment and retains new target ownership after a verified bind. */
        bool PublishGridSlot(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            const SDK::FVector2D& position, const SDK::FVector2D& size);
        /** Makes one validated attached slot visible without intercepting native map input. */
        bool ShowGridImage(std::size_t slotIndex) noexcept;
        /** Commits CPU publication identity after verified target binding, geometry, and visibility. */
        void StoreGridPublication(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo
#if defined(_DEBUG)
            , bool recycledPhysicalTarget
#endif
            ) noexcept;
        /** Releases unbound current, candidate, and retired target leases while preserving failures. */
        bool ReleaseGridSlotTargets(std::size_t slotIndex) noexcept;
        /** Retries an old target lease after its slot MID has stopped referencing that target. */
        bool ReleaseRetiredGridTarget(std::size_t slotIndex) noexcept;
        /** Detaches and retires one slot's widget, MID, and target leases in dependency order. */
        bool RetireGridSlot(std::size_t slotIndex, bool widgetDestructionStarted) noexcept;
        /** Removes the image from its canvas and clears its brush before MID release. */
        bool DetachGridSlotWidget(std::size_t slotIndex) noexcept;
        /** Retries retirement of all six slots before releasing their shared parent material. */
        bool RetireGridSlots(bool widgetDestructionStarted) noexcept;
        /** Returns whether a slot publication exactly matches assignment, content, and target identity. */
        bool MatchesGridPublication(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo) const noexcept;
        /** Matches physical-target assignment identity until a withdrawn, committed reassignment. */
        bool MatchesGridTargetIdentity(const GridPresentationSlot& slot,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            bool candidateTarget) const noexcept;
        /** Associates an acquired target lease with the exact assignment that created it. */
        void StoreGridTargetIdentity(GridPresentationSlot& slot,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            bool candidateTarget) noexcept;
        /** Checks target dimensions, format, gamma, mip policy, assignment and commit eligibility. */
        bool IsGridTargetEligible(const MainMapBuildingLayer::ConsumerContext& context,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            bool sourceSnapshotCurrent) const noexcept;
#if defined(_DEBUG)
        /** Emits an identity record for a slot transition or first failure. */
        void LogGridSlotEvent(std::size_t slotIndex,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            std::uint64_t contentGeneration, SDK::UObject* target,
            const char* event) noexcept;
        /** Suppresses a repeated failure for the same slot assignment and category. */
        bool ShouldLogGridFailure(std::size_t slotIndex,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            std::uint8_t failureKind) noexcept;
        /** Emits one assignment-scoped failure record after duplicate suppression. */
        void ReportGridFailure(std::size_t slotIndex,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            std::uint8_t failureKind, const char* event) noexcept;
        /** Logs a slot transition or failure using its retained assignment identity. */
        void ReportGridSlotEvent(std::size_t slotIndex, std::uint8_t failureKind,
            SDK::UObject* target, const char* event) noexcept;
#endif
        /** Loads and retains the mounted UI parent material once per visible session. */
        bool EnsureMaterial(SDK::UObject* worldContext, bool materialResourcesReady);
        /** Creates, retains, and attaches the single UImage to the active terrain canvas. */
        bool EnsureImage(SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree);
        /** Validates that the retained image remains attached through its expected CanvasPanelSlot. */
        bool ValidateAttachment(SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree) const noexcept;
        /** Maps Section bounds through terrain and canvas Slate geometry into checked float coordinates. */
        bool MapSectionGeometry(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, const SectionTargetInfo& targetInfo,
            SDK::FVector2D& position, SDK::FVector2D& size) const;
        /** Applies explicit top-left CanvasPanelSlot geometry and verifies its observable values. */
        bool ApplyGeometry(const SDK::FVector2D& position, const SDK::FVector2D& size);
        /** Reuses an unchanged target or commits a new target after binding and geometry validation. */
        bool SynchronizeTarget(const SectionTargetInfo& targetInfo,
            const SDK::FVector2D& position, const SDK::FVector2D& size);
        /** Restores the last valid Section rectangle after a candidate target update fails. */
        bool RestoreDisplayedGeometry();
        /** Converts terrain and canvas geometry corners into checked slot coordinates. */
        bool MapAbsoluteBoundsToCanvas(const SDK::FGeometry& canvasGeometry,
            const SDK::FGeometry& terrainGeometry, double localX, double localY,
            double localWidth, double localHeight,
            SDK::FVector2D& position, SDK::FVector2D& size) const;
        /** Binds a retained target to the MID and confirms the parameter readback identity. */
        bool BindTarget(SDK::UTextureRenderTarget2D* target);
        /** Checks whether the MID currently reads back the exact selected target identity. */
        bool IsTargetBound(SDK::UTextureRenderTarget2D* target) const noexcept;
        /** Restores the last valid target binding after a candidate update fails. */
        bool RestoreCurrentTarget() noexcept;
        /** Hides the image and removes its material's target reference before target release. */
        bool WithdrawTarget() noexcept;
        /** Retries release of the previously bound target after a verified replacement. */
        bool RetryRetiredTarget() noexcept;
        /** Hides the image while preserving all resources needed for a later retry. */
        void HideImage() noexcept;
        /** Makes the validated image visible without intercepting native map input. */
        bool ShowImage() noexcept;
        /** Cleans up every retained UObject while preserving leases after rejected releases. */
        bool Retire(bool widgetDestructionStarted) noexcept;
        /** Detaches and releases the inactive legacy single-image resources after grid slots retire. */
        bool RetireLegacySingleSection(bool widgetDestructionStarted) noexcept;
        /** Resets session identity and cached geometry after every presentation lease is released. */
        void ResetPresentationSession() noexcept;
        /** Reports a retirement failure once while its exact stage stays unchanged. */
        void LogRetirementFailure(const char* stage) noexcept;
        /** Releases the MID before target leases after its image brush is detached. */
        bool ReleaseDetachedResources(bool fullRetirement) noexcept;
        /** Returns the existing dynamic presentation material, if it has been retained. */
        SDK::UMaterialInstanceDynamic* MaterialInstance() const noexcept;
        /** Returns the existing retained UImage, if one has been created. */
        SDK::UImage* Image() const noexcept;

        MainMapNative::StrongObjectLease parentMaterial_;
        MainMapNative::StrongObjectLease materialInstance_;
        MainMapNative::StrongObjectLease image_;
        MainMapNative::StrongObjectLease target_;
        MainMapNative::StrongObjectLease candidateTarget_;
        MainMapNative::StrongObjectLease retiredTarget_;
        /** One stable presentation widget/MID and three lease states for a physical grid slot. */
        struct GridPresentationSlot final
        {
            MainMapNative::StrongObjectLease materialInstance;
            MainMapNative::StrongObjectLease image;
            MainMapNative::StrongObjectLease target;
            MainMapNative::StrongObjectLease candidateTarget;
            MainMapNative::StrongObjectLease retiredTarget;
            BuildingMapGridPlanner::TileIdentity TargetTile{};
            BuildingMapGridPlanner::WorldBounds TargetBounds{};
            std::uint64_t TargetSessionEpoch = 0;
            std::uint64_t TargetAssignmentGeneration = 0;
            BuildingMapGridPlanner::TileIdentity CandidateTile{};
            BuildingMapGridPlanner::WorldBounds CandidateBounds{};
            std::uint64_t CandidateSessionEpoch = 0;
            std::uint64_t CandidateAssignmentGeneration = 0;
            BuildingMapGridPlanner::TileIdentity Tile{};
            BuildingMapGridPlanner::WorldBounds Bounds{};
            std::uint64_t SessionEpoch = 0;
            std::uint64_t AssignmentGeneration = 0;
            std::uint64_t ContentGeneration = 0;
            bool HasTargetIdentity = false;
            bool HasCandidateIdentity = false;
            bool HasPublication = false;
            bool WidgetDetached = false;
#if defined(_DEBUG)
            std::uint64_t LastFailureAssignmentGeneration = 0;
            std::uint8_t LastFailureKind = 0;
            bool HasLastFailure = false;
#endif
        };
        std::array<GridPresentationSlot, BuildingMapGridPlanner::kPhysicalSlotCount> gridSlots_{};
        MainMapBuildingLayer::WorldRect lastGridVisibleWorld_{};
        double lastGridOriginX_ = 0.0;
        double lastGridOriginY_ = 0.0;
        std::uintptr_t lastGridCanvasIdentity_ = 0;
        std::uintptr_t lastGridTreeIdentity_ = 0;
        bool hasGridGeometryInput_ = false;
#if defined(_DEBUG)
        bool firstGridCompositeLogged_ = false;
        bool gridSessionRetired_ = false;
#endif
        SectionTargetInfo presentedInfo_{};
        uint64_t epoch_ = 0;
        const char* retirementFailureStage_ = nullptr;
        bool materialLoadAttempted_ = false;
        bool materialInstanceAttempted_ = false;
        bool materialValidated_ = false;
        bool presentedTargetValid_ = false;
        bool widgetDetached_ = false;
        bool materialInstanceDetached_ = false;
        bool firstAttachmentLogged_ = false;
    };
}
