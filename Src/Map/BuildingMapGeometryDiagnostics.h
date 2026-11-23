// Temporary debug-only measurements and zero-anchored composite coverage.
// Owns scalar diagnostic state only; never owns widgets, images, or native leases.
#pragma once

#if defined(_DEBUG)
#include "BuildingLayer.h"
#include "Native/NativeApi.h"
#include "ChimeraUI_classes.hpp"
#include "UMG_classes.hpp"
#include "plugin_helpers.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace MainMapBuildingMap
{
    /** Samples admitted live terrain geometry on the game thread without retaining game objects. */
    class BuildingMapGeometryDiagnostics final
    {
    public:
        /** Reads borrowed terrain/context and centimetre bounds on the admitted game thread; emits changed samples only. */
        static void Sample(SDK::UCrUW_MapMenuTerrain* terrain,
            const MainMapBuildingLayer::ConsumerContext& context,
            const MainMapBuildingLayer::WorldRect& rect)
        {
            if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread()) return;
            const auto now = GetTickCount64();
            if (hasSample_ && now - lastTime_ < kMinimumSampleIntervalMs) return;
            double values[13]{};
            if (!Measure(terrain, context, rect, values))
            {
                if (!unavailableLogged_)
                {
                    LogUnavailable(context, terrain);
                }
                unavailableLogged_ = true;
                lastTime_ = now;
                hasSample_ = true;
                return;
            }
            const bool sessionChanged = context.epoch != lastEpoch_ || context.world != lastWorld_ || terrain != lastTerrain_;
            const bool changed = !hasSample_ || sessionChanged || Changed(values) || CoverageChanged(rect, false);
            if (!changed && !unavailableLogged_) return;
            unavailableLogged_ = false;
            lastEpoch_ = context.epoch;
            lastWorld_ = context.world;
            lastTerrain_ = terrain;
            lastTime_ = now;
            hasSample_ = true;
            ++sampleCount_;
            std::copy(values, values + 13, previous_);
            CoverageChanged(rect, true);
            LogGeometry(terrain, context, values);
            LogCoverage(rect);
        }

    private:
        static constexpr ULONGLONG kMinimumSampleIntervalMs = 1000;
        inline static double previous_[13]{};
        inline static double previousCoverage_[80]{};
        inline static uint64_t lastEpoch_ = 0;
        inline static ULONGLONG lastTime_ = 0;
        inline static unsigned sampleCount_ = 0;
        inline static bool unavailableLogged_ = false;
        inline static bool hasSample_ = false;
        inline static SDK::UWorld* lastWorld_ = nullptr;
        inline static SDK::UCrUW_MapMenuTerrain* lastTerrain_ = nullptr;

        /** Returns whether measured values differ from the last log beyond subpixel/one-centimetre tolerances. */
        static bool Changed(const double* values)
        {
            for (unsigned index = 0; index < 13; ++index)
            {
                const double tolerance = index == 2 ? 0.001 : (index >= 7 && index <= 10 ? 1.0 : 0.5);
                if (std::abs(values[index] - previous_[index]) > tolerance) return true;
            }
            return false;
        }

        /** Returns discrete coverage changes for centimetre bounds; remember commits the scalar comparison state. */
        static bool CoverageChanged(const MainMapBuildingLayer::WorldRect& rect, bool remember)
        {
            const double width = rect.maxX-rect.minX;
            const double height = rect.maxY-rect.minY;
            double coverage[80]{};
            coverage[0] = (std::max)(4.0, std::ceil((std::max)(width/2.0,height)/12500.0));
            coverage[1] = (std::max)(4.0, std::ceil((std::max)(width,height)/12500.0));
            for (unsigned candidate = 0; candidate < 13; ++candidate)
            {
                const double grouping = candidate < 12 ? candidate+4.0 : coverage[0];
                const double side = grouping*12500.0;
                const unsigned offset = 2+candidate*6;
                coverage[offset] = std::floor(rect.minX/side);
                coverage[offset+1] = std::ceil(rect.maxX/side);
                coverage[offset+2] = std::floor(rect.minY/side);
                coverage[offset+3] = std::ceil(rect.maxY/side);
                coverage[offset+4] = std::ceil(width/side)+1.0;
                coverage[offset+5] = std::ceil(height/side)+1.0;
            }
            const bool changed = !std::equal(coverage, coverage+80, previousCoverage_);
            if (remember) std::copy(coverage, coverage+80, previousCoverage_);
            return changed;
        }

        /** Fills measured values from borrowed terrain/context and centimetre clip bounds; false means unsupported geometry. */
        static bool Measure(SDK::UCrUW_MapMenuTerrain* terrain,
            const MainMapBuildingLayer::ConsumerContext& context,
            const MainMapBuildingLayer::WorldRect& rect, double* values)
        {
            double span = 0;
            if (!terrain || !MainMapBuildingLayer::TryGetGroupedTilePixelSpan(span)) return false;
            const auto output = SDK::UWidgetLayoutLibrary::GetViewportSize(context.world);
            const double scale = SDK::UWidgetLayoutLibrary::GetViewportScale(context.world);
            const auto geometry = terrain->GetPaintSpaceGeometry();
            const double coordinates[4] = {(rect.minX-context.originX)/100.0,
                (rect.minY-context.originY)/100.0, (rect.maxX-context.originX)/100.0,
                (rect.maxY-context.originY)/100.0};
            for (double coordinate : coordinates)
                if (!std::isfinite(coordinate) || std::abs(coordinate) > (std::numeric_limits<float>::max)()) return false;
            SDK::FVector2D first{}, end{}, unused{};
            SDK::USlateBlueprintLibrary::LocalToViewport(context.world, geometry,
                {static_cast<float>(coordinates[0]), static_cast<float>(coordinates[1])}, &first, &unused);
            SDK::USlateBlueprintLibrary::LocalToViewport(context.world, geometry,
                {static_cast<float>(coordinates[2]), static_cast<float>(coordinates[3])}, &end, &unused);
            const double measured[13] = {output.X, output.Y, scale, first.X, first.Y,
                std::abs(end.X-first.X), std::abs(end.Y-first.Y),
                rect.minX, rect.minY, rect.maxX-rect.minX, rect.maxY-rect.minY, span/4.0, span};
            for (unsigned index = 0; index < 13; ++index)
                if (!std::isfinite(measured[index]) || (index != 7 && index != 8 && measured[index] <= 0)) return false;
            std::copy(measured, measured + 13, values);
            return true;
        }

        /** Emits measured units and correlation, without treating the configured zoom as live zoom. */
        static void LogGeometry(SDK::UCrUW_MapMenuTerrain* terrain,
            const MainMapBuildingLayer::ConsumerContext& context,
            const double* values)
        {
            LOG_INFO("MainMap grid diagnostic: sample=%u epoch=%llu world=%p terrain=%p admission=1 GT=1 visible=1 "
                "originCm=(%.6f,%.6f) outputPx=(%.3f,%.3f) uiScale=%.6f clipPx=(%.3f,%.3f,%.3f,%.3f) "
                "worldCm=(%.6f,%.6f,%.6f,%.6f) section125mPx=%.6f composite500mPx=%.6f pixelsPerMetre=%.9f",
                sampleCount_, static_cast<unsigned long long>(context.epoch), context.world, terrain,
                context.originX, context.originY, values[0], values[1], values[2], values[3], values[4],
                values[5], values[6], values[7], values[8], values[9], values[10], values[11], values[12], values[12]/500.0);
        }

        /** Emits one unavailable record until geometry becomes available or the state changes. */
        static void LogUnavailable(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCrUW_MapMenuTerrain* terrain)
        {
            ++sampleCount_;
            LOG_INFO("MainMap grid diagnostic: sample=%u geometry unavailable epoch=%llu world=%p terrain=%p admission=1 GT=1",
                sampleCount_, static_cast<unsigned long long>(context.epoch), context.world, terrain);
        }

        /** Writes an integral double into coordinate if within the exact signed domain; false leaves output unchanged. */
        static bool ToCoordinate(double value, int64_t& coordinate)
        {
            const double limit = std::ldexp(1.0, 53);
            if (!std::isfinite(value) || value <= -limit || value >= limit) return false;
            coordinate = static_cast<int64_t>(value);
            return true;
        }

        /** Appends exact half-open membership and separate worst-alignment capacity for one N. */
        static void AppendCandidate(std::string& output,
            const MainMapBuildingLayer::WorldRect& rect, int64_t grouping)
        {
            const double side = static_cast<double>(grouping)*12500.0;
            int64_t firstX = 0;
            int64_t firstY = 0;
            int64_t endX = 0;
            int64_t endY = 0;
            int64_t guaranteedColumns = 0;
            int64_t guaranteedRows = 0;
            if (!ToCoordinate(std::floor(rect.minX/side), firstX) ||
                !ToCoordinate(std::floor(rect.minY/side), firstY) ||
                !ToCoordinate(std::ceil(rect.maxX/side), endX) ||
                !ToCoordinate(std::ceil(rect.maxY/side), endY) ||
                !ToCoordinate(std::ceil((rect.maxX-rect.minX)/side)+1.0, guaranteedColumns) ||
                !ToCoordinate(std::ceil((rect.maxY-rect.minY)/side)+1.0, guaranteedRows))
            {
                output += " N" + std::to_string(grouping) + "=unavailable";
                return;
            }
            const int64_t columns = endX-firstX;
            const int64_t rows = endY-firstY;
            if (columns <= 0 || rows <= 0 || columns > (std::numeric_limits<int64_t>::max)()/rows)
            {
                output += " N" + std::to_string(grouping) + "=overflow";
                return;
            }
            output += " N" + std::to_string(grouping) + "=[" + std::to_string(firstX) + "," +
                std::to_string(endX) + ")x[" + std::to_string(firstY) + "," + std::to_string(endY) +
                ")=" + std::to_string(columns) + "x" + std::to_string(rows) + "#" +
                std::to_string(columns*rows) + " cap=" + std::to_string(guaranteedColumns) + "x" +
                std::to_string(guaranteedRows);
        }

        /** Evaluates sequential N=4..15 and the measured 3x2 minimum even when it exceeds 15. */
        static void LogCoverage(const MainMapBuildingLayer::WorldRect& rect)
        {
            const double width = rect.maxX-rect.minX;
            const double height = rect.maxY-rect.minY;
            int64_t minimum32 = 0;
            int64_t minimum22 = 0;
            if (!ToCoordinate((std::max)(4.0, std::ceil((std::max)(width/2.0,height)/12500.0)), minimum32) ||
                !ToCoordinate((std::max)(4.0, std::ceil((std::max)(width,height)/12500.0)), minimum22))
            {
                LOG_INFO("MainMap grid diagnostic: sample=%u minimum N unavailable (nonfinite/integer precision)", sampleCount_);
                return;
            }
            std::string firstCoverage = "MainMap grid diagnostic: sample=" + std::to_string(sampleCount_) +
                " minN3x2=" + std::to_string(minimum32) + " minN2x2=" + std::to_string(minimum22);
            for (int64_t grouping = 4; grouping <= 9; ++grouping) AppendCandidate(firstCoverage, rect, grouping);
            LOG_INFO("%s", firstCoverage.c_str());
            std::string remainingCoverage = "MainMap grid diagnostic: sample=" + std::to_string(sampleCount_) + " continued";
            for (int64_t grouping = 10; grouping <= 15; ++grouping) AppendCandidate(remainingCoverage, rect, grouping);
            if (minimum32 > 15) AppendCandidate(remainingCoverage, rect, minimum32);
            LOG_INFO("%s", remainingCoverage.c_str());
        }
    };
}
#endif
