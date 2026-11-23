/**
 * Applies the optional expanded arrangement to the existing native map menu.
 * The owner captures the five sibling slots once and restores only values it
 * still owns before releasing its admitted UObject leases.
 */
#pragma once

#include "Native/NativeApi.h"
#include "ChimeraUI_classes.hpp"
#include "UMG_classes.hpp"

#include <chrono>

namespace MainMapBuildingLayout
{
    /** Owns one menu's reversible layout mutation during a terrain session. */
    class BuildingMapLayout final
    {
        /** Holds only the slot values this component may change or restore. */
        struct SlotLayout
        {
            SDK::FVector2D position{};
            SDK::FVector2D size{};
            SDK::FVector2D alignment{};
        };

    public:
        /** Indicates whether the caller should defer visible geometry sampling. */
        enum class UpdateResult
        {
            Ready,
            WaitingForSlate
        };

        BuildingMapLayout() = default;
        BuildingMapLayout(const BuildingMapLayout&) = delete;
        BuildingMapLayout& operator=(const BuildingMapLayout&) = delete;

        /** Applies, resizes, or restores the layout for the admitted menu. */
        UpdateResult Update(SDK::UCrUW_MapMenu* menu, bool expanded);

        /** Restores owned values and releases leases; false keeps the owner alive for retry. */
        bool RestoreAndRelease();

    private:
        /** Records one menu-owned widget identity, its leased slot, and owned layout values. */
        struct SlotState
        {
            MainMapNative::StrongObjectLease slot;
            SDK::UWidget* widget = nullptr;
            SlotLayout original{};
            SlotLayout applied{};
            bool positionOwned = false;
            bool sizeOwned = false;
            bool alignmentOwned = false;
        };

        /** Stores validated typed links found during the single discovery pass. */
        struct Widgets
        {
            SDK::UWidgetTree* tree = nullptr;
            SDK::UOverlay* root = nullptr;
            SDK::UCanvasPanel* canvas = nullptr;
            SDK::UOverlaySlot* canvasRootSlot = nullptr;
            SDK::UCanvasPanelSlot* legendSlot = nullptr;
            SDK::UCanvasPanelSlot* zoomSlot = nullptr;
            SDK::UCanvasPanelSlot* frameSlot = nullptr;
            SDK::UCanvasPanelSlot* mapSlot = nullptr;
            SDK::UCanvasPanelSlot* buttonsSlot = nullptr;
        };

        /** Holds the seven rectangles and live dimensions needed for one layout. */
        struct Plan
        {
            SDK::FVector2D available{};
            SDK::FVector2D legendPosition{};
            SDK::FVector2D framePosition{};
            SDK::FVector2D frameSize{};
            SDK::FVector2D mapPosition{};
            SDK::FVector2D mapSize{};
            SDK::FVector2D zoomPosition{};
            SDK::FVector2D buttonsPosition{};
            SDK::FVector2D buttonsSize{};
        };

        /** Finds and validates only the root and five native widgets being changed. */
        bool Discover(SDK::UCrUW_MapMenu* menu, Widgets& widgets);

        /** Rejects invalid or terminating native objects before typed access. */
        static bool IsLiveObject(SDK::UObject* object);

        /** Finds one named direct child and checks its panel slot ownership. */
        bool FindChild(SDK::UPanelWidget* parent, const char* name,
            SDK::UClass* type, SDK::UWidget*& child) const;

        /** Checks liveness and the exact native canvas-slot relationship. */
        bool ValidateCanvasSlot(SDK::UWidget* widget, SDK::UPanelWidget* parent,
            SDK::UCanvasPanelSlot*& slot) const;

        /** Compares finite UI values with the tolerance used for native readback. */
        static bool SamePoint(const SDK::FVector2D& left, const SDK::FVector2D& right);

        /** Captures the original values and retains the menu plus modified slots between ticks. */
        bool Capture(SDK::UCrUW_MapMenu* menu, const Widgets& widgets);

        /** Calculates a responsive frame and control arrangement in live UI units. */
        bool Calculate(const SDK::FVector2D& availableSize, Plan& plan) const;

        /** Measures only the three visible control roots when a layout recalculation is needed. */
        bool MeasureControlSizes(SDK::FVector2D& legendSize, SDK::FVector2D& zoomSize,
            SDK::FVector2D& buttonsSize) const;

        /** Applies the root fill and the five existing sibling slot layouts. */
        bool Apply(const Widgets& widgets, const Plan& plan);

        /** Writes one property group, records ownership, and verifies native readback. */
        bool WritePosition(SDK::UCanvasPanelSlot* slot, SlotState& state,
            const SDK::FVector2D& value);

        /** Writes explicit point-anchored width and height and verifies native readback. */
        bool WriteSize(SDK::UCanvasPanelSlot* slot, SlotState& state,
            const SDK::FVector2D& value);

        /** Writes slot alignment and verifies native readback. */
        bool WriteAlignment(SDK::UCanvasPanelSlot* slot, SlotState& state,
            const SDK::FVector2D& value);

        /** Restores only fields whose current values still equal this owner's last writes. */
        bool Restore();

        /** Restores the owned position, size and alignment fields of one captured slot. */
        bool RestoreSlot(SDK::UCanvasPanelSlot* slot, SlotState& state);

        /** Runs config and geometry decisions after menu discovery or identity recovery. */
        UpdateResult UpdateCaptured(SDK::UCrUW_MapMenu* menu, bool expanded);

        /** Checks bounded Slate settlement and retires a failed application safely. */
        UpdateResult WaitForSlate(SDK::UCrUW_MapMenu* menu, const SDK::FVector2D& liveSize);

        /** Calculates and applies a changed available size, then starts bounded settlement. */
        UpdateResult ApplyForSize(SDK::UCrUW_MapMenu* menu, const SDK::FVector2D& liveSize);

        /** Releases captured leases in dependency order without losing failed releases. */
        bool ReleaseLeases();

        /** Drops value-only capture state after every owned property and lease is resolved. */
        void Clear();

        MainMapNative::StrongObjectLease menu_;
        MainMapNative::StrongObjectLease canvasRootSlot_;
        Widgets capturedWidgets_{};
        SlotState legend_{};
        SlotState zoom_{};
        SlotState frame_{};
        SlotState map_{};
        SlotState buttons_{};
        SDK::EHorizontalAlignment originalHorizontal_ = SDK::EHorizontalAlignment::HAlign_Center;
        SDK::EVerticalAlignment originalVertical_ = SDK::EVerticalAlignment::VAlign_Center;
        SDK::FVector2D lastAvailable_{};
        SDK::FVector2D settlingSize_{};
        SDK::UCrUW_MapMenu* failedMenu_ = nullptr;
        bool captured_ = false;
        bool restored_ = true;
        bool expanded_ = false;
        bool waitingForSlate_ = false;
        std::chrono::steady_clock::time_point settlementDeadline_{};
    };
}
