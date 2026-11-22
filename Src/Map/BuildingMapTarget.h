// Owns coordinate-keyed source textures, six session-bound physical composite targets, and inactive Section proof resources.
#pragma once

#include "BuildingLayer.h"
#include "BuildingMapRemote.h"
#include "BuildingMapGridPlanner.h"
#include "Native/NativeApi.h"
#include "Engine_classes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace MainMapBuildingMap
{
    /** Read-only description of the one successfully composed Section target. */
    struct SectionTargetInfo final
    {
        SDK::UTextureRenderTarget2D* Target = nullptr;
        int64_t GridX = 0;
        int64_t GridY = 0;
        double XMapUnits = 0.0;
        double YMapUnits = 0.0;
        double SectionSizeMapUnits = 0.0;
        int32_t Width = 0;
        int32_t Height = 0;
        uint64_t Revision = 0;
    };

    /** Owns MainMap's bounded composite/source GPU resources and inactive legacy Section proof state. */
    class BuildingMapTarget final
    {
    public:
        /** State distinguishes absent imagery from unavailable resources and composition failure. */
        enum class Status
        {
            NoImage,
            ResourcesUnavailable,
            Ready,
            Failure
        };

        /** Accepted current metadata for one demanded Section assigned to a composite slot. */
        struct CompositeSectionSource final
        {
            int64_t GridX = 0;
            int64_t GridY = 0;
            int64_t GroupingN = 0;
            std::uint64_t SessionEpoch = 0;
            uint32_t PhysicalSlotIndex = 0;
            bool HasImage = false;
            double XMapUnits = 0.0;
            double YMapUnits = 0.0;
            double SectionSizeMapUnits = 0.0;
            uint32_t Width = 0;
            uint32_t Height = 0;
            uint64_t RowStride = 0;
            uint64_t ByteCount = 0;
            uint64_t Revision = 0;
        };

        /** Current target-resource identity paired with one planner assignment for publication checks. */
        struct CompositeTargetInfo final
        {
            BuildingMapGridPlanner::SlotAssignment Assignment{};
            SDK::UTextureRenderTarget2D* Target = nullptr;
            bool TargetMatchesAssignment = false;
#if defined(_DEBUG)
            bool TargetWasReusedForAssignment = false;
#endif
        };

        /** Retires resources owned by the shared instance through admitted lease releases. */
        static bool RetireShared() noexcept;
        /** Exposes read-only target status and bounds to the next presentation stage. */
        static const BuildingMapTarget& GetShared() noexcept;
        /** Plans changed visible geometry into the existing target owner's CPU-only slot model. */
        static bool UpdateGridPlanShared(const MainMapBuildingLayer::WorldRect& visibleWorld,
            std::uint64_t sessionEpoch) noexcept;
        /** Reconciles current remote availability into assigned composites after a complete poll. */
        static void ReconcileGridSourcesShared(const BuildingMapRemote& remote,
            std::uint64_t sessionEpoch) noexcept;
        /** Marks the latest planning input unavailable until another valid rectangle is observed. */
        static bool InvalidateGridPlanInputShared() noexcept;
        /** Reports whether the most recently observed geometry produced a valid plan. */
        static bool HasCurrentGridPlanShared() noexcept;
        /** Exposes the target owner's current CPU plan and slot state without engine ownership. */
        static const BuildingMapGridPlanner& GetGridPlannerShared() noexcept;
        /** Reports whether accepted Section state corresponds to the latest complete poll. */
        static bool HasCurrentGridSourcesShared() noexcept;
        /** Exposes metadata-only sources for current assigned tiles in signed coordinate order. */
        static const std::vector<CompositeSectionSource>& GetGridSourcesShared() noexcept;
        /** Copies six current assignment/target identities without transferring resource ownership. */
        static std::array<CompositeTargetInfo, BuildingMapGridPlanner::kPhysicalSlotCount>
            GetCompositeTargetInfosShared() noexcept;
        /** Prepares current assigned composites after a complete poll without changing presentation. */
        static void PrepareGridCompositesShared(SDK::UObject* worldContext,
            const BuildingMapRemote& remote, std::uint64_t sessionEpoch,
            bool materialResourcesReady) noexcept;

        /** Reports whether the last selected Section has a completed retained target. */
        bool IsReady() const noexcept;
        /** Returns whether the current composition is ready or why no new target is available. */
        Status GetStatus() const noexcept;
        /** Returns the current target identity and its published Section bounds. */
        SectionTargetInfo GetTargetInfo() const noexcept;
        /** Retains or selects one visible Section and updates resources only when its revision changes. */
        void Update(SDK::UObject* worldContext, const BuildingMapRemote::ImageCollection& images,
            const MainMapBuildingLayer::WorldRect& visibleWorld, bool materialResourcesReady);
        /** Retires all transient resources through admitted StrongObjectLease releases. */
        bool Retire() noexcept;

    private:
        /** Returns the one persistent owner used by lifecycle and presentation integration. */
        static BuildingMapTarget& SharedOwner() noexcept;
        /** Groups imported input, composition material, and output target leases for construction. */
        struct ResourceSet final
        {
            MainMapNative::StrongObjectLease InputTexture;
            MainMapNative::StrongObjectLease MaterialInstance;
            MainMapNative::StrongObjectLease RenderTarget;
        };

        /** Owns one stable texture/material pair and any replacement for one signed Section coordinate. */
        struct SourceTextureEntry final
        {
            MainMapNative::StrongObjectLease Texture;
            MainMapNative::StrongObjectLease MaterialInstance;
            MainMapNative::StrongObjectLease PendingTexture;
            MainMapNative::StrongObjectLease PendingMaterialInstance;
            CompositeSectionSource Accepted{};
            CompositeSectionSource Pending{};
            CompositeSectionSource FailedAttempt{};
            std::uint64_t FailedObservationRevision = 0;
            bool HasAcceptedTexture = false;
            bool HasPendingTexture = false;
            bool PendingTextureValidated = false;
            bool HasFailedAttempt = false;
#if defined(_DEBUG)
            bool LeaseReleaseDeferred = false;
#endif
        };

        /** Owns one slot's reusable physical target separately from its current logical assignment. */
        struct CompositeSlotResources final
        {
            ResourceSet Resources;
            std::uint64_t ResourceSessionEpoch = 0;
            BuildingMapGridPlanner::TileIdentity Tile{};
            BuildingMapGridPlanner::WorldBounds Bounds{};
            std::uint64_t SessionEpoch = 0;
            std::uint64_t AssignmentGeneration = 0;
            bool HasAssignment = false;
            bool TargetResourceInvalid = false;
#if defined(_DEBUG)
            bool LeaseReleaseDeferred = false;
            std::uint64_t LastFailureAssignmentGeneration = 0;
            std::uint8_t LastFailureKind = 0;
            bool HasLastFailure = false;
            BuildingMapGridPlanner::TileIdentity ReuseFromTile{};
            std::uint64_t ReuseFromAssignmentGeneration = 0;
            std::uint64_t ReuseFromSessionEpoch = 0;
            bool HasPendingReuseLog = false;
            std::uint64_t LastReusedAssignmentGeneration = 0;
#endif
        };

#if defined(_DEBUG)
        /** Accumulates one changed preparation pass for bounded Client Debug reporting. */
        struct GridDiagnosticBatch final
        {
            std::uint32_t ImportedSources = 0;
            std::uint32_t ReusedSources = 0;
            std::uint32_t ReplacedSources = 0;
            std::uint32_t RetiredSources = 0;
            std::uint32_t CreatedTargets = 0;
            std::uint32_t ReusedTargets = 0;
            std::uint32_t RetiredTargets = 0;
            std::uint32_t ComposedSlots = 0;
            std::uint32_t DrawSubmissions = 0;
            std::uint32_t CpuCommits = 0;
            std::uint32_t SnapshotFailures = 0;
            std::uint32_t ImportFailures = 0;
            std::uint32_t ResourceFailures = 0;
            std::uint32_t CanvasFailures = 0;
            std::uint32_t CompositionFailures = 0;
            std::uint32_t ReleaseDeferrals = 0;
            std::uint32_t SuccessfulReleaseRetries = 0;
            std::int64_t FirstFailureGridX = 0;
            std::int64_t FirstFailureGridY = 0;
            std::int64_t FirstReleaseGridX = 0;
            std::int64_t FirstReleaseGridY = 0;
            bool HasSourceFailure = false;
            bool HasReleaseSource = false;
        };
#endif

        /** Normalized floating destination rectangle in one composite render target. */
        struct CompositeDestinationRectangle final
        {
            double Left = 0.0;
            double Top = 0.0;
            double Right = 0.0;
            double Bottom = 0.0;
        };

        /** Validates source metadata and encodes its RGBA8 pixels into one in-memory PNG. */
        static bool EncodePng(const SectionImage& image, std::vector<uint8_t>& pngBytes);
        /** Appends an uncompressed DEFLATE stream containing the PNG scanlines. */
        static bool EncodeDeflate(const SectionImage& image, std::vector<uint8_t>& deflateBytes);
        /** Appends a PNG chunk with a computed CRC over its type and payload. */
        static void AppendPngChunk(const char type[4], const uint8_t* payload, size_t payloadSize,
            std::vector<uint8_t>& pngBytes);
        /** Updates the PNG CRC-32 state for one byte. */
        static uint32_t UpdateCrc32(uint32_t crc, uint8_t value) noexcept;
        /** Writes one network-order integer into a PNG output vector. */
        static void AppendBigEndian32(uint32_t value, std::vector<uint8_t>& bytes);
        /** Appends the Adler-32 trailer required by a zlib-wrapped DEFLATE stream. */
        static void AppendAdler32(const std::vector<uint8_t>& bytes,
            std::vector<uint8_t>& deflateBytes);
        /** Imports and validates one newly selected Section input texture. */
        bool CreateInputTexture(SDK::UObject* worldContext, const SectionImage& image,
            ResourceSet& resources);
        /** Creates and validates an RGBA16f render target at the image's exact dimensions. */
        bool CreateRenderTarget(SDK::UObject* worldContext, const SectionImage& image,
            ResourceSet& resources);
        /** Creates the mounted composition material and optionally binds its first SpriteAtlas input. */
        bool CreateCompositionMaterial(SDK::UObject* worldContext, SDK::UTexture2D* inputTexture,
            ResourceSet& resources, bool updateTargetStatus = true,
            bool* leaseReleaseDeferred = nullptr);
        /** Ensures the retained composition parent is available before beginning a source import. */
        bool EnsureCompositionMaterial(bool updateTargetStatus,
            bool* leaseReleaseDeferred = nullptr);
        /** Creates and validates the fixed 1024-square RGBA16f target for one tile. */
        bool CreateCompositeRenderTarget(SDK::UObject* worldContext, ResourceSet& resources);
        /** Binds one source texture to SpriteAtlas and verifies the material parameter readback. */
        bool BindCompositionSource(SDK::UMaterialInstanceDynamic* material,
            SDK::UTexture2D* inputTexture) const;
        /** Draws the selected image as two white-tinted triangles into a cleared target. */
        bool ComposeSection(SDK::UObject* worldContext, int32_t width, int32_t height,
            ResourceSet& resources);
        /** Synchronizes source imports and composes only dirty assigned physical slots. */
        void PrepareGridComposites(SDK::UObject* worldContext, const BuildingMapRemote& remote,
            std::uint64_t sessionEpoch, bool materialResourcesReady) noexcept;
#if defined(_DEBUG)
        /** Emits changed preparation state and counters after duplicate suppression. */
        void EmitGridDiagnosticSummary(const BuildingMapRemote* remote,
            std::uint64_t sessionEpoch, const char* event, bool force) noexcept;
        /** Writes one aggregate record after its counts are collected. */
        void LogGridDiagnosticSummary(const BuildingMapRemote* remote,
            std::uint64_t sessionEpoch, const char* event, std::uint32_t assignedSlots,
            std::uint32_t dirtySlots, std::uint32_t availableSources,
            std::uint32_t noImageSources, std::uint32_t retainedTargets,
            std::uint32_t retainedSourceTextures, std::uint32_t retainedSourceMaterials) noexcept;
        /** Reports whether this pass contains changed state or a new operation/failure. */
        bool HasGridDiagnosticActivity() const noexcept;
        /** Compares and stores current bounded grid/poll state for duplicate-log suppression. */
        bool UpdateGridDiagnosticState(const BuildingMapRemote* remote,
            std::uint64_t sessionEpoch, std::uint32_t& assignedSlots,
            std::uint32_t& dirtySlots) noexcept;
        /** Emits one attempted composite result with its stable slot/world assignment identity. */
        void LogGridCompositeResult(const BuildingMapGridPlanner::SlotAssignment& assignment,
            std::uint32_t drawSubmissions, bool submitted, bool committed,
            const char* result) noexcept;
        /** Logs physical target allocation, retirement, or committed cross-assignment reuse. */
        void LogGridTargetTransition(std::size_t slotIndex, const char* event,
            SDK::UObject* target, const BuildingMapGridPlanner::TileIdentity& oldTile,
            std::uint64_t oldSessionEpoch, std::uint64_t oldAssignmentGeneration,
            const BuildingMapGridPlanner::SlotAssignment& newAssignment,
            std::uint64_t committedContentGeneration) noexcept;
        /** Logs one source import or lease-release event with its owning composite identity. */
        void LogGridSourceEvent(const CompositeSectionSource& source, const char* event) noexcept;
        /** Suppresses repeated failure records for the same slot assignment and failure class. */
        bool ShouldLogGridCompositeFailure(CompositeSlotResources& resources,
            const BuildingMapGridPlanner::SlotAssignment& assignment, std::uint8_t failureKind) noexcept;
        /** Retains the first coordinate whose admitted source-lease release was deferred. */
        void RecordGridSourceReleaseFailure(std::int64_t gridX, std::int64_t gridY) noexcept;
#endif
        /** Groups accepted available sources once by their fixed physical slot index. */
        bool GroupCompositeSources(
            std::array<std::vector<const CompositeSectionSource*>,
                BuildingMapGridPlanner::kPhysicalSlotCount>& sourcesBySlot) const noexcept;
        /** Synchronizes all available source textures and records per-slot readiness. */
        void SynchronizeGridSourceSet(SDK::UObject* worldContext, const BuildingMapRemote& remote,
            std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount>& slotSourcesReady) noexcept;
        /** Allocates/recomposes dirty assignments whose current source imports are ready. */
        void ComposeDirtyGridSlots(SDK::UObject* worldContext, const BuildingMapRemote& remote,
            const std::array<std::vector<const CompositeSectionSource*>,
                BuildingMapGridPlanner::kPhysicalSlotCount>& sourcesBySlot,
            const std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount>& slotSourcesReady);
        /** Prepares one dirty slot or retires its resource after logical assignment departure. */
        void PrepareDirtyGridSlot(SDK::UObject* worldContext, const BuildingMapRemote& remote,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            const std::vector<const CompositeSectionSource*>& sources,
            bool slotSourcesReady);
#if defined(_DEBUG)
        /** Records a submitted Canvas attempt and its CPU commit result for bounded diagnostics. */
        void RecordCompositeSubmission(CompositeSlotResources& resources,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            bool submitted, bool committed);
#endif
        /** Clears the logical assignment for a vacant physical slot while retaining its target. */
        void RetireUnassignedGridSlot(CompositeSlotResources& resources);
        /** Reuses or imports one coordinate's accepted Section image into the GPU source cache. */
        bool SynchronizeSourceTexture(SDK::UObject* worldContext,
            const CompositeSectionSource& source,
            const SectionImage& image) noexcept;
        /** Adopts a validated pending source after admitted release of its old pair succeeds. */
        bool PromotePendingSourceTexture(const CompositeSectionSource& source,
            SourceTextureEntry& entry) noexcept;
        /** Releases a stale or invalid pending pair and preserves retry bookkeeping on rejection. */
        bool ResetPendingSourceTexture(const CompositeSectionSource& source,
            SourceTextureEntry& entry) noexcept;
        /** Builds one imported texture and its bound MID, distinguishing parent-release deferral. */
        bool BuildSourceTextureCandidate(SDK::UObject* worldContext,
            const CompositeSectionSource& source, const SectionImage& image,
            SourceTextureEntry& entry, ResourceSet& candidate,
            bool& leaseReleaseDeferred, bool& materialFailure) noexcept;
        /** Imports a replacement texture and preserves it as pending if the old lease cannot release. */
        bool ImportReplacementSourceTexture(SDK::UObject* worldContext,
            const CompositeSectionSource& source, const SectionImage& image,
            SourceTextureEntry& entry) noexcept;
        /** Preserves a genuine unchanged import/material failure without repeating its PNG import. */
        void RecordFailedSourceTextureBuild(const CompositeSectionSource& source,
            ResourceSet& candidate, SourceTextureEntry& entry, bool materialFailure) noexcept;
        /** Adopts a fully validated source pair after retiring its prior accepted pair. */
        bool AdoptSourceTextureCandidate(const CompositeSectionSource& source,
            ResourceSet& candidate, SourceTextureEntry& entry) noexcept;
        /** Preserves an invalid imported texture if native lease release cannot complete. */
        void PreserveFailedSourceImport(const CompositeSectionSource& source,
            ResourceSet& candidate, SourceTextureEntry& entry) noexcept;
        /** Drops source textures that left the complete accepted assigned-demand snapshot. */
        void RetireDepartedSourceTextures() noexcept;
        /** Retires all composite and source GPU resources without losing failed lease records. */
        bool RetireGridResources() noexcept;
        /** Retires six composite targets and records rejected lease releases. */
        bool RetireCompositeGridTargets() noexcept;
        /** Retires retained targets from prior terrain epochs before current-session preparation. */
        bool RetireTargetsOutsideSession(std::uint64_t sessionEpoch) noexcept;
        /** Requires a complete current source snapshot and available composition materials. */
        bool ValidateGridPreparationSnapshot(const BuildingMapRemote& remote,
            std::uint64_t sessionEpoch, bool materialResourcesReady) noexcept;
        /** Retires one physical slot target and reports its deferred or completed release. */
        bool RetireCompositeTargetSlot(std::size_t slotIndex,
            CompositeSlotResources& resources) noexcept;
        /** Retires current source pairs and preserves entries whose lease releases are rejected. */
        bool RetireSourceGridTextures() noexcept;
        /** Reuses a valid same-session physical target or lazily allocates one for the slot. */
        bool EnsureCompositeSlotResources(SDK::UObject* worldContext,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            std::size_t physicalSlotIndex, CompositeSlotResources& resources);
        /** Retires an invalid or cross-session target before the slot can be reassigned. */
        bool RetireInvalidCompositeTarget(std::size_t physicalSlotIndex,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            CompositeSlotResources& resources);
        /** Updates logical assignment identity while preserving a valid same-session target. */
        bool ReassignCompositeTarget(const BuildingMapGridPlanner::SlotAssignment& assignment,
            CompositeSlotResources& resources);
        /** Lazily allocates and validates one target after any prior resource releases succeed. */
        bool AllocateCompositeTarget(SDK::UObject* worldContext,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            std::size_t physicalSlotIndex, CompositeSlotResources& resources);
        /** Compares the complete logical assignment stored beside one physical target. */
        static bool MatchesCompositeAssignment(const CompositeSlotResources& resources,
            const BuildingMapGridPlanner::SlotAssignment& assignment) noexcept;
        /** Clears and submits the accepted available subset for one dirty composite. */
        bool ComposeCompositeTile(SDK::UObject* worldContext,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            const std::vector<const CompositeSectionSource*>& sources,
            const BuildingMapRemote& remote);
        /** Clears a composite target and begins a validated Canvas context for its draw pass. */
        bool BeginCompositeCanvas(SDK::UObject* worldContext,
            SDK::UTextureRenderTarget2D* target, SDK::UCanvas*& canvas,
            SDK::FDrawToRenderTargetContext& context);
        /** Begins Canvas drawing, draws a slot's sources, and ends any successful begin. */
        bool DrawCompositeSources(const BuildingMapGridPlanner::SlotAssignment& assignment,
            const std::vector<const CompositeSectionSource*>& sources,
            const BuildingMapRemote& remote, SDK::UCanvas* canvas);
        /** Validates one source rectangle and draws it into the assigned composite target. */
        bool DrawCompositeSource(const CompositeSectionSource& source, const SectionImage& image,
            const BuildingMapGridPlanner::SlotAssignment& assignment, SDK::UCanvas* canvas);
        /** Converts actual published Section bounds to checked 1024-square target coordinates. */
        static bool TryGetCompositeDestination(const SectionImage& image,
            const BuildingMapGridPlanner::SlotAssignment& assignment,
            CompositeDestinationRectangle& output) noexcept;
        /** Compares provider metadata with the accepted CPU source record without reading pixels. */
        static bool MatchesAcceptedSource(const CompositeSectionSource& source,
            const SectionImage& image) noexcept;
        /** Compares the texture-relevant identity while ignoring grouping and slot membership. */
        static bool SameSourceTextureIdentity(const CompositeSectionSource& first,
            const CompositeSectionSource& second) noexcept;
        /** Releases a coordinate's pending then accepted material/texture leases while retaining failures. */
        static bool ResetSourceTextureEntry(SourceTextureEntry& source) noexcept;
        /** Clears only the logical assignment while retaining physical target lifetime identity. */
        static void ClearCompositeSlotAssignment(CompositeSlotResources& resources) noexcept;
        /** Retires a slot's physical target and clears all identity only after releases succeed. */
        static bool RetireCompositeSlotResources(CompositeSlotResources& resources) noexcept;
        /** Associates a physical target with its owning epoch and current logical assignment. */
        static void AssignCompositeSlot(CompositeSlotResources& destination,
            const BuildingMapGridPlanner::SlotAssignment& assignment) noexcept;
        /** Validates the fixed target allocation properties before physical reuse. */
        static bool IsCompositeTargetAllocationValid(
            const SDK::UTextureRenderTarget2D* target) noexcept;
        /** Assigns one Canvas triangle's positions, UVs, and opaque white vertex tint. */
        static void SetTriangle(SDK::FCanvasUVTri& triangle,
            const SDK::FVector2D& position0, const SDK::FVector2D& uv0,
            const SDK::FVector2D& position1, const SDK::FVector2D& uv1,
            const SDK::FVector2D& position2, const SDK::FVector2D& uv2) noexcept;
        /** Releases a resource set in dependency order while retaining failed leases. */
        static bool ResetResources(ResourceSet& resources) noexcept;
        /** Moves a fully built resource set into the active owner after old releases succeed. */
        void AdoptResources(ResourceSet& resources) noexcept;
        /** Builds, composes, and commits a changed selection without discarding the prior target. */
        void BuildSelectedImage(SDK::UObject* worldContext, const SectionImage& image,
            const char* selectionReason);
        /** Reports whether pending resources match a selected coordinate and revision. */
        bool MatchesSelectedImage(const SectionImage& image) const noexcept;
        /** Retries cleanup of stale or partially-created pending objects. */
        bool ClearStalePendingResources(const SectionImage& image) noexcept;
        /** Promotes a previously composed pending target after old leases release. */
        void PromotePendingResources(const SectionImage& image) noexcept;
        /** Reports whether current retained content already represents this revision. */
        bool IsCurrentImage(const SectionImage& image) const noexcept;
        /** Updates the CPU grid model only when visible geometry or session epoch changed. */
        bool UpdateGridPlan(const MainMapBuildingLayer::WorldRect& visibleWorld,
            std::uint64_t sessionEpoch) noexcept;
        /** Retires assignments and accepted source state when geometry/session ownership is lost. */
        bool InvalidateGridPlanInput() noexcept;
        /** Reconciles one complete remote source snapshot or invalidates current source certainty. */
        void ReconcileGridSources(const BuildingMapRemote& remote,
            std::uint64_t sessionEpoch) noexcept;
        /** Marks every assigned tile dirty when the latest source snapshot is uncertain. */
        void MarkAllAssignedGridSlotsDirty() noexcept;
        /** Tests whether changed remote, session, grouping or slot identities require a rebuild. */
        bool NeedsGridSourceRebuild(const BuildingMapRemote& remote,
            std::uint64_t sessionEpoch) const noexcept;
        /** Commits one successfully built metadata snapshot and records its input identities. */
        void CommitGridSourceSnapshot(const BuildingMapRemote& remote,
            std::uint64_t sessionEpoch, std::vector<CompositeSectionSource>& nextSources) noexcept;
        /** Builds assigned-tile metadata without copying the remote's pixel storage. */
        bool BuildGridSourceSnapshot(const BuildingMapRemote& remote,
            std::vector<CompositeSectionSource>& output) const noexcept;
        /** Marks slots affected by a difference between accepted and next source snapshots. */
        void MarkGridSourceDifferences(const std::vector<CompositeSectionSource>& nextSources) noexcept;
        /** Marks one still-assigned slot dirty after a source-state transition. */
        void MarkGridSourceSlot(uint32_t slotIndex) noexcept;
        /** Finds the assigned physical slot using signed zero-anchored floor division. */
        bool FindAssignedCompositeSlot(int64_t gridX, int64_t gridY,
            uint32_t& slotIndex) const noexcept;
        /** Detects slot assignment changes that require source coordinates to be remapped. */
        bool SameSourceAssignments() const noexcept;
        /** Compares source metadata, revision, and composite assignment identity. */
        static bool SameGridSource(const CompositeSectionSource& first,
            const CompositeSectionSource& second) noexcept;
        /** Converts a signed Section coordinate to its zero-anchored composite coordinate. */
        static int64_t FloorDivideSectionCoordinate(int64_t coordinate, int64_t groupingN) noexcept;
        /** Reports validity for the latest cached geometry/session input. */
        bool HasCurrentGridPlan() const noexcept;
        /** Invalidates logical plan/source identities without touching GPU targets before UI withdrawal. */
        void InvalidateGridPlanState() noexcept;
        /** Compares the last grid input exactly to avoid replanning unchanged ticks. */
        static bool SameGridPlanInput(const MainMapBuildingLayer::WorldRect& first,
            const MainMapBuildingLayer::WorldRect& second) noexcept;
        /** Retains an eligible Section or chooses the greatest visible intersection in map order. */
        const SectionImage* SelectVisibleImage(const BuildingMapRemote::ImageCollection& images,
            const MainMapBuildingLayer::WorldRect& visibleWorld, const char*& selectionReason) const noexcept;
        /** Validates published Section bounds and returns their positive intersection area. */
        bool GetIntersectionArea(const SectionImage& image,
            const MainMapBuildingLayer::WorldRect& visibleWorld, double& area) const noexcept;
        /** Moves every staged lease into the pending owner without losing failed releases. */
        void PreservePendingResources(ResourceSet& resources) noexcept;
        /** Stores the selected Section identity, bounds, and transition reason after composition succeeds. */
        void SetSelectedImage(const SectionImage& image, const char* selectionReason) noexcept;

        MainMapNative::StrongObjectLease material_;
        ResourceSet current_;
        ResourceSet pending_;
        SectionTargetInfo targetInfo_{};
        SectionTargetInfo pendingInfo_{};
        BuildingMapGridPlanner gridPlanner_{};
        std::vector<CompositeSectionSource> compositeSources_;
        std::map<BuildingMapRemote::SectionCoordinate, SourceTextureEntry> sourceTextures_;
        std::array<CompositeSlotResources, BuildingMapGridPlanner::kPhysicalSlotCount> compositeTargets_{};
        std::array<std::uint64_t, BuildingMapGridPlanner::kPhysicalSlotCount> sourceAssignmentGenerations_{};
        MainMapBuildingLayer::WorldRect lastGridPlanWorld_{};
        std::uint64_t lastGridPlanEpoch_ = 0;
        std::uint64_t lastSourceObservationRevision_ = 0;
        std::uint64_t sourceSnapshotEpoch_ = 0;
        int64_t sourceSnapshotGroupingN_ = 0;
#if defined(_DEBUG)
        GridDiagnosticBatch gridDiagnosticBatch_{};
        std::array<std::uint64_t, BuildingMapGridPlanner::kPhysicalSlotCount>
            lastDiagnosticAssignmentGenerations_{};
        std::array<std::uint8_t, BuildingMapGridPlanner::kPhysicalSlotCount>
            lastDiagnosticAssignmentStates_{};
        std::uint64_t lastDiagnosticEpoch_ = 0;
        std::uint64_t lastDiagnosticObservationRevision_ = 0;
        std::int64_t lastDiagnosticGroupingN_ = 0;
        std::uint32_t currentCompositeDrawSubmissions_ = 0;
        std::uint8_t currentCompositeFailureKind_ = 0;
        BuildingMapRemote::PollState lastDiagnosticPollState_ = BuildingMapRemote::PollState::Unavailable;
        bool hasLastDiagnosticState_ = false;
        bool lastDiagnosticSnapshotValid_ = false;
#endif
        uint64_t revision_ = 0;
        uint64_t pendingRevision_ = 0;
        const char* pendingSelectionReason_ = nullptr;
        bool materialLoadAttempted_ = false;
        bool materialValidated_ = false;
        bool ready_ = false;
        bool pendingReady_ = false;
        bool firstCompositionLogged_ = false;
        bool hasGridPlanInput_ = false;
        bool gridPlanInputValid_ = false;
        bool sourceSnapshotValid_ = false;
        Status status_ = Status::NoImage;
    };
}
