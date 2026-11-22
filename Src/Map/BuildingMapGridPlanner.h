// Plans the zero-anchored Section composite grid and reconciles six logical slots.
// This CPU-only model owns no engine objects, provider handles, or rendering resources.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace MainMapBuildingMap
{
    /** Computes checked composite plans and tracks physical-slot assignment/content identities. */
    class BuildingMapGridPlanner final
    {
    public:
        static constexpr std::size_t kPhysicalSlotCount = 6;
        static constexpr std::int64_t kBaseSectionSideCentimetres = 12500;

        /** Input rectangle in the current MainMap world coordinate convention, in centimetres. */
        struct WorldRectangle final
        {
            double MinX = 0.0;
            double MinY = 0.0;
            double MaxX = 0.0;
            double MaxY = 0.0;
        };

        /** Stable tile key; grouping and signed coordinates define the zero-anchored lattice identity. */
        struct TileIdentity final
        {
            std::int64_t GroupingN = 0;
            std::int64_t X = 0;
            std::int64_t Y = 0;
        };

        /** Exact integral world bounds for a tile, in centimetres with half-open maximum edges. */
        struct WorldBounds final
        {
            std::int64_t MinX = 0;
            std::int64_t MinY = 0;
            std::int64_t MaxX = 0;
            std::int64_t MaxY = 0;
        };

        /** A required tile key paired with the bounds derived from its composite side. */
        struct GridTile final
        {
            TileIdentity Identity{};
            WorldBounds Bounds{};
        };

        /** Half-open signed tile-coordinate range intersected by a valid visible rectangle. */
        struct TileCoordinateRange final
        {
            std::int64_t FirstX = 0;
            std::int64_t EndX = 0;
            std::int64_t FirstY = 0;
            std::int64_t EndY = 0;
        };

        /** Complete bounded plan; tile descriptors are generated from its canonical range and side. */
        struct GridPlan final
        {
            bool IsValid = false;
            std::int64_t GroupingN = 0;
            std::int64_t CompositeSideCentimetres = 0;
            TileCoordinateRange Range{};
        };

        /** Failure reason for invalid geometry or a plan that cannot fit the six-slot invariant. */
        enum class PlanFailure : std::uint8_t
        {
            None,
            InvalidRectangle,
            ArithmeticOutOfRange,
            TileCountOutOfRange,
            AssignmentGenerationOutOfRange
        };

        /** Bit flags describe a slot's transition during one successful reconciliation. */
        enum SlotChange : std::uint8_t
        {
            NoChange = 0,
            RetainedAssignment = 1,
            NewlyAssigned = 2,
            RetiredAssignment = 4,
            GroupingChanged = 8,
            SessionChanged = 16
        };

        /** Current logical state for one stable physical slot index. */
        struct SlotAssignment final
        {
            std::uint32_t PhysicalSlotIndex = 0;
            bool IsAssigned = false;
            TileIdentity Tile{};
            std::uint64_t SessionEpoch = 0;
            WorldBounds Bounds{};
            std::uint64_t AssignmentGeneration = 0;
            bool IsDirty = false;
            bool HasCommittedContent = false;
            std::uint64_t CommittedContentGeneration = 0;
        };

        /** Before/after state and reasons for one physical slot's latest transition. */
        struct SlotTransition final
        {
            std::uint8_t Changes = NoChange;
            SlotAssignment Before{};
            SlotAssignment After{};
        };

        using SlotTransitionArray = std::array<SlotTransition, kPhysicalSlotCount>;

        /** Initializes physical slot indices once for the fixed six-slot model. */
        BuildingMapGridPlanner() noexcept;

        /** Plans the supplied visible rectangle into output and reports why invalid geometry or bounds failed. */
        static bool TryCreatePlan(const WorldRectangle& visibleWorld,
            GridPlan& output, PlanFailure& failure) noexcept;

        /** Returns the row-major tile at index in output; an invalid plan or out-of-range index fails. */
        static bool TryGetTileAt(const GridPlan& plan, std::size_t index, GridTile& output) noexcept;

        /** Returns the exact intersecting tile count from plan's range, or zero when plan is invalid. */
        static std::size_t GetIntersectingTileCount(const GridPlan& plan) noexcept;

        /** Plans visibleWorld for sessionEpoch and transactionally reconciles its six slot assignments. */
        bool TryUpdate(const WorldRectangle& visibleWorld, std::uint64_t sessionEpoch,
            PlanFailure& failure) noexcept;

        /** Marks slotIndex content valid for assignmentGeneration and advances its CPU content identity. */
        bool RecordCommittedContent(std::size_t slotIndex,
            std::uint64_t assignmentGeneration) noexcept;

        /** Invalidates slotIndex content only when assignmentGeneration still identifies its assignment. */
        bool InvalidateCommittedContent(std::size_t slotIndex,
            std::uint64_t assignmentGeneration) noexcept;

        /** Marks only the current matching assignment dirty without changing its identity. */
        bool MarkDirty(std::size_t slotIndex, std::uint64_t assignmentGeneration) noexcept;

        /** Retires every current assignment and invalidates the plan during geometry/lifecycle loss. */
        bool RetireAllAssignments() noexcept;

        /** Returns the last successfully reconciled plan, or an invalid empty plan before the first update. */
        const GridPlan& CurrentPlan() const noexcept;

        /** Returns the fixed six-slot CPU assignment state. */
        const std::array<SlotAssignment, kPhysicalSlotCount>& Assignments() const noexcept;

        /** Returns the before/after transitions from the last successful plan update. */
        const SlotTransitionArray& LastTransitions() const noexcept;

    private:
        /** Converts a finite exactly representable floored/ceiled quotient into signed coordinates. */
        static bool TryConvertCoordinate(double value, std::int64_t& output) noexcept;

        /** Multiplies a signed tile coordinate by its positive side only when int64 arithmetic is safe. */
        static bool TryMultiplyCoordinate(std::int64_t coordinate,
            std::int64_t side, std::int64_t& output) noexcept;

        /** Checks one integral tile edge while preserving negative-coordinate boundaries. */
        static bool TryTileEdge(std::int64_t coordinate, std::int64_t side,
            std::int64_t& output) noexcept;

        /** Builds all transition/slot state in temporary arrays before the owner commits it. */
        bool TryReconcile(const GridPlan& plan, std::uint64_t sessionEpoch,
            std::array<SlotAssignment, kPhysicalSlotCount>& nextAssignments,
            SlotTransitionArray& nextTransitions) const noexcept;

        /** Validates and fills the grouping, range, and bounded intersecting-tile count. */
        static bool BuildPlanRange(const WorldRectangle& visibleWorld, std::int64_t groupingN,
            std::int64_t compositeSide, GridPlan& output, PlanFailure& failure) noexcept;

        /** Confirms every tile's exact centimetre edges before the plan becomes valid. */
        static bool ValidatePlanTiles(const GridPlan& plan) noexcept;

        /** Initializes transition snapshots while leaving unchanged and unused slots unclassified. */
        static void InitializeTransitions(SlotTransitionArray& transitions,
            const std::array<SlotAssignment, kPhysicalSlotCount>& assignments) noexcept;

        /** Marks old assignments that exactly match required tile keys as retained in their slots. */
        bool RetainMatchingAssignments(const GridPlan& plan, std::uint64_t sessionEpoch,
            std::array<bool, kPhysicalSlotCount>& occupiedSlots,
            std::array<bool, kPhysicalSlotCount>& matchedTiles,
            SlotTransitionArray& transitions) const noexcept;

        /** Assigns unmatched required tiles to the lowest available physical slot deterministically. */
        bool AssignUnmatchedTiles(const GridPlan& plan, std::uint64_t sessionEpoch,
            std::array<bool, kPhysicalSlotCount>& occupiedSlots,
            const std::array<bool, kPhysicalSlotCount>& matchedTiles,
            std::array<SlotAssignment, kPhysicalSlotCount>& assignments,
            SlotTransitionArray& transitions) const noexcept;

        /** Retires old assignments left outside the new required range and records change reasons. */
        bool RetireUnusedAssignments(const GridPlan& plan, std::uint64_t sessionEpoch,
            const std::array<bool, kPhysicalSlotCount>& occupiedSlots,
            std::array<SlotAssignment, kPhysicalSlotCount>& assignments,
            SlotTransitionArray& transitions) const noexcept;

        /** Finds a requested tile already assigned to an unused physical slot. */
        static bool MatchesTile(const SlotAssignment& assignment, const GridTile& tile,
            std::uint64_t sessionEpoch) noexcept;

        /** Advances generation identities and invalidates committed contents on assignment change. */
        static bool AdvanceAssignmentGeneration(SlotAssignment& assignment) noexcept;

        /** Copies a new tile assignment into a slot after advancing its identities. */
        static bool AssignTile(SlotAssignment& assignment, const GridTile& tile,
            std::uint64_t sessionEpoch) noexcept;

        /** Retires one slot while preserving its physical index and monotonic identity counters. */
        static bool RetireSlot(SlotAssignment& assignment) noexcept;

        /** Adds grouping/session reasons for a changed old assignment relative to a new plan. */
        static void AddPlanChangeReasons(std::uint8_t& changes,
            const SlotAssignment& before, const GridPlan& plan,
            std::uint64_t sessionEpoch) noexcept;

        GridPlan currentPlan_{};
        std::array<SlotAssignment, kPhysicalSlotCount> assignments_{};
        SlotTransitionArray lastTransitions_{};
    };
}
