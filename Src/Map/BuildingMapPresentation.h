// Owns six physical target publications and pools cropped images inside validated native terrain segments.
// Overlap images share their target slot's presentation MID and detach before target mutation.
#pragma once

#include "BuildingLayer.h"
#include "BuildingMapTarget.h"
#include "Native/NativeApi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace SDK
{
    class UCanvasPanel;
    class UCanvasPanelSlot;
    class UImage;
    class UCrMapMenuTerrainData;
    class UCrUW_MapMenuTerrain;
    class UCrUW_MapMenuTerrainSegment;
    class UMaterialInstanceDynamic;
    class UTextureRenderTarget2D;
    class UWidgetTree;
    struct FGeometry;
    struct FVector2D;
}

namespace MainMapBuildingMap
{
    /** Presents six assigned composites through reusable segment-local images without owning their composition. */
    class BuildingMapPresentation final
    {
    public:
        /** Withdraws invalid, dirty, or retired slots before target preparation can overwrite pixels. */
        static bool PrepareGridUpdateShared(std::uint64_t sessionEpoch,
            const std::array<BuildingMapGridPlanner::SlotAssignment,
                BuildingMapGridPlanner::kPhysicalSlotCount>& assignments,
            bool sourceSnapshotCurrent) noexcept;
        /** Publishes clean composites through segment-local consumers of the six physical targets. */
        static bool UpdateGridShared(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree,
            const std::array<BuildingMapTarget::CompositeTargetInfo,
                BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
            bool sourceSnapshotCurrent, bool materialResourcesReady);
        /** Detaches all owned overlap images and releases retained objects in dependency order. */
        static bool RetireShared(bool widgetDestructionStarted = false) noexcept;

    private:
        /** Holds one slot's retained UI resources and separate target/publication identities. */
        struct GridPresentationSlot;
        /** Retains exact native segment, image, slot, and canvas identities with data-derived world bounds. */
        struct NativeSegmentAssociation;
        /** Owns one pooled crop image and cached geometry for one target/segment pair. */
        struct OverlapImage;
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
        /** Withdraws the target's complete overlap membership before composition mutates its pixels. */
        bool PrepareGridUpdate(std::uint64_t sessionEpoch,
            const std::array<BuildingMapGridPlanner::SlotAssignment,
                BuildingMapGridPlanner::kPhysicalSlotCount>& assignments,
            bool sourceSnapshotCurrent) noexcept;
        /** Publishes each committed assignment after source, target, session, and geometry validation. */
        bool UpdateGrid(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree,
            const std::array<BuildingMapTarget::CompositeTargetInfo,
                BuildingMapGridPlanner::kPhysicalSlotCount>& targets,
            bool sourceSnapshotCurrent, bool materialResourcesReady);
        /** Builds or validates native segment identities and data-derived world bounds for this session. */
        bool ReconcileNativeSegments(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* terrainCanvas, SDK::UWidgetTree* terrainTree);
        /** Checks cached segment identities, native image parents, and the current data geometry. */
        bool AreNativeSegmentAssociationsCurrent(SDK::UCrUW_MapMenuTerrain* terrain,
            SDK::UCrMapMenuTerrainData* data, SDK::UWidgetTree* tree,
            const MainMapBuildingLayer::ConsumerContext& context,
            double originX, double originY, double segmentWidth, double segmentHeight) const;
        /** Detaches old owned overlap widgets before replacing native segment associations. */
        bool RetireNativeSegmentAssociations() noexcept;
        /** Releases every retained native widget and slot lease without discarding rejected ownership. */
        bool ReleaseNativeSegmentAssociations() noexcept;
        /** Associates one segment by matching its actual native slot geometry to a terrain data record. */
        bool TryAssociateNativeSegment(SDK::UCrUW_MapMenuTerrainSegment* segment,
            SDK::UCanvasPanel* terrainCanvas, SDK::UCrMapMenuTerrainData* data,
            double originX, double originY, double segmentWidth, double segmentHeight,
            std::vector<NativeSegmentAssociation>& associations);
        /** Stores one validated data record with strong widget and native-canvas leases. */
        bool StoreNativeSegmentAssociation(SDK::UCrUW_MapMenuTerrainSegment* segment,
            std::int32_t gridX, std::int32_t gridY, double originX, double originY,
            double expectedX, double expectedY, double segmentWidth, double segmentHeight,
            std::vector<NativeSegmentAssociation>& associations);
        /** Reconciles only segment/target intersections when the native grid or assignments change. */
        bool ReconcileOverlapGeometry(const std::array<BuildingMapTarget::CompositeTargetInfo,
            BuildingMapGridPlanner::kPhysicalSlotCount>& targets);
        /** Validates segment associations and reconciles overlap membership before target publication. */
        bool ReconcilePresentationGeometry(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree,
            const std::array<BuildingMapTarget::CompositeTargetInfo,
                BuildingMapGridPlanner::kPhysicalSlotCount>& targets);
        /** Withdraws all physical target publications before the source or material becomes unavailable. */
        bool WithdrawAllGridSlots() noexcept;
        /** Ensures the shared UI parent material only when at least one target can be published. */
        bool EnsureParentForEligibleTargets(const MainMapBuildingLayer::ConsumerContext& context,
            const std::array<BuildingMapTarget::CompositeTargetInfo,
                BuildingMapGridPlanner::kPhysicalSlotCount>& targets, bool sourceSnapshotCurrent,
            bool materialResourcesReady);
        /** Adds every nonempty segment intersection for one target to its reusable pool membership. */
        bool AddTargetOverlapMembership(std::size_t slotIndex,
            const BuildingMapGridPlanner::SlotAssignment& assignment, std::size_t& poolIndex);
        /** Calculates and caches anchors and UV crop values for one target/segment rectangle. */
        bool CacheOverlapGeometry(OverlapImage& overlap,
            const NativeSegmentAssociation& segment,
            const BuildingMapGridPlanner::WorldBounds& targetBounds);
        /** Sets a full material brush with the cached valid UV region and verifies Slate readback. */
        bool SetOverlapBrush(OverlapImage& overlap, SDK::UImage* image, SDK::UObject* material);
        /** Applies and verifies stretch anchors and zero-offset layout for one overlap. */
        bool ApplyOverlapLayout(OverlapImage& overlap, SDK::UCanvasPanelSlot* slot,
            const char*& failureOperation);
        /** Binds all overlap images to their target's shared MID before publication becomes visible. */
        bool BindGridOverlapConsumers(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo);
        /** Emits changed-state counts for currently visible overlap consumers. */
        void ReportActiveOverlapCount() noexcept;
        /** Emits the current attached consumer count for one physical target slot. */
        void ReportTargetConsumerCount(std::size_t slotIndex) noexcept;
        /** Binds a reused image to one segment canvas with checked stretch anchors and crop UVs. */
        bool ApplyOverlap(OverlapImage& overlap, GridPresentationSlot& targetSlot,
            const NativeSegmentAssociation& segment,
            const BuildingMapGridPlanner::WorldBounds& targetBounds,
            const char*& failureOperation);
        /** Creates or reuses a pooled image record and retains its native segment canvas. */
        bool EnsureOverlapImage(OverlapImage& overlap, SDK::UWidgetTree* tree,
            const NativeSegmentAssociation& segment, const char*& failureOperation);
        /** Creates and leases one pooled MainMap-owned UImage under its original widget tree. */
        bool CreateOverlapImage(OverlapImage& overlap, SDK::UWidgetTree* tree,
            const char*& failureOperation);
        /** Moves a brush-detached pooled image and updates its canvas lease. */
        bool AttachOverlapImage(OverlapImage& overlap, SDK::UImage* image,
            SDK::UCanvasPanel* canvas, const char*& failureOperation);
        /** Removes the image from its previous panel and appends it to the destination canvas. */
        bool MoveOverlapImageToCanvas(OverlapImage& overlap, SDK::UImage* image,
            SDK::UCanvasPanel* canvas, const char*& failureOperation);
        /** Hides and clears every overlap consumer for a physical target before MID release. */
        bool WithdrawOverlapImages(std::size_t slotIndex) noexcept;
        /** Removes all pooled owned images before native terrain teardown and drops associations. */
        bool RetireOverlapImages() noexcept;
        /** Updates one physical target slot and withdraws it when current publication checks fail. */
        bool UpdateGridSlot(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo,
            bool sourceSnapshotCurrent);
        /** Revalidates a stationary slot binding/visibility without recomputing its Canvas geometry. */
        bool MaintainStationaryGridSlot(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo);
        /** Stores native terrain identities used to detect canvas reconstruction. */
        void StoreGridAttachmentIdentity(SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree) noexcept;
        /** Ensures the shared packaged presentation parent is retained without creating a legacy MID. */
        bool EnsureGridParentMaterial(SDK::UObject* worldContext, bool materialResourcesReady);
        /** Creates or validates the one shared presentation MID retained by a physical target slot. */
        bool EnsureGridSlotWidgets(std::size_t slotIndex, SDK::UCanvasPanel* canvas,
            SDK::UWidgetTree* tree, SDK::UObject* worldContext,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo);
        /** Binds and reads back a composite target on one slot's dedicated presentation MID. */
        bool BindGridTarget(std::size_t slotIndex, SDK::UTextureRenderTarget2D* target);
        /** Confirms one slot's MID currently references the specified composite target. */
        bool IsGridTargetBound(std::size_t slotIndex,
            SDK::UTextureRenderTarget2D* target) const noexcept;
        /** Hides one slot and detaches its MID brush before target mutation or admitted retirement. */
        bool WithdrawGridSlot(std::size_t slotIndex) noexcept;
        /** Hides and verifies all overlap images before target mutation. */
        bool HideGridSlotImage(std::size_t slotIndex) noexcept;
        /** Clears the image brush and releases its MID before any retained target can be modified. */
        bool DetachGridSlotMaterial(std::size_t slotIndex) noexcept;
        /** Clears the direct UImage brush reference and confirms no material remains attached. */
        bool ClearGridSlotBrush(std::size_t slotIndex) noexcept;
        /** Releases the detached MID lease and retains it for retry if native release is rejected. */
        bool ReleaseGridSlotMaterial(std::size_t slotIndex) noexcept;
        /** Publishes one checked assignment and retains new target ownership after a verified bind. */
        bool PublishGridSlot(std::size_t slotIndex,
            const BuildingMapTarget::CompositeTargetInfo& targetInfo);
        /** Makes all bound overlap images for one target visible without intercepting native map input. */
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
        /** Retires one physical target's MID and target leases in dependency order. */
        bool RetireGridSlot(std::size_t slotIndex) noexcept;
        /** Retries retirement of all six slots before releasing their shared parent material. */
        bool RetireGridSlots() noexcept;
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
        /** One physical target publication owner with one shared presentation MID. */
        struct GridPresentationSlot final
        {
            MainMapNative::StrongObjectLease materialInstance;
            std::vector<std::size_t> overlapMembers;
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
#if defined(_DEBUG)
            std::uint64_t LastFailureAssignmentGeneration = 0;
            std::uint8_t LastFailureKind = 0;
            bool HasLastFailure = false;
#endif
        };
        /** One actual native segment identity and its data-derived world bounds. */
        struct NativeSegmentAssociation final
        {
            MainMapNative::StrongObjectLease segment;
            MainMapNative::StrongObjectLease segmentTree;
            MainMapNative::StrongObjectLease terrainImage;
            MainMapNative::StrongObjectLease fogImage;
            MainMapNative::StrongObjectLease segmentSlot;
            MainMapNative::StrongObjectLease terrainImageSlot;
            MainMapNative::StrongObjectLease fogImageSlot;
            MainMapNative::StrongObjectLease canvas;
            MainMapBuildingLayer::WorldRect bounds{};
            std::int32_t gridX = 0;
            std::int32_t gridY = 0;
        };
        /** One pool entry reused for a target/segment intersection across content updates. */
        struct OverlapImage final
        {
            MainMapNative::StrongObjectLease image;
            MainMapNative::StrongObjectLease outerTree;
            MainMapNative::StrongObjectLease canvas;
            std::size_t targetSlot = static_cast<std::size_t>(-1);
            std::size_t segmentIndex = static_cast<std::size_t>(-1);
            float anchorMinimumX = 0.0f;
            float anchorMinimumY = 0.0f;
            float anchorMaximumX = 0.0f;
            float anchorMaximumY = 0.0f;
            float uvMinimumX = 0.0f;
            float uvMinimumY = 0.0f;
            float uvMaximumX = 0.0f;
            float uvMaximumY = 0.0f;
            bool layoutApplied = false;
            bool geometryReady = false;
            bool active = false;
            std::uint64_t lastFailureSessionEpoch = 0;
            std::uint64_t lastFailureAssignmentGeneration = 0;
            std::int32_t lastFailureSegmentX = 0;
            std::int32_t lastFailureSegmentY = 0;
            const char* lastFailureOperation = nullptr;
        };
        std::array<GridPresentationSlot, BuildingMapGridPlanner::kPhysicalSlotCount> gridSlots_{};
        std::vector<NativeSegmentAssociation> nativeSegments_;
        std::vector<OverlapImage> overlapImages_;
        std::array<BuildingMapGridPlanner::WorldBounds,
            BuildingMapGridPlanner::kPhysicalSlotCount> overlapTargetBounds_{};
        std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount> overlapTargetAssigned_{};
        std::array<std::size_t, BuildingMapGridPlanner::kPhysicalSlotCount> loggedConsumerCounts_{};
        std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount> hasLoggedConsumerCount_{};
        std::size_t loggedActiveOverlapCount_ = 0;
        bool hasLoggedActiveOverlapCount_ = false;
        bool nativeAssociationFailureLogged_ = false;
        std::int32_t rejectedSegmentIndex_ = -1;
        bool overlapGeometryReady_ = false;
        double nativeSegmentWidth_ = 0.0;
        double nativeSegmentHeight_ = 0.0;
        std::uintptr_t lastGridCanvasIdentity_ = 0;
        std::uintptr_t lastGridTreeIdentity_ = 0;
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
