/**
 * Implements the reversible, optional layout of the five native map-menu
 * siblings. Discovery and snapshots occur once; later ticks only observe the
 * menu, configuration and live root dimensions until a layout change is needed.
 */
#include "BuildingMapLayout.h"

#include "plugin_helpers.h"

#include <algorithm>
#include <cmath>

namespace MainMapBuildingLayout
{
    namespace
    {
        constexpr float kTolerance = 0.05f;
        constexpr double kOuterMargin = 32.0;
        constexpr double kPanelGap = 24.0;
        constexpr double kLegendWidth = 360.0;
        constexpr double kLegendHeight = 720.0;
        constexpr double kFrameAspect = 1696.3378 / 977.04803;
        constexpr double kFrameMapLeft = 20.0;
        constexpr double kFrameMapTop = 52.0;
        constexpr double kFrameMapRight = 25.2864;
        constexpr double kFrameMapBottom = 24.766;
        constexpr std::chrono::milliseconds kSlateSettlement{500};
    }

    /** Rejects invalid and terminating UObject outer chains before typed access. */
    bool BuildingMapLayout::IsLiveObject(SDK::UObject* object)
    {
        if (!object)
            return false;

        const auto forbidden = SDK::EObjectFlags::ClassDefaultObject | SDK::EObjectFlags::ArchetypeObject |
            SDK::EObjectFlags::InheritableComponentTemplate | SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed;
        for (int depth = 0; object && depth < 32; ++depth, object = object->Outer)
        {
            if ((static_cast<unsigned>(object->Flags) & static_cast<unsigned>(forbidden)) != 0)
                return false;
        }

        return object == nullptr;
    }

    /** Names a missing object, terminating outer, or invalid outer chain for failure logs. */
    const char* BuildingMapLayout::GetObjectFailure(SDK::UObject* object)
    {
        if (!object)
            return "unavailable";

        const auto forbidden = SDK::EObjectFlags::ClassDefaultObject | SDK::EObjectFlags::ArchetypeObject |
            SDK::EObjectFlags::InheritableComponentTemplate | SDK::EObjectFlags::BeginDestroyed |
            SDK::EObjectFlags::FinishDestroyed;
        for (int depth = 0; object && depth < 32; ++depth, object = object->Outer)
        {
            if ((static_cast<unsigned>(object->Flags) & static_cast<unsigned>(forbidden)) != 0)
                return "terminating-or-non-instance";
        }

        return object == nullptr ? "available" : "invalid-outer-chain";
    }

    /** Compares finite points using a small tolerance for Slate float readback. */
    bool BuildingMapLayout::SamePoint(const SDK::FVector2D& left, const SDK::FVector2D& right)
    {
        return std::isfinite(left.X) && std::isfinite(left.Y) &&
            std::abs(left.X - right.X) <= kTolerance &&
            std::abs(left.Y - right.Y) <= kTolerance;
    }

    /** Checks a direct panel child and its exact canvas slot before caching it. */
    bool BuildingMapLayout::FindChild(SDK::UPanelWidget* parent, const char* name,
        SDK::UClass* type, SDK::UWidget*& child) const
    {
        child = nullptr;
        if (!IsLiveObject(parent) || !name || !type)
            return false;

        const int32_t count = parent->GetChildrenCount();
        if (count < 0 || count > 16)
            return false;

        for (int32_t index = 0; index < count; ++index)
        {
            auto* candidate = parent->GetChildAt(index);
            if (!IsLiveObject(candidate) || candidate->GetParent() != parent)
                return false;

            auto* slot = candidate->Slot;
            if (!IsLiveObject(slot) || slot->Content != candidate || slot->Parent != parent)
                return false;

            if (candidate->GetName() != name)
                continue;

            if (child || !candidate->IsA(type))
                return false;

            child = candidate;
        }

        return child != nullptr;
    }

    /** Validates a widget's current canvas slot without changing native state. */
    bool BuildingMapLayout::ValidateCanvasSlot(SDK::UWidget* widget,
        SDK::UPanelWidget* parent, SDK::UCanvasPanelSlot*& slot) const
    {
        slot = nullptr;
        if (!IsLiveObject(widget) || !IsLiveObject(parent) || widget->GetParent() != parent)
            return false;

        auto* candidate = widget->Slot;
        if (!IsLiveObject(candidate) || !candidate->IsA(SDK::UCanvasPanelSlot::StaticClass()))
            return false;

        auto* canvasSlot = static_cast<SDK::UCanvasPanelSlot*>(candidate);
        if (canvasSlot->Parent != parent || canvasSlot->Content != widget)
            return false;

        slot = canvasSlot;
        return true;
    }

    /** Discovers the named native siblings and validates only their required relationships. */
    bool BuildingMapLayout::Discover(SDK::UCrUW_MapMenu* menu, Widgets& widgets)
    {
        if (!IsLiveObject(menu) || !menu->IsA(SDK::UCrUW_MapMenu::StaticClass()))
            return false;

        widgets.tree = menu->WidgetTree;
        if (!IsLiveObject(widgets.tree) || widgets.tree->Outer != menu)
            return false;

        auto* rootWidget = widgets.tree->RootWidget;
        if (!IsLiveObject(rootWidget) || !rootWidget->IsA(SDK::UOverlay::StaticClass()) ||
            rootWidget->Outer != widgets.tree || rootWidget->GetParent())
            return false;

        widgets.root = static_cast<SDK::UOverlay*>(rootWidget);
        SDK::UWidget* canvasWidget = nullptr;
        if (!FindChild(widgets.root, "CanvasPanel_32", SDK::UCanvasPanel::StaticClass(), canvasWidget))
            return false;

        widgets.canvas = static_cast<SDK::UCanvasPanel*>(canvasWidget);
        auto* rootSlot = widgets.canvas->Slot;
        if (widgets.canvas->Outer != widgets.tree || !IsLiveObject(rootSlot) ||
            !rootSlot->IsA(SDK::UOverlaySlot::StaticClass()))
            return false;
        widgets.canvasRootSlot = static_cast<SDK::UOverlaySlot*>(rootSlot);
        if (widgets.canvasRootSlot->Parent != widgets.root ||
            widgets.canvasRootSlot->Content != widgets.canvas)
            return false;

        SDK::UWidget* legendWidget = nullptr;
        SDK::UWidget* zoomWidget = nullptr;
        SDK::UWidget* frameWidget = nullptr;
        SDK::UWidget* mapWidget = nullptr;
        SDK::UWidget* buttonsWidget = nullptr;
        if (!FindChild(widgets.canvas, "MapMenuLegend", SDK::UCrUW_MapMenuLegend::StaticClass(), legendWidget) ||
            !FindChild(widgets.canvas, "MapMenuZoomSlider", SDK::UCrUW_MapMenuZoomSlider::StaticClass(), zoomWidget) ||
            !FindChild(widgets.canvas, "MapAreaFrame", SDK::UOverlay::StaticClass(), frameWidget) ||
            !FindChild(widgets.canvas, "MapMenuMapArea", SDK::UCrUW_MapMenuMapArea::StaticClass(), mapWidget) ||
            !FindChild(widgets.canvas, "CP_Buttons", SDK::UCanvasPanel::StaticClass(), buttonsWidget))
            return false;

        if (legendWidget != menu->MapMenuLegend || zoomWidget != menu->MapMenuZoomSlider ||
            mapWidget != menu->MapMenuMapArea)
            return false;

        return ValidateCanvasSlot(static_cast<SDK::UWidget*>(legendWidget), widgets.canvas, widgets.legendSlot) &&
            ValidateCanvasSlot(static_cast<SDK::UWidget*>(zoomWidget), widgets.canvas, widgets.zoomSlot) &&
            ValidateCanvasSlot(frameWidget, widgets.canvas, widgets.frameSlot) &&
            ValidateCanvasSlot(mapWidget, widgets.canvas, widgets.mapSlot) &&
            ValidateCanvasSlot(buttonsWidget, widgets.canvas, widgets.buttonsSlot);
    }

    /** Captures original properties and retains the five widgets, five slots and root slot. */
    bool BuildingMapLayout::Capture(SDK::UCrUW_MapMenu* menu, const Widgets& widgets)
    {
        if (!menu_.Acquire(menu) || !canvasRootSlot_.Acquire(widgets.canvasRootSlot))
            return false;

        originalHorizontal_ = widgets.canvasRootSlot->HorizontalAlignment;
        originalVertical_ = widgets.canvasRootSlot->VerticalAlignment;
        if (originalHorizontal_ != SDK::EHorizontalAlignment::HAlign_Center ||
            originalVertical_ != SDK::EVerticalAlignment::VAlign_Center)
            return false;

        SlotState* states[] = {&legend_, &zoom_, &frame_, &map_, &buttons_};
        SDK::UWidget* widgetList[] = {
            menu->MapMenuLegend, menu->MapMenuZoomSlider, widgets.frameSlot->Content,
            menu->MapMenuMapArea, widgets.buttonsSlot->Content};
        SDK::UCanvasPanelSlot* slotList[] = {
            widgets.legendSlot, widgets.zoomSlot, widgets.frameSlot, widgets.mapSlot, widgets.buttonsSlot};
        const SDK::FVector2D expectedAlignments[] = {
            {1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};

        for (int index = 0; index < 5; ++index)
        {
            const auto anchors = slotList[index]->GetAnchors();
            const auto alignment = slotList[index]->GetAlignment();
            const auto size = slotList[index]->GetSize();
            const auto layout = slotList[index]->GetLayout();
            if (!SamePoint(anchors.Minimum, {0.0f, 0.0f}) ||
                !SamePoint(anchors.Maximum, {0.0f, 0.0f}) ||
                !SamePoint(alignment, expectedAlignments[index]) || slotList[index]->GetAutoSize() ||
                !std::isfinite(size.X) || !std::isfinite(size.Y) || size.X <= 0 || size.Y <= 0 ||
                !std::isfinite(layout.Offsets.Left) || !std::isfinite(layout.Offsets.Top) ||
                !std::isfinite(layout.Offsets.Right) || !std::isfinite(layout.Offsets.Bottom))
                return false;

            states[index]->name = index == 0 ? "legend" : index == 1 ? "zoom" :
                index == 2 ? "frame" : index == 3 ? "map" : "buttons";
            if (!states[index]->slot.Acquire(slotList[index]))
                return false;

            states[index]->widget = widgetList[index];
            states[index]->original.position = {layout.Offsets.Left, layout.Offsets.Top};
            states[index]->original.size = {layout.Offsets.Right, layout.Offsets.Bottom};
            states[index]->original.alignment = layout.Alignment;
            states[index]->applied = states[index]->original;
        }

        capturedWidgets_ = widgets;
        captured_ = true;
        restored_ = true;
        return true;
    }

    /** Measures native outer control roots only when expansion or available size changes. */
    bool BuildingMapLayout::MeasureControlSizes(SDK::FVector2D& legendSize,
        SDK::FVector2D& zoomSize, SDK::FVector2D& buttonsSize) const
    {
        const auto legendDesired = legend_.widget->GetDesiredSize();
        const auto zoomDesired = zoom_.widget->GetDesiredSize();
        const auto buttonsDesired = buttons_.widget->GetDesiredSize();
        const auto legendOriginal = legend_.original.size;
        const auto zoomOriginal = zoom_.original.size;
        const auto buttonsOriginal = buttons_.original.size;
        if (!std::isfinite(legendDesired.X) || !std::isfinite(legendDesired.Y) ||
            !std::isfinite(zoomDesired.X) || !std::isfinite(zoomDesired.Y) ||
            !std::isfinite(buttonsDesired.X) || !std::isfinite(buttonsDesired.Y))
            return false;

        legendSize = {static_cast<float>((std::max)({kLegendWidth, static_cast<double>(legendOriginal.X),
                static_cast<double>(legendDesired.X)})),
            static_cast<float>((std::max)({kLegendHeight, static_cast<double>(legendOriginal.Y),
                static_cast<double>(legendDesired.Y)}))};
        zoomSize = {static_cast<float>((std::max)(static_cast<double>(zoomOriginal.X),
                static_cast<double>(zoomDesired.X))),
            static_cast<float>((std::max)(static_cast<double>(zoomOriginal.Y),
                static_cast<double>(zoomDesired.Y)))};
        buttonsSize = {static_cast<float>((std::max)(static_cast<double>(buttonsOriginal.X),
                static_cast<double>(buttonsDesired.X))),
            static_cast<float>((std::max)(static_cast<double>(buttonsOriginal.Y),
                static_cast<double>(buttonsDesired.Y)))};
        return zoomSize.X > 0 && zoomSize.Y > 0 && buttonsSize.X > 0 && buttonsSize.Y > 0;
    }

    /** Computes the proposed margins and frame-fit layout from live root geometry. */
    bool BuildingMapLayout::Calculate(const SDK::FVector2D& rootSize, Plan& plan) const
    {
        if (!std::isfinite(rootSize.X) || !std::isfinite(rootSize.Y) || rootSize.X <= 0 || rootSize.Y <= 0)
            return false;

        SDK::FVector2D legendSize{};
        SDK::FVector2D zoomSize{};
        SDK::FVector2D buttonsSize{};
        if (!MeasureControlSizes(legendSize, zoomSize, buttonsSize))
            return false;

        plan.available = rootSize;
        const double frameLeft = kOuterMargin + legendSize.X + kPanelGap;
        const double frameRight = rootSize.X - kOuterMargin - zoomSize.X - kPanelGap;
        const double frameBottom = rootSize.Y - kOuterMargin - buttonsSize.Y - kPanelGap;
        const double availableWidth = frameRight - frameLeft;
        const double availableHeight = frameBottom - kOuterMargin;
        if (!std::isfinite(availableWidth) || !std::isfinite(availableHeight) ||
            availableWidth <= kFrameMapLeft + kFrameMapRight ||
            availableHeight <= kFrameMapTop + kFrameMapBottom ||
            legendSize.Y + 2.0 * kOuterMargin > rootSize.Y)
            return false;

        const double scale = (std::min)(availableWidth / 1696.3378, availableHeight / 977.04803);
        const double frameWidth = 1696.3378 * scale;
        const double frameHeight = 977.04803 * scale;
        if (!std::isfinite(scale) || scale <= 0 || std::abs(frameWidth / frameHeight - kFrameAspect) > 0.001)
            return false;

        plan.legendPosition = {static_cast<float>(kOuterMargin), static_cast<float>(kOuterMargin)};
        plan.framePosition = {static_cast<float>(frameLeft + (availableWidth - frameWidth) * 0.5),
            static_cast<float>(kOuterMargin)};
        plan.frameSize = {static_cast<float>(frameWidth), static_cast<float>(frameHeight)};
        plan.mapPosition = {plan.framePosition.X + static_cast<float>(kFrameMapLeft),
            plan.framePosition.Y + static_cast<float>(kFrameMapTop)};
        plan.mapSize = {plan.frameSize.X - static_cast<float>(kFrameMapLeft + kFrameMapRight),
            plan.frameSize.Y - static_cast<float>(kFrameMapTop + kFrameMapBottom)};
        plan.zoomPosition = {plan.framePosition.X + plan.frameSize.X + static_cast<float>(kPanelGap),
            plan.framePosition.Y + static_cast<float>(kFrameMapTop)};
        plan.buttonsSize = buttonsSize;
        plan.buttonsPosition = {rootSize.X - static_cast<float>(kOuterMargin) - plan.buttonsSize.X,
            rootSize.Y - static_cast<float>(kOuterMargin) - plan.buttonsSize.Y};
        return std::isfinite(plan.mapSize.X) && std::isfinite(plan.mapSize.Y) &&
            plan.mapSize.X > 0 && plan.mapSize.Y > 0;
    }

    /** Sets a slot position once and verifies both values before claiming ownership. */
    bool BuildingMapLayout::WritePosition(SDK::UCanvasPanelSlot* slot, SlotState& state,
        const SDK::FVector2D& value)
    {
        if (SamePoint(state.applied.position, value))
        {
            state.positionOwned = true;
            return true;
        }

        state.applied.position = value;
        state.positionOwned = true;
        slot->SetPosition(value);
        const auto layout = slot->GetLayout();
        return SamePoint({layout.Offsets.Left, layout.Offsets.Top}, value);
    }

    /** Sets an explicit slot size and verifies its width and height readback. */
    bool BuildingMapLayout::WriteSize(SDK::UCanvasPanelSlot* slot, SlotState& state,
        const SDK::FVector2D& value)
    {
        if (SamePoint(state.applied.size, value))
        {
            state.sizeOwned = true;
            return true;
        }

        state.applied.size = value;
        state.sizeOwned = true;
        slot->SetSize(value);
        const auto layout = slot->GetLayout();
        return SamePoint({layout.Offsets.Right, layout.Offsets.Bottom}, value);
    }

    /** Sets a slot alignment and verifies the native readback. */
    bool BuildingMapLayout::WriteAlignment(SDK::UCanvasPanelSlot* slot, SlotState& state,
        const SDK::FVector2D& value)
    {
        if (!SamePoint(slot->GetAlignment(), value))
        {
            state.applied.alignment = value;
            state.alignmentOwned = true;
            slot->SetAlignment(value);
        }
        else
        {
            state.applied.alignment = value;
            state.alignmentOwned = true;
        }

        return SamePoint(slot->GetAlignment(), value);
    }

    /** Applies the root fill and the five planned point-slot changes in a fixed order. */
    bool BuildingMapLayout::Apply(const Widgets& widgets, const Plan& plan)
    {
        if (widgets.canvasRootSlot->HorizontalAlignment != originalHorizontal_ &&
            widgets.canvasRootSlot->HorizontalAlignment != SDK::EHorizontalAlignment::HAlign_Fill)
            return false;
        if (widgets.canvasRootSlot->VerticalAlignment != originalVertical_ &&
            widgets.canvasRootSlot->VerticalAlignment != SDK::EVerticalAlignment::VAlign_Fill)
            return false;

        SlotState* states[] = {&legend_, &frame_, &map_, &zoom_, &buttons_};
        SDK::UCanvasPanelSlot* slots[] = {
            widgets.legendSlot, widgets.frameSlot, widgets.mapSlot, widgets.zoomSlot, widgets.buttonsSlot};
        for (int index = 0; index < 5; ++index)
        {
            const auto layout = slots[index]->GetLayout();
            const auto& expected = expanded_ ? states[index]->applied : states[index]->original;
            if (!SamePoint({layout.Offsets.Left, layout.Offsets.Top}, expected.position) ||
                ((index == 1 || index == 2 || index == 4) &&
                    !SamePoint({layout.Offsets.Right, layout.Offsets.Bottom}, expected.size)))
                return false;
        }
        if (!SamePoint(widgets.legendSlot->GetAlignment(),
                expanded_ ? legend_.applied.alignment : legend_.original.alignment))
            return false;

        if (widgets.canvasRootSlot->HorizontalAlignment != SDK::EHorizontalAlignment::HAlign_Fill)
            widgets.canvasRootSlot->SetHorizontalAlignment(SDK::EHorizontalAlignment::HAlign_Fill);
        if (widgets.canvasRootSlot->VerticalAlignment != SDK::EVerticalAlignment::VAlign_Fill)
            widgets.canvasRootSlot->SetVerticalAlignment(SDK::EVerticalAlignment::VAlign_Fill);
        if (widgets.canvasRootSlot->HorizontalAlignment != SDK::EHorizontalAlignment::HAlign_Fill ||
            widgets.canvasRootSlot->VerticalAlignment != SDK::EVerticalAlignment::VAlign_Fill)
            return false;

        return WritePosition(widgets.legendSlot, legend_, plan.legendPosition) &&
            WriteAlignment(widgets.legendSlot, legend_, {0.0f, 0.0f}) &&
            WritePosition(widgets.frameSlot, frame_, plan.framePosition) &&
            WriteSize(widgets.frameSlot, frame_, plan.frameSize) &&
            WritePosition(widgets.mapSlot, map_, plan.mapPosition) &&
            WriteSize(widgets.mapSlot, map_, plan.mapSize) &&
            WritePosition(widgets.zoomSlot, zoom_, plan.zoomPosition) &&
            WritePosition(widgets.buttonsSlot, buttons_, plan.buttonsPosition) &&
            WriteSize(widgets.buttonsSlot, buttons_, plan.buttonsSize);
    }

    /** Restores the owned fields of one slot without replacing unrelated properties. */
    bool BuildingMapLayout::RestoreSlot(SDK::UCanvasPanelSlot* slot, SlotState& state)
    {
        if (!IsLiveObject(slot) || !IsLiveObject(state.widget) || slot->Content != state.widget)
        {
            if (reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure slot=%s validation=slot-widget-relationship slotState=%s widgetState=%s slot=%p widget=%p",
                    state.name ? state.name : "unknown", GetObjectFailure(slot),
                    GetObjectFailure(state.widget), slot, state.widget);
            return false;
        }
        bool restored = RestorePosition(slot, state);
        restored = RestoreSize(slot, state) && restored;
        restored = RestoreAlignment(slot, state) && restored;
        return restored;
    }

    /** Restores one owned position while preserving intervening external writes. */
    bool BuildingMapLayout::RestorePosition(SDK::UCanvasPanelSlot* slot, SlotState& state)
    {
        if (!state.positionOwned)
            return true;

        auto layout = slot->GetLayout();
        const SDK::FVector2D current{layout.Offsets.Left, layout.Offsets.Top};
        if (SamePoint(current, state.applied.position))
        {
            slot->SetPosition(state.original.position);
            layout = slot->GetLayout();
        }
        const SDK::FVector2D readback{layout.Offsets.Left, layout.Offsets.Top};
        if (!SamePoint(readback, state.original.position))
        {
            if (reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure slot=%s property=position objectState=available validation=%s current=(%.3f,%.3f) original=(%.3f,%.3f) lastApplied=(%.3f,%.3f)",
                    state.name, SamePoint(current, state.applied.position) ? "setter-readback-differs-from-original" : "current-conflicts-with-last-write",
                    readback.X, readback.Y, state.original.position.X, state.original.position.Y,
                    state.applied.position.X, state.applied.position.Y);
            return false;
        }
        state.positionOwned = false;
        return true;
    }

    /** Restores one owned size while preserving intervening external writes. */
    bool BuildingMapLayout::RestoreSize(SDK::UCanvasPanelSlot* slot, SlotState& state)
    {
        if (!state.sizeOwned)
            return true;

        auto layout = slot->GetLayout();
        const SDK::FVector2D current{layout.Offsets.Right, layout.Offsets.Bottom};
        if (SamePoint(current, state.applied.size))
        {
            slot->SetSize(state.original.size);
            layout = slot->GetLayout();
        }
        const SDK::FVector2D readback{layout.Offsets.Right, layout.Offsets.Bottom};
        if (!SamePoint(readback, state.original.size))
        {
            if (reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure slot=%s property=size objectState=available validation=%s current=(%.3f,%.3f) original=(%.3f,%.3f) lastApplied=(%.3f,%.3f)",
                    state.name, SamePoint(current, state.applied.size) ? "setter-readback-differs-from-original" : "current-conflicts-with-last-write",
                    readback.X, readback.Y, state.original.size.X, state.original.size.Y,
                    state.applied.size.X, state.applied.size.Y);
            return false;
        }
        state.sizeOwned = false;
        return true;
    }

    /** Restores one owned alignment while preserving intervening external writes. */
    bool BuildingMapLayout::RestoreAlignment(SDK::UCanvasPanelSlot* slot, SlotState& state)
    {
        if (!state.alignmentOwned)
            return true;

        const auto current = slot->GetAlignment();
        if (SamePoint(current, state.applied.alignment))
            slot->SetAlignment(state.original.alignment);
        const auto readback = slot->GetAlignment();
        if (!SamePoint(readback, state.original.alignment))
        {
            if (reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure slot=%s property=alignment objectState=available validation=%s current=(%.3f,%.3f) original=(%.3f,%.3f) lastApplied=(%.3f,%.3f)",
                    state.name, SamePoint(current, state.applied.alignment) ? "setter-readback-differs-from-original" : "current-conflicts-with-last-write",
                    readback.X, readback.Y, state.original.alignment.X, state.original.alignment.Y,
                    state.applied.alignment.X, state.applied.alignment.Y);
            return false;
        }
        state.alignmentOwned = false;
        return true;
    }

    /** Restores each slot and the root allocation, preserving any external changes. */
    bool BuildingMapLayout::Restore()
    {
        reportRestoreDiagnostics_ = !restoreDiagnosticsReported_;
        if (!captured_)
            return !captured_;
        if (!MainMapNative::CanOperate())
            return false;
        restoreDiagnosticsReported_ = true;

        bool restored = true;
        SlotState* states[] = {&legend_, &zoom_, &frame_, &map_, &buttons_};
        for (auto* state : states)
        {
            auto* slot = static_cast<SDK::UCanvasPanelSlot*>(state->slot.Get());
            if (!RestoreSlot(slot, *state))
                restored = false;
        }

        if (!RestoreRootAlignment())
            restored = false;

        restored_ = restored;
        return restored;
    }

    /** Restores root alignments and reports conflicts or setter readback failures. */
    bool BuildingMapLayout::RestoreRootAlignment()
    {
        auto* rootSlot = static_cast<SDK::UOverlaySlot*>(canvasRootSlot_.Get());
        if (!IsLiveObject(rootSlot))
        {
            if (reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure root property=alignment validation=root-slot-unavailable objectState=%s slot=%p",
                    GetObjectFailure(canvasRootSlot_.Get()), canvasRootSlot_.Get());
            return false;
        }
        if (rootSlot->HorizontalAlignment == SDK::EHorizontalAlignment::HAlign_Fill)
        {
            rootSlot->SetHorizontalAlignment(originalHorizontal_);
            if (rootSlot->HorizontalAlignment != originalHorizontal_ && reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure root property=horizontal-alignment objectState=available validation=setter-readback-differs-from-original current=%u original=%u lastApplied=%u",
                    static_cast<unsigned>(rootSlot->HorizontalAlignment), static_cast<unsigned>(originalHorizontal_),
                    static_cast<unsigned>(SDK::EHorizontalAlignment::HAlign_Fill));
        }
        if (rootSlot->VerticalAlignment == SDK::EVerticalAlignment::VAlign_Fill)
        {
            rootSlot->SetVerticalAlignment(originalVertical_);
            if (rootSlot->VerticalAlignment != originalVertical_ && reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure root property=vertical-alignment objectState=available validation=setter-readback-differs-from-original current=%u original=%u lastApplied=%u",
                    static_cast<unsigned>(rootSlot->VerticalAlignment), static_cast<unsigned>(originalVertical_),
                    static_cast<unsigned>(SDK::EVerticalAlignment::VAlign_Fill));
        }
        const bool restored = rootSlot->HorizontalAlignment == originalHorizontal_ &&
            rootSlot->VerticalAlignment == originalVertical_;
        if (!restored)
        {
            if (reportRestoreDiagnostics_)
                LOG_WARN("MainMap ExpandedMap restore failure root property=alignment objectState=available validation=current-conflicts-with-last-write horizontal=%u originalHorizontal=%u lastHorizontal=%u vertical=%u originalVertical=%u lastVertical=%u",
                    static_cast<unsigned>(rootSlot->HorizontalAlignment), static_cast<unsigned>(originalHorizontal_),
                    static_cast<unsigned>(SDK::EHorizontalAlignment::HAlign_Fill),
                    static_cast<unsigned>(rootSlot->VerticalAlignment), static_cast<unsigned>(originalVertical_),
                    static_cast<unsigned>(SDK::EVerticalAlignment::VAlign_Fill));
        }

        return restored;
    }

    /** Releases retained objects in slot-before-widget order and preserves rejected leases. */
    bool BuildingMapLayout::ReleaseLeases()
    {
        reportLeaseDiagnostics_ = !leaseDiagnosticsReported_;
        leaseDiagnosticsReported_ = true;
        bool released = true;
        SlotState* states[] = {&legend_, &zoom_, &frame_, &map_, &buttons_};
        for (auto* state : states)
        {
            if (state->slot.Reset())
                state->widget = nullptr;
            else
            {
                released = false;
                if (reportLeaseDiagnostics_)
                    LOG_WARN("MainMap ExpandedMap lease release failure slot=%s layoutRestoration=%s object=%p nativeAdmission=%s release=retained-for-retry",
                        state->name ? state->name : "unknown", restored_ ? "complete-or-not-required" : "incomplete",
                        state->slot.Get(),
                        MainMapNative::CanOperate() ? "available" : "unavailable");
            }
        }
        if (!canvasRootSlot_.Reset())
        {
            released = false;
            if (reportLeaseDiagnostics_)
                LOG_WARN("MainMap ExpandedMap lease release failure slot=root layoutRestoration=%s object=%p nativeAdmission=%s release=retained-for-retry",
                    restored_ ? "complete-or-not-required" : "incomplete", canvasRootSlot_.Get(),
                    MainMapNative::CanOperate() ? "available" : "unavailable");
        }
        if (released && !menu_.Reset())
        {
            released = false;
            if (reportLeaseDiagnostics_)
                LOG_WARN("MainMap ExpandedMap lease release failure slot=menu layoutRestoration=%s object=%p nativeAdmission=%s release=retained-for-retry",
                    restored_ ? "complete-or-not-required" : "incomplete", menu_.Get(),
                    MainMapNative::CanOperate() ? "available" : "unavailable");
        }
        return released;
    }

    /** Clears cached identities only after verified restoration and successful lease release. */
    void BuildingMapLayout::Clear()
    {
        capturedWidgets_ = {};
        lastAvailable_ = {};
        settlingSize_ = {};
        captured_ = false;
        restored_ = true;
        expanded_ = false;
        waitingForSlate_ = false;
        reportRestoreDiagnostics_ = false;
        restoreDiagnosticsReported_ = false;
        admissionDiagnosticsReported_ = false;
        reportLeaseDiagnostics_ = false;
        leaseDiagnosticsReported_ = false;
        failedMenu_ = nullptr;
    }

    /** Restores the original menu layout and releases every retained native object. */
    bool BuildingMapLayout::RestoreAndRelease()
    {
        if (!captured_)
        {
            if (!ReleaseLeases())
                return false;
            Clear();
            return true;
        }
        if ((!restored_ && !Restore()) || !ReleaseLeases())
        {
            if (!admissionDiagnosticsReported_ && !MainMapNative::CanOperate())
            {
                LOG_WARN("MainMap ExpandedMap restore failure native-operation=unavailable validation=admission-rejected; captured layout owner retained");
                admissionDiagnosticsReported_ = true;
            }
            return false;
        }
        Clear();
        return true;
    }

    /** Checks the expected resized map geometry and completes or rolls back Slate settlement. */
    BuildingMapLayout::UpdateResult BuildingMapLayout::WaitForSlate(
        SDK::UCrUW_MapMenu* menu, const SDK::FVector2D& liveSize)
    {
        if (!SamePoint(liveSize, settlingSize_))
        {
            expanded_ = true;
            return ApplyForSize(menu, liveSize);
        }

        const auto mapSize = SDK::USlateBlueprintLibrary::GetLocalSize(
            static_cast<SDK::UWidget*>(capturedWidgets_.mapSlot->Content)->GetCachedGeometry());
        const SDK::FVector2D expectedMapSize = map_.applied.size;
        if (SamePoint(liveSize, settlingSize_) && SamePoint(mapSize, expectedMapSize))
        {
            waitingForSlate_ = false;
            lastAvailable_ = liveSize;
            expanded_ = true;
            return UpdateResult::Ready;
        }
        if (std::chrono::steady_clock::now() < settlementDeadline_)
            return UpdateResult::WaitingForSlate;

        failedMenu_ = menu;
        const bool restored = Restore();
        expanded_ = false;
        waitingForSlate_ = false;
        if (!restored)
            LOG_WARN("MainMap: ExpandedMap Slate settlement failed; rollback is incomplete for this menu");
        return UpdateResult::Ready;
    }

    /** Applies a changed dimension plan and starts the bounded Slate settle window. */
    BuildingMapLayout::UpdateResult BuildingMapLayout::ApplyForSize(
        SDK::UCrUW_MapMenu* menu, const SDK::FVector2D& liveSize)
    {
        if (!expanded_ &&
            (capturedWidgets_.canvasRootSlot->HorizontalAlignment != originalHorizontal_ ||
                capturedWidgets_.canvasRootSlot->VerticalAlignment != originalVertical_))
        {
            failedMenu_ = menu;
            return UpdateResult::Ready;
        }

        Plan plan{};
        if (!Calculate(liveSize, plan))
        {
            failedMenu_ = menu;
            if (!Restore())
                LOG_WARN("MainMap: ExpandedMap application failed; rollback is incomplete for this menu");
            expanded_ = false;
            waitingForSlate_ = false;
            return UpdateResult::Ready;
        }

        const bool settlementAlreadyPending = waitingForSlate_;
        restored_ = false;
        if (!Apply(capturedWidgets_, plan))
        {
            failedMenu_ = menu;
            if (!Restore())
                LOG_WARN("MainMap: ExpandedMap application failed; rollback is incomplete for this menu");
            expanded_ = false;
            waitingForSlate_ = false;
            return UpdateResult::Ready;
        }

        settlingSize_ = plan.available;
        if (!settlementAlreadyPending)
            settlementDeadline_ = std::chrono::steady_clock::now() + kSlateSettlement;
        waitingForSlate_ = true;
        return UpdateResult::WaitingForSlate;
    }

    /** Runs configuration, settlement and responsive-size decisions on the captured menu. */
    BuildingMapLayout::UpdateResult BuildingMapLayout::UpdateCaptured(
        SDK::UCrUW_MapMenu* menu, bool expanded, const SDK::FVector2D& liveSize)
    {
        if (!expanded)
        {
            const bool hadPendingLayout = waitingForSlate_;
            waitingForSlate_ = false;
            if ((expanded_ || hadPendingLayout) && !Restore())
            {
                failedMenu_ = menu;
                LOG_WARN("MainMap: ExpandedMap disable could not verify complete native layout restoration");
                return UpdateResult::Ready;
            }
            expanded_ = false;
            lastAvailable_ = {};
            return UpdateResult::Ready;
        }

        if (!std::isfinite(liveSize.X) || !std::isfinite(liveSize.Y) || liveSize.X <= 0 || liveSize.Y <= 0)
            return UpdateResult::Ready;
        if (waitingForSlate_)
            return WaitForSlate(menu, liveSize);
        if (expanded_ && SamePoint(liveSize, lastAvailable_))
            return UpdateResult::Ready;
        return ApplyForSize(menu, liveSize);
    }

    /** Discovers once, handles menu replacement, and forwards current config to the captured layout. */
    BuildingMapLayout::UpdateResult BuildingMapLayout::Update(SDK::UCrUW_MapMenu* menu,
        bool expanded, const SDK::FVector2D& allottedSize)
    {
        if (!MainMapNative::CanOperate() || !menu)
            return UpdateResult::Ready;

        if (captured_ && expanded && expanded_ && !waitingForSlate_ &&
            SamePoint(allottedSize, lastAvailable_))
            return UpdateResult::Ready;

        if (!expanded && !captured_)
        {
            const bool hasOutstandingLeases = menu_ || canvasRootSlot_ || legend_.slot || zoom_.slot ||
                frame_.slot || map_.slot || buttons_.slot;
            if (hasOutstandingLeases)
                ReleaseLeases();
            return UpdateResult::Ready;
        }

        if (expanded && failedMenu_ == menu)
            return UpdateResult::Ready;

        if (captured_ && (capturedWidgets_.tree->Outer != menu || menu->WidgetTree != capturedWidgets_.tree ||
            menu->MapMenuLegend != legend_.widget || menu->MapMenuZoomSlider != zoom_.widget ||
            menu->MapMenuMapArea != map_.widget))
        {
            if (!RestoreAndRelease())
            {
                LOG_WARN("MainMap: ExpandedMap menu identity changed; original layout restoration is incomplete");
                return UpdateResult::Ready;
            }
        }

        if (!captured_)
        {
            Widgets discovered{};
            if (!Discover(menu, discovered) || !Capture(menu, discovered))
            {
                failedMenu_ = menu;
                ReleaseLeases();
                LOG_WARN("MainMap: ExpandedMap native widget validation failed; expansion disabled for this menu");
                return UpdateResult::Ready;
            }
            capturedWidgets_ = discovered;
        }

        return UpdateCaptured(menu, expanded, allottedSize);
    }
}
