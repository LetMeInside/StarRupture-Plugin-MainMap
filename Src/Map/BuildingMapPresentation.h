// Owns the one terrain-canvas image that presents BuildingMapTarget's Section composition.
// The owner retains the exact material, widget, and bound target objects until admitted cleanup.
#pragma once

#include "BuildingLayer.h"
#include "BuildingMapTarget.h"
#include "Native/NativeApi.h"

#include <cstdint>

namespace SDK
{
    class UCanvasPanel;
    class UImage;
    class UMaterialInstanceDynamic;
    class UTextureRenderTarget2D;
    class UWidgetTree;
    struct FGeometry;
    struct FVector2D;
}

namespace MainMapBuildingMap
{
    /** Presents one composed Section on the existing terrain canvas without owning its source or target. */
    class BuildingMapPresentation final
    {
    public:
        /** Updates the one session presentation from the ready target and current terrain context. */
        static bool UpdateShared(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree, const SectionTargetInfo& targetInfo,
            bool targetReady, bool materialResourcesReady);
        /** Detaches the image and releases its retained objects in dependency order. */
        static bool RetireShared(bool widgetDestructionStarted = false) noexcept;

    private:
        /** Restricts ownership to the one process-lifetime instance used by BuildingLayer. */
        BuildingMapPresentation() = default;
        /** Prevents copying the UObject lease owner or duplicating its widget. */
        BuildingMapPresentation(const BuildingMapPresentation&) = delete;
        /** Prevents copying the UObject lease owner or duplicating its widget. */
        BuildingMapPresentation& operator=(const BuildingMapPresentation&) = delete;
        /** Returns the persistent owner so rejected releases remain recorded for later cleanup. */
        static BuildingMapPresentation& SharedOwner() noexcept;
        /** Runs one presentation update for the supplied session and completed target. */
        bool Update(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree, const SectionTargetInfo& targetInfo,
            bool targetReady, bool materialResourcesReady);
        /** Loads and retains the mounted UI parent material once per visible session. */
        bool EnsureMaterial(SDK::UObject* worldContext, bool materialResourcesReady);
        /** Creates, retains, and attaches the single UImage to the active terrain canvas. */
        bool EnsureImage(SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree);
        /** Validates that the retained image remains attached through its expected CanvasPanelSlot. */
        bool ValidateAttachment(SDK::UCanvasPanel* canvas, SDK::UWidgetTree* tree) const noexcept;
        /** Maps Section bounds through terrain and canvas Slate geometry into checked float coordinates. */
        bool MapSectionGeometry(const MainMapBuildingLayer::ConsumerContext& context,
            SDK::UCanvasPanel* canvas, const SectionTargetInfo& targetInfo,
            SDK::FVector2D& position, SDK::FVector2D& size) const;
        /** Applies explicit top-left CanvasPanelSlot geometry and verifies its observable values. */
        bool ApplyGeometry(const SDK::FVector2D& position, const SDK::FVector2D& size);
        /** Reuses an unchanged target or commits a new target after binding and geometry validation. */
        bool SynchronizeTarget(const SectionTargetInfo& targetInfo,
            const SDK::FVector2D& position, const SDK::FVector2D& size);
        /** Restores the last valid Section rectangle after a candidate target update fails. */
        bool RestoreDisplayedGeometry();
        /** Converts terrain and canvas geometry corners into checked slot coordinates. */
        bool MapAbsoluteBoundsToCanvas(const SDK::FGeometry& canvasGeometry,
            const SDK::FGeometry& terrainGeometry, double localX, double localY, double localSize,
            SDK::FVector2D& position, SDK::FVector2D& size) const;
        /** Binds a retained target to the MID and confirms the parameter readback identity. */
        bool BindTarget(SDK::UTextureRenderTarget2D* target);
        /** Checks whether the MID currently reads back the exact selected target identity. */
        bool IsTargetBound(SDK::UTextureRenderTarget2D* target) const noexcept;
        /** Restores the last valid target binding after a candidate update fails. */
        bool RestoreCurrentTarget() noexcept;
        /** Hides the image and removes its material's target reference before target release. */
        bool WithdrawTarget() noexcept;
        /** Retries release of the previously bound target after a verified replacement. */
        bool RetryRetiredTarget() noexcept;
        /** Hides the image while preserving all resources needed for a later retry. */
        void HideImage() noexcept;
        /** Makes the validated image visible without intercepting native map input. */
        bool ShowImage() noexcept;
        /** Cleans up every retained UObject while preserving leases after rejected releases. */
        bool Retire(bool widgetDestructionStarted) noexcept;
        /** Reports a retirement failure once while its exact stage stays unchanged. */
        void LogRetirementFailure(const char* stage) noexcept;
        /** Releases the MID before target leases after its image brush is detached. */
        bool ReleaseDetachedResources(bool fullRetirement) noexcept;
        /** Returns the existing dynamic presentation material, if it has been retained. */
        SDK::UMaterialInstanceDynamic* MaterialInstance() const noexcept;
        /** Returns the existing retained UImage, if one has been created. */
        SDK::UImage* Image() const noexcept;

        MainMapNative::StrongObjectLease parentMaterial_;
        MainMapNative::StrongObjectLease materialInstance_;
        MainMapNative::StrongObjectLease image_;
        MainMapNative::StrongObjectLease target_;
        MainMapNative::StrongObjectLease candidateTarget_;
        MainMapNative::StrongObjectLease retiredTarget_;
        SectionTargetInfo presentedInfo_{};
        uint64_t epoch_ = 0;
        const char* retirementFailureStage_ = nullptr;
        bool materialLoadAttempted_ = false;
        bool materialInstanceAttempted_ = false;
        bool materialValidated_ = false;
        bool presentedTargetValid_ = false;
        bool widgetDetached_ = false;
        bool materialInstanceDetached_ = false;
        bool firstAttachmentLogged_ = false;
    };
}
