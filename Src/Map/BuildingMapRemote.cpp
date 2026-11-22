// Implements MainMap's consumer-driven access to MiniMap's completed Section
// images while keeping provider objects and scheduling entirely in MiniMap.
#include "BuildingMapRemote.h"

#include "Native/NativeApi.h"
#include "plugin_helpers.h"

#include <Windows.h>

#include <algorithm>
#include <limits>

namespace MainMapBuildingMap
{
    /** Resolves the provider export and accepts only the complete authoritative v1 table. */
    bool BuildingMapRemote::DiscoverProvider()
    {
        const HMODULE module = GetModuleHandleW(L"MiniMap.dll");
        if (module == nullptr)
        {
            availability_ = Availability::ProviderUnavailable;
            return false;
        }

        using Getter = BuildingMapABIResult (BUILDING_MAP_ABI_CALL*)(
            uint32_t, uint32_t, BuildingMapServiceABITable*);
        const auto getter = reinterpret_cast<Getter>(GetProcAddress(module, "MiniMap_GetBuildingMapServiceABI"));
        if (getter == nullptr)
        {
            availability_ = Availability::ABIIncompatible;
            return false;
        }

        BuildingMapServiceABITable candidate{};
        const auto result = getter(BUILDING_MAP_ABI_VERSION, sizeof(candidate), &candidate);
        if (result != BUILDING_MAP_ABI_OK || candidate.StructSize != sizeof(candidate) ||
            candidate.Version != BUILDING_MAP_ABI_VERSION ||
            candidate.ContractId != BUILDING_MAP_ABI_CONTRACT_ID || candidate.QueryState == nullptr ||
            candidate.CreateCollector == nullptr || candidate.DisposeCollector == nullptr ||
            candidate.OpenViewport == nullptr || candidate.UpdateViewport == nullptr ||
            candidate.CopySectionImage == nullptr || candidate.CloseViewport == nullptr)
        {
            availability_ = Availability::ABIIncompatible;
            return false;
        }

        table_ = candidate;
        tableAvailable_ = true;
        availability_ = Availability::ServiceUnavailable;
        return true;
    }

    /** Checks thread admission before querying service and opaque-handle state. */
    BuildingMapABIResult BuildingMapRemote::QueryProviderState(BuildingMapABIState& state)
    {
        state = {};
        state.StructSize = sizeof(state);
        return table_.QueryState(collector_, viewport_, &state);
    }

    /** Creates the provider-owned collector and opens an ordinary viewport when absent. */
    bool BuildingMapRemote::EnsureViewport(double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits)
    {
        if (collector_ == 0)
        {
            const auto result = table_.CreateCollector(&collector_);
            if (result != BUILDING_MAP_ABI_OK || collector_ == 0)
            {
                availability_ = result == BUILDING_MAP_ABI_NO_SESSION
                    ? Availability::NoGameplaySession : Availability::Failure;
                collector_ = 0;
                return false;
            }
        }

        if (viewport_ != 0)
        {
            return true;
        }

        const auto result = table_.OpenViewport(collector_, xMapUnits, yMapUnits,
            widthMapUnits, heightMapUnits, &viewport_);
        if (result == BUILDING_MAP_ABI_OK && viewport_ != 0)
        {
            return true;
        }

        viewport_ = 0;
        DisposeUnusedCollector();
        availability_ = result == BUILDING_MAP_ABI_NO_SESSION
            ? Availability::NoGameplaySession : Availability::Failure;
        return false;
    }

    /** Confirms live provider/session state and prepares a current collector and viewport. */
    bool BuildingMapRemote::PrepareViewport(double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits)
    {
        BuildingMapABIState state{};
        const auto result = QueryProviderState(state);
        if (result != BUILDING_MAP_ABI_OK)
        {
            availability_ = Availability::Failure;
            return false;
        }
        if (state.ServiceAvailable == 0)
        {
            availability_ = Availability::ServiceUnavailable;
            AbandonHandles();
            ClearImages();
            return false;
        }
        if (state.SessionAvailable == 0)
        {
            availability_ = Availability::NoGameplaySession;
            AbandonHandles();
            ClearImages();
            return false;
        }
        if ((collector_ != 0 && state.CollectorValid == 0) ||
            (viewport_ != 0 && state.ViewportValid == 0))
        {
            availability_ = collector_ != 0 && state.CollectorValid == 0
                ? Availability::CollectorInvalidated : Availability::ViewportInvalidated;
            ReleaseValidOwnershipAfterHandleInvalidation();
            AbandonHandles();
            ClearImages();
        }
        return EnsureViewport(xMapUnits, yMapUnits, widthMapUnits, heightMapUnits);
    }

    /** Updates even an unchanged rectangle, validates ownership again, and polls current images. */
    bool BuildingMapRemote::AdvanceViewportAndPoll(double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits)
    {
        auto result = table_.UpdateViewport(viewport_, xMapUnits, yMapUnits,
            widthMapUnits, heightMapUnits);
        if (result != BUILDING_MAP_ABI_OK)
        {
            availability_ = Availability::Failure;
            if (result == BUILDING_MAP_ABI_INVALID_HANDLE || result == BUILDING_MAP_ABI_NO_SESSION)
            {
                ReleaseValidOwnershipAfterHandleInvalidation();
                AbandonHandles();
                ClearImages();
            }
            return false;
        }

        BuildingMapABIState state{};
        result = QueryProviderState(state);
        if (result != BUILDING_MAP_ABI_OK)
        {
            availability_ = Availability::Failure;
            return false;
        }
        if (state.ServiceAvailable == 0 || state.SessionAvailable == 0)
        {
            availability_ = state.ServiceAvailable == 0
                ? Availability::ServiceUnavailable : Availability::NoGameplaySession;
            AbandonHandles();
            ClearImages();
            return false;
        }
        if (state.CollectorValid == 0 || state.ViewportValid == 0)
        {
            availability_ = state.CollectorValid == 0
                ? Availability::CollectorInvalidated : Availability::ViewportInvalidated;
            ReleaseValidOwnershipAfterHandleInvalidation();
            AbandonHandles();
            ClearImages();
            return false;
        }

        availability_ = Availability::Active;
        try
        {
            return PollVisibleImages(state.VisibleGridWidth, state.VisibleGridHeight);
        }
        catch (...)
        {
            return false;
        }
    }

    /** Advances the same rectangle every active tick, then polls the current visible grid. */
    void BuildingMapRemote::UpdateViewport(double xMapUnits, double yMapUnits,
        double widthMapUnits, double heightMapUnits)
    {
        pollState_ = PollState::Incomplete;
        if (!IsAdmittedGameThread())
        {
            availability_ = Availability::Failure;
            return;
        }
        if (!tableAvailable_ && !DiscoverProvider())
        {
            ClearImages();
            return;
        }
        if (!PrepareViewport(xMapUnits, yMapUnits, widthMapUnits, heightMapUnits))
        {
            return;
        }
        if (!AdvanceViewportAndPoll(xMapUnits, yMapUnits, widthMapUnits, heightMapUnits))
        {
            availability_ = Availability::Failure;
        }
    }

    /** Copies one cell using its current metadata and retries once if its required size changes. */
    BuildingMapABIResult BuildingMapRemote::CopySectionImage(uint64_t localX, uint64_t localY,
        BuildingMapABISectionImage& metadata)
    {
        metadata = {};
        metadata.StructSize = sizeof(metadata);
        auto result = table_.CopySectionImage(viewport_, localX, localY, &metadata, nullptr, 0);
        if (result == BUILDING_MAP_ABI_NO_IMAGE)
        {
            return result;
        }
        if (result != BUILDING_MAP_ABI_BUFFER_TOO_SMALL)
        {
            return result;
        }
        if (!ResizeCopyBuffer(metadata.ByteCount))
        {
            return BUILDING_MAP_ABI_FAILED;
        }

        for (unsigned attempt = 0; attempt < 2; ++attempt)
        {
            metadata = {};
            metadata.StructSize = sizeof(metadata);
            result = table_.CopySectionImage(viewport_, localX, localY, &metadata,
                copyBuffer_.data(), copyBuffer_.size());
            if (result == BUILDING_MAP_ABI_OK && metadata.ByteCount <= copyBuffer_.size())
            {
                copyBuffer_.resize(static_cast<size_t>(metadata.ByteCount));
            }
            if (result != BUILDING_MAP_ABI_BUFFER_TOO_SMALL)
            {
                return result;
            }
            if (attempt != 0)
            {
                return result;
            }
            if (!ResizeCopyBuffer(metadata.ByteCount))
            {
                return BUILDING_MAP_ABI_FAILED;
            }
        }
        return result;
    }

    /** Resizes reusable scratch bytes only when the provider's required count fits local storage. */
    bool BuildingMapRemote::ResizeCopyBuffer(uint64_t requiredBytes) noexcept
    {
        if (requiredBytes == 0 || requiredBytes > (std::numeric_limits<size_t>::max)())
        {
            return false;
        }
        try
        {
            copyBuffer_.resize(static_cast<size_t>(requiredBytes));
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    /** Polls bounded cells and commits availability/removal observations only after full success. */
    bool BuildingMapRemote::PollVisibleImages(uint64_t visibleWidth, uint64_t visibleHeight)
    {
        observedImageScratch_.clear();
        noImageScratch_.clear();
        for (uint64_t localY = 0; localY < visibleHeight; ++localY)
        {
            for (uint64_t localX = 0; localX < visibleWidth; ++localX)
            {
                BuildingMapABISectionImage metadata{};
                const auto result = CopySectionImage(localX, localY, metadata);
                if (result == BUILDING_MAP_ABI_NO_IMAGE)
                {
                    noImageScratch_.emplace_back(metadata.GridX, metadata.GridY);
                    continue;
                }
                if (result != BUILDING_MAP_ABI_OK || !StoreChangedImage(metadata))
                {
                    return false;
                }
                observedImageScratch_.emplace_back(metadata.GridX, metadata.GridY);
            }
        }

        if (!ValidatePollCoordinates()) return false;

        for (auto image = images_.begin(); image != images_.end();)
        {
            if (!std::binary_search(observedImageScratch_.begin(), observedImageScratch_.end(), image->first))
            {
                image = images_.erase(image);
                imagesChanged_ = true;
                if (!AdvanceSourceObservationRevision()) return false;
                continue;
            }
            ++image;
        }
        if (noImageSections_ != noImageScratch_)
        {
            if (!AdvanceSourceObservationRevision()) return false;
            noImageSections_.swap(noImageScratch_);
        }
        pollState_ = PollState::Complete;
        return true;
    }

    /** Sorts both source-state sets and rejects duplicates or contradictory cell outcomes. */
    bool BuildingMapRemote::ValidatePollCoordinates()
    {
        std::sort(observedImageScratch_.begin(), observedImageScratch_.end());
        std::sort(noImageScratch_.begin(), noImageScratch_.end());
        if (std::adjacent_find(observedImageScratch_.begin(), observedImageScratch_.end()) !=
            observedImageScratch_.end()) return false;
        if (std::adjacent_find(noImageScratch_.begin(), noImageScratch_.end()) != noImageScratch_.end())
        {
            return false;
        }
        for (const SectionCoordinate& coordinate : observedImageScratch_)
        {
            if (std::binary_search(noImageScratch_.begin(), noImageScratch_.end(), coordinate)) return false;
        }
        return true;
    }

    /** Reuses prior pixel storage when exact Section metadata and RGBA8 bytes are unchanged. */
    bool BuildingMapRemote::StoreChangedImage(const BuildingMapABISectionImage& metadata)
    {
        if (metadata.Width == 0 || metadata.Height == 0 ||
            metadata.Width > (std::numeric_limits<uint64_t>::max)() / 4 ||
            metadata.RowStride != uint64_t(metadata.Width) * 4 ||
            metadata.RowStride > (std::numeric_limits<uint64_t>::max)() / metadata.Height ||
            metadata.ByteCount != metadata.RowStride * metadata.Height ||
            metadata.ByteCount != copyBuffer_.size())
        {
            return false;
        }

        const auto key = std::make_pair(metadata.GridX, metadata.GridY);
        auto existing = images_.find(key);
        if (existing != images_.end() && existing->second.Width == metadata.Width &&
            existing->second.Height == metadata.Height && existing->second.RowStride == metadata.RowStride &&
            existing->second.ByteCount == metadata.ByteCount &&
            existing->second.XMapUnits == metadata.XMapUnits &&
            existing->second.YMapUnits == metadata.YMapUnits &&
            existing->second.SectionSizeMapUnits == metadata.SectionSizeMapUnits &&
            std::equal(copyBuffer_.begin(), copyBuffer_.end(), existing->second.Pixels.begin(),
                existing->second.Pixels.end()))
        {
            return true;
        }

        if (lastImageRevision_ == (std::numeric_limits<uint64_t>::max)())
        {
            return false;
        }

        SectionImage image{};
        image.GridX = metadata.GridX;
        image.GridY = metadata.GridY;
        image.XMapUnits = metadata.XMapUnits;
        image.YMapUnits = metadata.YMapUnits;
        image.SectionSizeMapUnits = metadata.SectionSizeMapUnits;
        image.Width = metadata.Width;
        image.Height = metadata.Height;
        image.RowStride = metadata.RowStride;
        image.ByteCount = metadata.ByteCount;
        image.Revision = lastImageRevision_ + 1;
        const uint64_t acceptedRevision = image.Revision;
        try
        {
            image.Pixels = copyBuffer_;
            images_.insert_or_assign(key, std::move(image));
        }
        catch (...)
        {
            return false;
        }
        lastImageRevision_ += 1;
        imagesChanged_ = true;
        if (!AdvanceSourceObservationRevision()) return false;
        if (!firstImageAvailabilityLogged_)
        {
            LOG_INFO("MainMap diagnostics: first completed remote image available grid=%lld,%lld revision=%llu",
                static_cast<long long>(metadata.GridX), static_cast<long long>(metadata.GridY),
                static_cast<unsigned long long>(acceptedRevision));
            firstImageAvailabilityLogged_ = true;
        }
        return true;
    }

    /** Discards stale handles and their collector relationship without provider calls. */
    void BuildingMapRemote::AbandonHandles() noexcept
    {
        collector_ = 0;
        viewport_ = 0;
    }

    /** Closes or disposes only the provider handles still reported valid after partial invalidation. */
    void BuildingMapRemote::ReleaseValidOwnershipAfterHandleInvalidation()
    {
        if ((collector_ == 0 && viewport_ == 0) || !IsAdmittedGameThread())
        {
            return;
        }
        BuildingMapABIState state{};
        if (QueryProviderState(state) != BUILDING_MAP_ABI_OK || state.ServiceAvailable == 0 ||
            state.SessionAvailable == 0)
        {
            return;
        }
        if (viewport_ != 0 && state.ViewportValid != 0)
        {
            if (table_.CloseViewport(viewport_) != BUILDING_MAP_ABI_OK)
            {
                availability_ = Availability::Failure;
            }
            return;
        }
        if (collector_ != 0 && state.CollectorValid != 0 && state.ViewportValid == 0)
        {
            if (table_.DisposeCollector(collector_) != BUILDING_MAP_ABI_OK)
            {
                availability_ = Availability::Failure;
            }
        }
    }

    /** Releases the provider viewport or unused collector when state confirms valid ownership. */
    void BuildingMapRemote::CloseViewport()
    {
        if (collector_ == 0 && viewport_ == 0)
        {
            ClearImages();
            return;
        }
        if (tableAvailable_ && IsAdmittedGameThread())
        {
            BuildingMapABIState state{};
            const auto result = QueryProviderState(state);
            if (result != BUILDING_MAP_ABI_OK)
            {
                availability_ = Availability::Failure;
            }
            else if (state.ServiceAvailable == 0)
            {
                availability_ = Availability::ServiceUnavailable;
            }
            else if (state.SessionAvailable == 0)
            {
                availability_ = Availability::NoGameplaySession;
            }
            else if (viewport_ != 0 && state.ViewportValid != 0)
            {
                if (table_.CloseViewport(viewport_) != BUILDING_MAP_ABI_OK)
                {
                    availability_ = Availability::Failure;
                }
            }
            else if (collector_ != 0 && state.CollectorValid != 0 && state.ViewportValid == 0)
            {
                if (table_.DisposeCollector(collector_) != BUILDING_MAP_ABI_OK)
                {
                    availability_ = Availability::Failure;
                }
            }
        }
        AbandonHandles();
        ClearImages();
    }

    /** Stops provider access before plugin shutdown and drops process-local copied images. */
    void BuildingMapRemote::Shutdown()
    {
        CloseViewport();
        table_ = {};
        tableAvailable_ = false;
        availability_ = Availability::ProviderUnavailable;
        copyBuffer_.clear();
    }

    /** Clears images and reports the removal of an existing visible image set. */
    void BuildingMapRemote::ClearImages() noexcept
    {
        if (!images_.empty() || !noImageSections_.empty())
        {
            images_.clear();
            noImageSections_.clear();
            imagesChanged_ = true;
            AdvanceSourceObservationRevision();
        }
        pollState_ = PollState::Unavailable;
    }

    /** Disposes a collector created before a failed viewport open only if it remains valid. */
    void BuildingMapRemote::DisposeUnusedCollector()
    {
        if (collector_ == 0 || !IsAdmittedGameThread())
        {
            return;
        }
        BuildingMapABIState state{};
        if (QueryProviderState(state) == BUILDING_MAP_ABI_OK && state.ServiceAvailable != 0 &&
            state.SessionAvailable != 0 && state.CollectorValid != 0 && state.ViewportValid == 0)
        {
            if (table_.DisposeCollector(collector_) != BUILDING_MAP_ABI_OK)
            {
                availability_ = Availability::Failure;
            }
        }
        collector_ = 0;
    }

    /** Requires both the native operation lease and the existing game-thread admission. */
    bool BuildingMapRemote::IsAdmittedGameThread() const noexcept
    {
        return MainMapNative::CanOperate();
    }

    /** Returns whether the last completed poll changed image content or visible membership. */
    bool BuildingMapRemote::ConsumeImageChanges() noexcept
    {
        const bool changed = imagesChanged_;
        imagesChanged_ = false;
        return changed;
    }

    /** Exposes only MainMap-owned pixel buffers and provider-supplied world metadata. */
    const BuildingMapRemote::ImageCollection& BuildingMapRemote::Images() const noexcept
    {
        return images_;
    }

    /** Returns provider and session availability from the last attempted consumer operation. */
    BuildingMapRemote::Availability BuildingMapRemote::GetAvailability() const noexcept
    {
        return availability_;
    }

    /** Returns whether a full visible-cell snapshot was accepted by the most recent poll. */
    BuildingMapRemote::PollState BuildingMapRemote::GetPollState() const noexcept
    {
        return pollState_;
    }

    /** Exposes the bounded coordinate set that explicitly returned NO_IMAGE. */
    const BuildingMapRemote::NoImageCollection& BuildingMapRemote::NoImageSections() const noexcept
    {
        return noImageSections_;
    }

    /** Exposes a monotonic local token for source additions, revisions, and departures. */
    uint64_t BuildingMapRemote::GetSourceObservationRevision() const noexcept
    {
        return sourceObservationRevision_;
    }

    /** Advances the source token without allowing identity wraparound. */
    bool BuildingMapRemote::AdvanceSourceObservationRevision() noexcept
    {
        if (sourceObservationRevision_ == (std::numeric_limits<uint64_t>::max)()) return false;
        ++sourceObservationRevision_;
        return true;
    }
}
