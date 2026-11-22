// Consumes MiniMap's polling-only Building Map ABI and owns MainMap copies of
// the completed images currently visible through one remote viewport.
#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "../../../../MiniMap/Solution/Src/Services/Map/Buildings/BuildingMapServiceABI.h"

namespace MainMapBuildingMap
{
    /** A copied completed RGBA8 image keyed by its signed provider Section coordinates. */
    struct SectionImage final
    {
        int64_t GridX = 0;
        int64_t GridY = 0;
        double XMapUnits = 0.0;
        double YMapUnits = 0.0;
        double SectionSizeMapUnits = 0.0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint64_t RowStride = 0;
        uint64_t ByteCount = 0;
        uint64_t Revision = 0;
        std::vector<uint8_t> Pixels;
    };

    /** Owns one opaque MiniMap collector/viewport and the consumer's visible image copies. */
    class BuildingMapRemote final
    {
    public:
        /** Describes whether the latest visible-cell poll is a complete current snapshot. */
        enum class PollState
        {
            Unavailable,
            Incomplete,
            Complete
        };

        /** Availability observed from discovery and QueryState; images may still be incomplete. */
        enum class Availability
        {
            ProviderUnavailable,
            ABIIncompatible,
            ServiceUnavailable,
            NoGameplaySession,
            CollectorInvalidated,
            ViewportInvalidated,
            Active,
            Failure
        };

        /** Reports whether viewport bounds were accepted and whether this call opened its lifetime. */
        enum class ViewportUpdateResult
        {
            NoViewport,
            Rejected,
            Opened,
            Updated
        };

        /** Global signed Section coordinate to consumer-owned completed RGBA8 image. */
        using ImageCollection = std::map<std::pair<int64_t, int64_t>, SectionImage>;
        /** Signed global Section coordinate observed as NO_IMAGE in the last complete poll. */
        using SectionCoordinate = std::pair<int64_t, int64_t>;
        using NoImageCollection = std::vector<SectionCoordinate>;

        /** Updates an active demand with unchanged map-unit rectangle values and polls its images. */
        ViewportUpdateResult UpdateViewport(double xMapUnits, double yMapUnits,
            double widthMapUnits, double heightMapUnits);
        /** Closes valid provider ownership when callable and clears all consumer image state. */
        void CloseViewport();
        /** Releases a viewport during the existing plugin shutdown path. */
        void Shutdown();
        /** Returns and clears the change flag after image additions, updates, or removals. */
        bool ConsumeImageChanges() noexcept;
        /** Returns the current consumer-owned image collection for later presentation integration. */
        const ImageCollection& Images() const noexcept;
        /** Returns the most recently observed provider/session/operation condition. */
        Availability GetAvailability() const noexcept;
        /** Returns the completeness of the latest visible grid poll. */
        PollState GetPollState() const noexcept;
        /** Returns coordinates reported NO_IMAGE by the latest complete poll. */
        const NoImageCollection& NoImageSections() const noexcept;
        /** Returns a monotonic token advanced when accepted source observations change. */
        uint64_t GetSourceObservationRevision() const noexcept;

    private:
        /** Resolves and validates MiniMap's exported v1 table without linking to its export. */
        bool DiscoverProvider();
        /** Queries service/session and both opaque handles into an initialized public ABI state. */
        BuildingMapABIResult QueryProviderState(BuildingMapABIState& state);
        /** Creates an opaque collector and opens its ordinary CPU-image viewport when needed. */
        bool EnsureViewport(double xMapUnits, double yMapUnits, double widthMapUnits, double heightMapUnits,
            bool& opened);
        /** Checks provider/session/handle state and prepares valid ownership for this demand. */
        bool PrepareViewport(double xMapUnits, double yMapUnits, double widthMapUnits, double heightMapUnits,
            bool& opened);
        /** Advances one provider update and polls the resulting current viewport grid. */
        bool AdvanceViewportAndPoll(double xMapUnits, double yMapUnits,
            double widthMapUnits, double heightMapUnits, bool& updateAccepted);
        /** Copies each visible cell and commits availability/NO_IMAGE state only after a full poll. */
        bool PollVisibleImages(uint64_t visibleWidth, uint64_t visibleHeight);
        /** Sorts and validates the current available/NO_IMAGE coordinate scratch sets. */
        bool ValidatePollCoordinates();
        /** Retrieves one local cell using metadata-first buffer sizing and bounded retry. */
        BuildingMapABIResult CopySectionImage(uint64_t localX, uint64_t localY, BuildingMapABISectionImage& metadata);
        /** Resizes reusable MainMap scratch storage to a provider-reported byte count. */
        bool ResizeCopyBuffer(uint64_t requiredBytes) noexcept;
        /** Compares copied metadata and bytes, replacing only changed consumer-owned storage. */
        bool StoreChangedImage(const BuildingMapABISectionImage& metadata);
        /** Clears invalidated opaque handles without calling the provider. */
        void AbandonHandles() noexcept;
        /** Releases only ownership the provider still validates after partial invalidation. */
        void ReleaseValidOwnershipAfterHandleInvalidation();
        /** Clears the current visible image set and records a change when content was present. */
        void ClearImages() noexcept;
        /** Advances the source observation token unless its finite identity space is exhausted. */
        bool AdvanceSourceObservationRevision() noexcept;
        /** Disposes a created collector only when QueryState still reports it valid. */
        void DisposeUnusedCollector();
        /** Checks the calling context before any serialized provider operation. */
        bool IsAdmittedGameThread() const noexcept;

        BuildingMapServiceABITable table_{};
        BuildingMapCollectorHandle collector_ = 0;
        BuildingMapViewportHandle viewport_ = 0;
        ImageCollection images_;
        NoImageCollection noImageSections_;
        NoImageCollection observedImageScratch_;
        NoImageCollection noImageScratch_;
        std::vector<uint8_t> copyBuffer_;
        uint64_t lastImageRevision_ = 0;
        uint64_t sourceObservationRevision_ = 0;
        Availability availability_ = Availability::ProviderUnavailable;
        PollState pollState_ = PollState::Unavailable;
        bool tableAvailable_ = false;
        bool imagesChanged_ = false;
        bool firstImageAvailabilityLogged_ = false;
    };
}
