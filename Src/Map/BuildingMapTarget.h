// Imports one consumer-owned Section and composes it into a retained linear RGBA16f target.
#pragma once

#include "BuildingMapRemote.h"
#include "Native/NativeApi.h"
#include "Engine_classes.hpp"

#include <cstdint>
#include <cstddef>
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
    };

    /** Owns the transient import and composition objects for one selected image identity. */
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

        /** Updates the one lifecycle-owned target instance. */
        static void UpdateShared(SDK::UObject* worldContext,
            const BuildingMapRemote::ImageCollection& images, bool materialResourcesReady);
        /** Retires resources owned by the shared instance through admitted lease releases. */
        static bool RetireShared() noexcept;
        /** Exposes read-only target status and bounds to the next presentation stage. */
        static const BuildingMapTarget& GetShared() noexcept;

        /** Reports whether the last selected Section has a completed retained target. */
        bool IsReady() const noexcept;
        /** Returns whether the current composition is ready or why no new target is available. */
        Status GetStatus() const noexcept;
        /** Returns the current target identity and its published Section bounds. */
        SectionTargetInfo GetTargetInfo() const noexcept;
        /** Selects the first image and updates resources only when its local revision changes. */
        void Update(SDK::UObject* worldContext, const BuildingMapRemote::ImageCollection& images,
            bool materialResourcesReady);
        /** Retires all transient resources through admitted StrongObjectLease releases. */
        bool Retire() noexcept;

    private:
        /** Returns the one persistent owner used by lifecycle and presentation integration. */
        static BuildingMapTarget& SharedOwner() noexcept;
        /** Groups temporary GPU objects until an imported Section is fully composed. */
        struct ResourceSet final
        {
            MainMapNative::StrongObjectLease InputTexture;
            MainMapNative::StrongObjectLease MaterialInstance;
            MainMapNative::StrongObjectLease RenderTarget;
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
        /** Creates an instance of the mounted composition material and binds SpriteAtlas. */
        bool CreateCompositionMaterial(SDK::UObject* worldContext, SDK::UTexture2D* inputTexture,
            ResourceSet& resources);
        /** Draws the selected image as two white-tinted triangles into a cleared target. */
        bool ComposeSection(SDK::UObject* worldContext, int32_t width, int32_t height,
            ResourceSet& resources);
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
        void BuildSelectedImage(SDK::UObject* worldContext, const SectionImage& image);
        /** Reports whether pending resources match a selected coordinate and revision. */
        bool MatchesSelectedImage(const SectionImage& image) const noexcept;
        /** Retries cleanup of stale or partially-created pending objects. */
        bool ClearStalePendingResources(const SectionImage& image) noexcept;
        /** Promotes a previously composed pending target after old leases release. */
        void PromotePendingResources(const SectionImage& image) noexcept;
        /** Reports whether current retained content already represents this revision. */
        bool IsCurrentImage(const SectionImage& image) const noexcept;
        /** Moves every staged lease into the pending owner without losing failed releases. */
        void PreservePendingResources(ResourceSet& resources) noexcept;
        /** Stores the selected Section identity and bounds after composition succeeds. */
        void SetSelectedImage(const SectionImage& image) noexcept;

        MainMapNative::StrongObjectLease material_;
        ResourceSet current_;
        ResourceSet pending_;
        SectionTargetInfo targetInfo_{};
        SectionTargetInfo pendingInfo_{};
        uint64_t revision_ = 0;
        uint64_t pendingRevision_ = 0;
        bool materialLoadAttempted_ = false;
        bool materialValidated_ = false;
        bool ready_ = false;
        bool pendingReady_ = false;
        Status status_ = Status::NoImage;
    };
}
