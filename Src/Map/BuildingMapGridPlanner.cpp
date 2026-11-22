// Implements checked zero-anchored tile plans and deterministic six-slot state transitions.
#include "BuildingMapGridPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MainMapBuildingMap
{
    /** Initializes physical slot indices once for the fixed six-slot model. */
    BuildingMapGridPlanner::BuildingMapGridPlanner() noexcept
    {
        for (std::size_t slotIndex = 0; slotIndex < kPhysicalSlotCount; ++slotIndex)
        {
            assignments_[slotIndex].PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
            lastTransitions_[slotIndex].Before.PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
            lastTransitions_[slotIndex].After.PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
        }
    }

    /** Creates a complete checked plan; failure clears output and never leaves a partial plan. */
    bool BuildingMapGridPlanner::TryCreatePlan(const WorldRectangle& visibleWorld,
        GridPlan& output, PlanFailure& failure) noexcept
    {
        output = {};
        failure = PlanFailure::None;
        const double width = visibleWorld.MaxX - visibleWorld.MinX;
        const double height = visibleWorld.MaxY - visibleWorld.MinY;
        if (!std::isfinite(visibleWorld.MinX) || !std::isfinite(visibleWorld.MinY) ||
            !std::isfinite(visibleWorld.MaxX) || !std::isfinite(visibleWorld.MaxY) ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0)
        {
            failure = PlanFailure::InvalidRectangle;
            return false;
        }
        const double requiredN = std::ceil((std::max)(width / 2.0, height) /
            static_cast<double>(kBaseSectionSideCentimetres));
        const double grouping = (std::max)(4.0, requiredN);
        const double maximumExactSide = std::ldexp(1.0, 53);
        if (!std::isfinite(grouping) || grouping * kBaseSectionSideCentimetres > maximumExactSide)
        {
            failure = PlanFailure::ArithmeticOutOfRange;
            return false;
        }
        const std::int64_t groupingN = static_cast<std::int64_t>(grouping);
        const std::int64_t compositeSide = groupingN * kBaseSectionSideCentimetres;
        GridPlan candidate{};
        if (!BuildPlanRange(visibleWorld, groupingN, compositeSide, candidate, failure)) return false;
        candidate.IsValid = true;
        if (!ValidatePlanTiles(candidate))
        {
            failure = PlanFailure::ArithmeticOutOfRange;
            return false;
        }
        output = candidate;
        return true;
    }

    /** Returns the exact intersecting tile count for a valid plan, or zero for an invalid plan. */
    std::size_t BuildingMapGridPlanner::GetIntersectingTileCount(const GridPlan& plan) noexcept
    {
        const double exactLimit = std::ldexp(1.0, 53);
        if (!plan.IsValid || plan.GroupingN < 4 ||
            static_cast<double>(plan.GroupingN) * kBaseSectionSideCentimetres > exactLimit ||
            plan.CompositeSideCentimetres != plan.GroupingN * kBaseSectionSideCentimetres ||
            plan.Range.FirstX <= -exactLimit || plan.Range.FirstX >= exactLimit ||
            plan.Range.EndX <= -exactLimit || plan.Range.EndX >= exactLimit ||
            plan.Range.FirstY <= -exactLimit || plan.Range.FirstY >= exactLimit ||
            plan.Range.EndY <= -exactLimit || plan.Range.EndY >= exactLimit) return 0;
        const std::int64_t columns = plan.Range.EndX - plan.Range.FirstX;
        const std::int64_t rows = plan.Range.EndY - plan.Range.FirstY;
        if (columns <= 0 || rows <= 0 || columns > 6 || rows > 6 || columns > 6 / rows) return 0;
        return static_cast<std::size_t>(columns * rows);
    }

    /** Enumerates one checked row-major tile descriptor from a valid plan. */
    bool BuildingMapGridPlanner::TryGetTileAt(const GridPlan& plan,
        std::size_t index, GridTile& output) noexcept
    {
        output = {};
        const std::size_t tileCount = GetIntersectingTileCount(plan);
        if (tileCount == 0 || index >= tileCount) return false;
        const std::int64_t columns = plan.Range.EndX - plan.Range.FirstX;
        const std::size_t columnCount = static_cast<std::size_t>(columns);
        const std::int64_t columnOffset = static_cast<std::int64_t>(index % columnCount);
        const std::int64_t rowOffset = static_cast<std::int64_t>(index / columnCount);
        const std::int64_t tileX = plan.Range.FirstX + columnOffset;
        const std::int64_t tileY = plan.Range.FirstY + rowOffset;
        if (tileX == (std::numeric_limits<std::int64_t>::max)() ||
            tileY == (std::numeric_limits<std::int64_t>::max)()) return false;
        GridTile candidate{};
        candidate.Identity = {plan.GroupingN, tileX, tileY};
        if (!TryTileEdge(tileX, plan.CompositeSideCentimetres, candidate.Bounds.MinX) ||
            !TryTileEdge(tileY, plan.CompositeSideCentimetres, candidate.Bounds.MinY) ||
            !TryTileEdge(tileX + 1, plan.CompositeSideCentimetres, candidate.Bounds.MaxX) ||
            !TryTileEdge(tileY + 1, plan.CompositeSideCentimetres, candidate.Bounds.MaxY)) return false;
        output = candidate;
        return true;
    }

    /** Reconciles plan assignments transactionally and records deterministic slot transitions. */
    bool BuildingMapGridPlanner::TryUpdate(const WorldRectangle& visibleWorld,
        std::uint64_t sessionEpoch, PlanFailure& failure) noexcept
    {
        GridPlan candidatePlan{};
        if (!TryCreatePlan(visibleWorld, candidatePlan, failure)) return false;
        std::array<SlotAssignment, kPhysicalSlotCount> nextAssignments{};
        SlotTransitionArray nextTransitions{};
        if (!TryReconcile(candidatePlan, sessionEpoch, nextAssignments, nextTransitions))
        {
            failure = PlanFailure::AssignmentGenerationOutOfRange;
            return false;
        }
        currentPlan_ = candidatePlan;
        assignments_ = nextAssignments;
        lastTransitions_ = nextTransitions;
        failure = PlanFailure::None;
        return true;
    }

    /** Records a CPU-side committed-content identity reported by a future composition owner. */
    bool BuildingMapGridPlanner::RecordCommittedContent(std::size_t slotIndex,
        std::uint64_t assignmentGeneration) noexcept
    {
        if (slotIndex >= kPhysicalSlotCount) return false;
        SlotAssignment& assignment = assignments_[slotIndex];
        if (!assignment.IsAssigned || assignment.AssignmentGeneration != assignmentGeneration ||
            assignment.CommittedContentGeneration == (std::numeric_limits<std::uint64_t>::max)()) return false;
        ++assignment.CommittedContentGeneration;
        assignment.HasCommittedContent = true;
        assignment.IsDirty = false;
        return true;
    }

    /** Invalidates committed content for the current assignment without claiming GPU completion. */
    bool BuildingMapGridPlanner::InvalidateCommittedContent(std::size_t slotIndex,
        std::uint64_t assignmentGeneration) noexcept
    {
        if (slotIndex >= kPhysicalSlotCount) return false;
        SlotAssignment& assignment = assignments_[slotIndex];
        if (!assignment.IsAssigned || assignment.AssignmentGeneration != assignmentGeneration) return false;
        if (assignment.HasCommittedContent)
        {
            if (assignment.CommittedContentGeneration == (std::numeric_limits<std::uint64_t>::max)()) return false;
            ++assignment.CommittedContentGeneration;
            assignment.HasCommittedContent = false;
        }
        assignment.IsDirty = true;
        return true;
    }

    /** Marks one assigned tile for future work while preserving assignment and publication identity. */
    bool BuildingMapGridPlanner::MarkDirty(std::size_t slotIndex,
        std::uint64_t assignmentGeneration) noexcept
    {
        if (slotIndex >= kPhysicalSlotCount) return false;
        SlotAssignment& assignment = assignments_[slotIndex];
        if (!assignment.IsAssigned || assignment.AssignmentGeneration != assignmentGeneration) return false;
        assignment.IsDirty = true;
        return true;
    }

    /** Retires all assigned tiles and invalidates the current plan during lifecycle loss. */
    bool BuildingMapGridPlanner::RetireAllAssignments() noexcept
    {
        std::array<SlotAssignment, kPhysicalSlotCount> nextAssignments = assignments_;
        SlotTransitionArray nextTransitions{};
        InitializeTransitions(nextTransitions, assignments_);
        for (std::size_t slotIndex = 0; slotIndex < kPhysicalSlotCount; ++slotIndex)
        {
            if (!nextAssignments[slotIndex].IsAssigned) continue;
            nextTransitions[slotIndex].Changes = RetiredAssignment;
            if (!RetireSlot(nextAssignments[slotIndex])) return false;
            nextTransitions[slotIndex].After = nextAssignments[slotIndex];
        }
        assignments_ = nextAssignments;
        lastTransitions_ = nextTransitions;
        currentPlan_ = {};
        return true;
    }

    /** Returns the last successfully reconciled plan, or an invalid empty plan before the first update. */
    const BuildingMapGridPlanner::GridPlan& BuildingMapGridPlanner::CurrentPlan() const noexcept
    {
        return currentPlan_;
    }

    /** Returns the fixed six-slot CPU assignment state. */
    const std::array<BuildingMapGridPlanner::SlotAssignment,
        BuildingMapGridPlanner::kPhysicalSlotCount>& BuildingMapGridPlanner::Assignments() const noexcept
    {
        return assignments_;
    }

    /** Returns the before/after transitions from the last successful plan update. */
    const BuildingMapGridPlanner::SlotTransitionArray& BuildingMapGridPlanner::LastTransitions() const noexcept
    {
        return lastTransitions_;
    }

    /** Converts a finite exactly representable floored/ceiled quotient into signed coordinates. */
    bool BuildingMapGridPlanner::TryConvertCoordinate(double value, std::int64_t& output) noexcept
    {
        const double exactLimit = std::ldexp(1.0, 53);
        if (!std::isfinite(value) || value <= -exactLimit || value >= exactLimit) return false;
        output = static_cast<std::int64_t>(value);
        return true;
    }

    /** Multiplies a signed tile coordinate by its positive side only when int64 arithmetic is safe. */
    bool BuildingMapGridPlanner::TryMultiplyCoordinate(std::int64_t coordinate,
        std::int64_t side, std::int64_t& output) noexcept
    {
        if (side <= 0 || coordinate > (std::numeric_limits<std::int64_t>::max)() / side ||
            coordinate < (std::numeric_limits<std::int64_t>::min)() / side) return false;
        output = coordinate * side;
        return true;
    }

    /** Checks one integral tile edge while preserving negative-coordinate boundaries. */
    bool BuildingMapGridPlanner::TryTileEdge(std::int64_t coordinate,
        std::int64_t side, std::int64_t& output) noexcept
    {
        return TryMultiplyCoordinate(coordinate, side, output);
    }

    /** Builds all transition/slot state in temporary arrays before the owner commits it. */
    bool BuildingMapGridPlanner::TryReconcile(const GridPlan& plan, std::uint64_t sessionEpoch,
        std::array<SlotAssignment, kPhysicalSlotCount>& nextAssignments,
        SlotTransitionArray& nextTransitions) const noexcept
    {
        nextAssignments = assignments_;
        std::array<bool, kPhysicalSlotCount> occupiedSlots{};
        std::array<bool, kPhysicalSlotCount> matchedTiles{};
        InitializeTransitions(nextTransitions, assignments_);
        if (!RetainMatchingAssignments(plan, sessionEpoch, occupiedSlots, matchedTiles, nextTransitions) ||
            !AssignUnmatchedTiles(plan, sessionEpoch, occupiedSlots, matchedTiles, nextAssignments, nextTransitions) ||
            !RetireUnusedAssignments(plan, sessionEpoch, occupiedSlots, nextAssignments, nextTransitions)) return false;
        return true;
    }

    /** Checks whether an assigned slot already represents a plan tile in the same session. */
    bool BuildingMapGridPlanner::MatchesTile(const SlotAssignment& assignment,
        const GridTile& tile, std::uint64_t sessionEpoch) noexcept
    {
        return assignment.IsAssigned && assignment.SessionEpoch == sessionEpoch &&
            assignment.Tile.GroupingN == tile.Identity.GroupingN &&
            assignment.Tile.X == tile.Identity.X && assignment.Tile.Y == tile.Identity.Y;
    }

    /** Advances assignment/content generations and invalidates the previous committed identity. */
    bool BuildingMapGridPlanner::AdvanceAssignmentGeneration(SlotAssignment& assignment) noexcept
    {
        if (assignment.AssignmentGeneration == (std::numeric_limits<std::uint64_t>::max)() ||
            assignment.CommittedContentGeneration == (std::numeric_limits<std::uint64_t>::max)()) return false;
        ++assignment.AssignmentGeneration;
        ++assignment.CommittedContentGeneration;
        assignment.HasCommittedContent = false;
        assignment.IsDirty = true;
        return true;
    }

    /** Copies a new tile assignment into a slot after advancing its identities. */
    bool BuildingMapGridPlanner::AssignTile(SlotAssignment& assignment,
        const GridTile& tile, std::uint64_t sessionEpoch) noexcept
    {
        if (!AdvanceAssignmentGeneration(assignment)) return false;
        assignment.IsAssigned = true;
        assignment.Tile = tile.Identity;
        assignment.SessionEpoch = sessionEpoch;
        assignment.Bounds = tile.Bounds;
        return true;
    }

    /** Retires one slot while preserving its physical index and monotonic identity counters. */
    bool BuildingMapGridPlanner::RetireSlot(SlotAssignment& assignment) noexcept
    {
        if (!AdvanceAssignmentGeneration(assignment)) return false;
        assignment.IsAssigned = false;
        assignment.Tile = {};
        assignment.SessionEpoch = 0;
        assignment.Bounds = {};
        assignment.IsDirty = false;
        return true;
    }

    /** Adds grouping/session reasons for a changed old assignment relative to a new plan. */
    void BuildingMapGridPlanner::AddPlanChangeReasons(std::uint8_t& changes,
        const SlotAssignment& before, const GridPlan& plan, std::uint64_t sessionEpoch) noexcept
    {
        if (before.IsAssigned && before.Tile.GroupingN != plan.GroupingN) changes |= GroupingChanged;
        if (before.IsAssigned && before.SessionEpoch != sessionEpoch) changes |= SessionChanged;
    }

    /** Validates and fills the grouping, range, and bounded intersecting-tile count. */
    bool BuildingMapGridPlanner::BuildPlanRange(const WorldRectangle& visibleWorld,
        std::int64_t groupingN, std::int64_t compositeSide, GridPlan& output,
        PlanFailure& failure) noexcept
    {
        const double side = static_cast<double>(compositeSide);
        const double firstX = std::floor(visibleWorld.MinX / side);
        const double endX = std::ceil(visibleWorld.MaxX / side);
        const double firstY = std::floor(visibleWorld.MinY / side);
        const double endY = std::ceil(visibleWorld.MaxY / side);
        TileCoordinateRange range{};
        if (!TryConvertCoordinate(firstX, range.FirstX) || !TryConvertCoordinate(endX, range.EndX) ||
            !TryConvertCoordinate(firstY, range.FirstY) || !TryConvertCoordinate(endY, range.EndY))
        {
            failure = PlanFailure::ArithmeticOutOfRange;
            return false;
        }
        const std::int64_t columns = range.EndX - range.FirstX;
        const std::int64_t rows = range.EndY - range.FirstY;
        if (columns <= 0 || rows <= 0 || columns > 6 || rows > 6 || columns > 6 / rows)
        {
            failure = PlanFailure::TileCountOutOfRange;
            return false;
        }
        output.GroupingN = groupingN;
        output.CompositeSideCentimetres = compositeSide;
        output.Range = range;
        return true;
    }

    /** Confirms every tile's exact centimetre edges before the plan becomes valid. */
    bool BuildingMapGridPlanner::ValidatePlanTiles(const GridPlan& plan) noexcept
    {
        const std::int64_t columns = plan.Range.EndX - plan.Range.FirstX;
        const std::int64_t rows = plan.Range.EndY - plan.Range.FirstY;
        const std::size_t tileCount = static_cast<std::size_t>(columns * rows);
        for (std::size_t index = 0; index < tileCount; ++index)
        {
            GridTile tile{};
            if (!TryGetTileAt(plan, index, tile)) return false;
        }
        return true;
    }

    /** Initializes transition snapshots while leaving unchanged and unused slots unclassified. */
    void BuildingMapGridPlanner::InitializeTransitions(SlotTransitionArray& transitions,
        const std::array<SlotAssignment, kPhysicalSlotCount>& assignments) noexcept
    {
        for (std::size_t slotIndex = 0; slotIndex < kPhysicalSlotCount; ++slotIndex)
        {
            transitions[slotIndex] = {};
            transitions[slotIndex].Before = assignments[slotIndex];
            transitions[slotIndex].After = assignments[slotIndex];
        }
    }

    /** Marks old assignments that exactly match required tile keys as retained in their slots. */
    bool BuildingMapGridPlanner::RetainMatchingAssignments(const GridPlan& plan,
        std::uint64_t sessionEpoch, std::array<bool, kPhysicalSlotCount>& occupiedSlots,
        std::array<bool, kPhysicalSlotCount>& matchedTiles,
        SlotTransitionArray& transitions) const noexcept
    {
        const std::size_t tileCount = GetIntersectingTileCount(plan);
        for (std::size_t slotIndex = 0; slotIndex < kPhysicalSlotCount; ++slotIndex)
        {
            if (!assignments_[slotIndex].IsAssigned) continue;
            for (std::size_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
            {
                GridTile tile{};
                if (!TryGetTileAt(plan, tileIndex, tile)) return false;
                if (!matchedTiles[tileIndex] && MatchesTile(assignments_[slotIndex], tile, sessionEpoch))
                {
                    occupiedSlots[slotIndex] = true;
                    matchedTiles[tileIndex] = true;
                    transitions[slotIndex].Changes = RetainedAssignment;
                    break;
                }
            }
        }
        return true;
    }

    /** Assigns unmatched required tiles to the lowest available physical slot deterministically. */
    bool BuildingMapGridPlanner::AssignUnmatchedTiles(const GridPlan& plan,
        std::uint64_t sessionEpoch, std::array<bool, kPhysicalSlotCount>& occupiedSlots,
        const std::array<bool, kPhysicalSlotCount>& matchedTiles,
        std::array<SlotAssignment, kPhysicalSlotCount>& assignments,
        SlotTransitionArray& transitions) const noexcept
    {
        const std::size_t tileCount = GetIntersectingTileCount(plan);
        for (std::size_t tileIndex = 0; tileIndex < tileCount; ++tileIndex)
        {
            if (matchedTiles[tileIndex]) continue;
            std::size_t slotIndex = 0;
            while (slotIndex < kPhysicalSlotCount && occupiedSlots[slotIndex]) ++slotIndex;
            GridTile tile{};
            if (slotIndex == kPhysicalSlotCount || !TryGetTileAt(plan, tileIndex, tile)) return false;
            SlotAssignment& assignment = assignments[slotIndex];
            if (assignment.IsAssigned)
            {
                transitions[slotIndex].Changes |= RetiredAssignment;
                AddPlanChangeReasons(transitions[slotIndex].Changes,
                    assignment, plan, sessionEpoch);
            }
            transitions[slotIndex].Changes |= NewlyAssigned;
            if (!AssignTile(assignment, tile, sessionEpoch)) return false;
            occupiedSlots[slotIndex] = true;
            transitions[slotIndex].After = assignment;
        }
        return true;
    }

    /** Retires old assignments left outside the new required range and records change reasons. */
    bool BuildingMapGridPlanner::RetireUnusedAssignments(const GridPlan& plan,
        std::uint64_t sessionEpoch, const std::array<bool, kPhysicalSlotCount>& occupiedSlots,
        std::array<SlotAssignment, kPhysicalSlotCount>& assignments,
        SlotTransitionArray& transitions) const noexcept
    {
        for (std::size_t slotIndex = 0; slotIndex < kPhysicalSlotCount; ++slotIndex)
        {
            if (occupiedSlots[slotIndex] || !assignments[slotIndex].IsAssigned) continue;
            AddPlanChangeReasons(transitions[slotIndex].Changes,
                assignments[slotIndex], plan, sessionEpoch);
            transitions[slotIndex].Changes |= RetiredAssignment;
            if (!RetireSlot(assignments[slotIndex])) return false;
            transitions[slotIndex].After = assignments[slotIndex];
        }
        return true;
    }

}
