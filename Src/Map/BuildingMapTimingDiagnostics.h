// Provides thresholded development timing records correlated to one MainMap tick.
#pragma once

#if defined(_DEBUG)
#include "plugin_helpers.h"

#include <chrono>
#include <cstdint>

namespace MainMapBuildingMap
{
    /** Carries already-observed tick state to slow-operation diagnostics without new game queries. */
    class BuildingMapTimingDiagnostics final
    {
    public:
        /** Records one named operation when its owning scope exits. */
        class Scope final
        {
        public:
            /** Starts a no-allocation elapsed-time sample for a static stage name. */
            explicit Scope(const char* stage) noexcept : stage_(stage), started_(
                std::chrono::steady_clock::now())
            {
            }

            /** Emits the completed sample only when it crosses the configured threshold. */
            ~Scope() noexcept
            {
                BuildingMapTimingDiagnostics::Record(stage_, started_);
            }

            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            const char* stage_;
            std::chrono::steady_clock::time_point started_;
        };

        /** Emits one slow preparation pass with all synchronous source-creation samples included. */
        class PreparationScope final
        {
        public:
            /** Samples a grid preparation pass and reads its counters only when the scope exits. */
            PreparationScope(const char* stage, const std::uint64_t& createdTextures,
                const double& creationTotalMs, const double& creationLongestMs) noexcept
                : stage_(stage), createdTextures_(createdTextures), creationTotalMs_(creationTotalMs),
                creationLongestMs_(creationLongestMs), started_(std::chrono::steady_clock::now())
            {
            }

            /** Emits the aggregate only when total preparation reaches 20 ms. */
            ~PreparationScope() noexcept
            {
                BuildingMapTimingDiagnostics::RecordPreparation(stage_, started_, createdTextures_,
                    creationTotalMs_, creationLongestMs_);
            }

            PreparationScope(const PreparationScope&) = delete;
            PreparationScope& operator=(const PreparationScope&) = delete;

        private:
            const char* stage_;
            const std::uint64_t& createdTextures_;
            const double& creationTotalMs_;
            const double& creationLongestMs_;
            std::chrono::steady_clock::time_point started_;
        };

        struct Context final
        {
            std::uint64_t Tick = 0;
            std::uint64_t Images = 0;
            std::uint64_t Sources = 0;
            std::uint64_t Imports = 0;
            std::uint64_t Composites = 0;
            std::int64_t GroupingN = 0;
            bool ZoomPending = false;
            bool LeftButtonHeld = false;
            bool RequestedBoundsChanged = false;
        };

        /** Replaces the correlation snapshot for the current game-thread tick. */
        static void SetContext(const Context& context) noexcept
        {
            CurrentContext() = context;
        }

        /** Returns the current snapshot for an in-flight development stage. */
        static Context GetContext() noexcept
        {
            return CurrentContext();
        }

        /** Emits one record only when elapsed work reaches the 20 ms development threshold. */
        static void Record(const char* stage, std::chrono::steady_clock::time_point started) noexcept
        {
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsedMs < 20.0)
            {
                return;
            }
            const auto& context = CurrentContext();
            const std::uint64_t event = NextEvent();
            LOG_INFO("MainMap timing: event=%llu stage=%s elapsed_ms=%.3f tick=%llu zoom_pending=%u "
                "requested_bounds_changed=%u left_button_held=%u grouping_n=%lld images=%llu sources=%llu "
                "imports=%llu composites=%llu",
                static_cast<unsigned long long>(event), stage, elapsedMs,
                static_cast<unsigned long long>(context.Tick), context.ZoomPending ? 1u : 0u,
                context.RequestedBoundsChanged ? 1u : 0u, context.LeftButtonHeld ? 1u : 0u,
                static_cast<long long>(context.GroupingN),
                static_cast<unsigned long long>(context.Images),
                static_cast<unsigned long long>(context.Sources),
                static_cast<unsigned long long>(context.Imports),
                static_cast<unsigned long long>(context.Composites));
        }

    private:
        /** Retains the game-thread-only diagnostic snapshot without allocating storage. */
        static Context& CurrentContext() noexcept
        {
            static Context context{};
            return context;
        }

        /** Emits one pass aggregate with the same threshold and correlation fields as stage samples. */
        static void RecordPreparation(const char* stage, std::chrono::steady_clock::time_point started,
            std::uint64_t createdTextures, double creationTotalMs, double creationLongestMs) noexcept
        {
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            if (elapsedMs < 20.0)
            {
                return;
            }
            const auto& context = CurrentContext();
            LOG_INFO("MainMap timing: event=%llu stage=%s elapsed_ms=%.3f tick=%llu zoom_pending=%u "
                "requested_bounds_changed=%u left_button_held=%u grouping_n=%lld images=%llu sources=%llu "
                "imports=%llu composites=%llu source_texture_creations=%llu direct_creation_total_ms=%.3f "
                "direct_creation_longest_ms=%.3f",
                static_cast<unsigned long long>(NextEvent()), stage, elapsedMs,
                static_cast<unsigned long long>(context.Tick), context.ZoomPending ? 1u : 0u,
                context.RequestedBoundsChanged ? 1u : 0u, context.LeftButtonHeld ? 1u : 0u,
                static_cast<long long>(context.GroupingN),
                static_cast<unsigned long long>(context.Images),
                static_cast<unsigned long long>(context.Sources),
                static_cast<unsigned long long>(context.Imports),
                static_cast<unsigned long long>(context.Composites),
                static_cast<unsigned long long>(createdTextures), creationTotalMs, creationLongestMs);
        }

        /** Allocates the next process-local slow-event identifier without dynamic storage. */
        static std::uint64_t NextEvent() noexcept
        {
            static std::uint64_t event = 0;
            return ++event;
        }
    };
}
#endif
