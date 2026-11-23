// Owns MainMap's temporary higher-order canvas and restores native FoW images before terrain teardown.
#pragma once

#include "Native/NativeApi.h"
#include "SlateCore_structs.hpp"
#include "UMG_classes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace SDK
{
    class UCrUW_MapMenuTerrain;
}

namespace MainMapBuildingMap
{
    /** Separates successful native presentation, deferred Slate layout, and unrecoverable FoW state. */
    enum class FogOfWarMaintenanceResult : std::uint8_t
    {
        Synchronized,
        GeometryPending,
        Failed
    };

    /** Temporarily reparents native FoW images while retaining their objects, resources, and original slot layouts. */
    class FogOfWarPresentation final
    {
    public:
        /** Attaches native segment FoW images after their game-owned construction is complete. */
        static bool AttachShared(SDK::UCrUW_MapMenuTerrain* terrain);
        /** Tracks live segment geometry and visibility while the terrain session remains active. */
        static FogOfWarMaintenanceResult MaintainShared(
            SDK::UCrUW_MapMenuTerrain* terrain, SDK::UCanvasPanel* terrainCanvas);
        /** Restores all native images before the game's terrain destruction begins. */
        static bool RestoreShared(bool widgetDestructionStarted = false) noexcept;

    private:
        /** Retains one moved FoW image and the parent/slot state needed to reconstruct its original layout. */
        struct ImageState final
        {
            MainMapNative::StrongObjectLease segment;
            MainMapNative::StrongObjectLease image;
            MainMapNative::StrongObjectLease originalParent;
            SDK::FAnchorData originalLayout{};
            std::int32_t originalZOrder = 0;
            SDK::ESlateVisibility originalVisibility{};
            bool originalAutoSize = false;
            SDK::ESlateVisibility imposedVisibility{};
            bool hasImposedVisibility = false;
            bool geometryReady = false;
        };

        /** Distinguishes valid segment geometry from a temporarily unavailable Slate layout. */
        enum class GeometryResult : std::uint8_t
        {
            Ready,
            Unavailable,
            Failed
        };

        /** Keeps the presentation owner alive so rejected native releases remain retryable. */
        FogOfWarPresentation() = default;
        /** Prevents duplicating native widget ownership and restoration state. */
        FogOfWarPresentation(const FogOfWarPresentation&) = delete;
        /** Prevents duplicating native widget ownership and restoration state. */
        FogOfWarPresentation& operator=(const FogOfWarPresentation&) = delete;
        /** Returns the process-lifetime owner used by terrain lifecycle callbacks. */
        static FogOfWarPresentation& SharedOwner() noexcept;
        /** Validates the live terrain, creates its FoW canvas, and moves every current native image. */
        bool Attach(SDK::UCrUW_MapMenuTerrain* terrain);
        /** Refreshes all moved images from the terrain's current native segment identities. */
        FogOfWarMaintenanceResult Maintain(SDK::UCrUW_MapMenuTerrain* terrain,
            SDK::UCanvasPanel* terrainCanvas);
        /** Validates and synchronizes one native image, returning deferred geometry without ending the session. */
        FogOfWarMaintenanceResult MaintainImage(SDK::UCrUW_MapMenuTerrain* terrain,
            SDK::UCanvasPanel* terrainCanvas, std::size_t index);
        /** Restores native ownership and releases the temporary presentation container. */
        bool Restore(bool widgetDestructionStarted) noexcept;
        /** Creates the full-size, non-interactive FoW canvas at terrain-canvas Z=2. */
        bool CreateCanvas(SDK::UWidgetTree* tree, SDK::UCanvasPanel* terrainCanvas);
        /** Moves one native image and records its original parent and required canvas slot layout. */
        bool MoveImage(SDK::UCrUW_MapMenuTerrain* terrain, std::size_t index);
        /** Maps the live segment paint bounds into terrain-canvas coordinates and applies them at child Z=0. */
        GeometryResult UpdateImageGeometry(ImageState& state, SDK::UCanvasPanel* terrainCanvas);
        /** Hides a relocated image until valid segment geometry is available. */
        bool HideImageUntilGeometry(ImageState& state);
        /** Applies native local visibility after geometry is valid and mirrors segment visibility. */
        bool UpdateImageVisibility(ImageState& state, bool segmentVisible);
        /** Logs the first actionable failure for this presentation attempt. */
        void ReportFailure(const char* condition, std::int32_t segmentIndex = -1) noexcept;
        /** Restores one image to its native parent and reconstructs its canvas slot state. */
        bool RestoreImage(ImageState& state) noexcept;
        /** Removes the empty presentation canvas and releases every retained native/presentation object. */
        bool RetireCanvas(bool widgetDestructionStarted) noexcept;

        MainMapNative::StrongObjectLease terrain_;
        MainMapNative::StrongObjectLease terrainCanvas_;
        MainMapNative::StrongObjectLease tree_;
        MainMapNative::StrongObjectLease fowCanvas_;
        MainMapNative::StrongObjectLease fowCanvasSlot_;
        std::array<ImageState, 36> images_{};
        std::size_t imageCount_ = 0;
        bool failureLogged_ = false;
        bool restorationFailureLogged_ = false;
    };
}
