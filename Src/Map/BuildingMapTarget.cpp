// Retains MainMap's single-Section path and prepares internal coordinate-keyed composite GPU state.
#include "BuildingMapTarget.h"

#include "BuildingLayer.h"
#include "MaterialProof.h"
#include "Engine_classes.hpp"
#include "plugin_helpers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace MainMapBuildingMap
{
    namespace
    {
        constexpr char kComposeMaterialPath[] =
            "/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose";
        constexpr int32_t kCompositeTargetSize = 1024;
    }

    /** Returns a process-lifetime owner whose leases survive a rejected shutdown release. */
    BuildingMapTarget& BuildingMapTarget::SharedOwner() noexcept
    {
        alignas(BuildingMapTarget) static uint8_t storage[sizeof(BuildingMapTarget)]{};
        static BuildingMapTarget* target = ::new (storage) BuildingMapTarget();
        return *target;
    }

    /** Retires resources owned by the shared instance through admitted lease releases. */
    bool BuildingMapTarget::RetireShared() noexcept
    {
        return SharedOwner().Retire();
    }

    /** Exposes read-only target status and bounds to the next presentation stage. */
    const BuildingMapTarget& BuildingMapTarget::GetShared() noexcept
    {
        return SharedOwner();
    }

    /** Plans changed visible geometry into the existing target owner's CPU-only slot model. */
    bool BuildingMapTarget::UpdateGridPlanShared(
        const MainMapBuildingLayer::WorldRect& visibleWorld, std::uint64_t sessionEpoch) noexcept
    {
        return SharedOwner().UpdateGridPlan(visibleWorld, sessionEpoch);
    }

    /** Reconciles remote Section observations into the assigned grid after the regular provider poll. */
    void BuildingMapTarget::ReconcileGridSourcesShared(const BuildingMapRemote& remote,
        std::uint64_t sessionEpoch) noexcept
    {
        SharedOwner().ReconcileGridSources(remote, sessionEpoch);
    }

    /** Marks the latest planning input unavailable until another valid rectangle is observed. */
    bool BuildingMapTarget::InvalidateGridPlanInputShared() noexcept
    {
        return SharedOwner().InvalidateGridPlanInput();
    }

    /** Reports whether the most recently observed geometry produced a valid plan. */
    bool BuildingMapTarget::HasCurrentGridPlanShared() noexcept
    {
        return SharedOwner().HasCurrentGridPlan();
    }

    /** Exposes the target owner's current CPU plan and slot state without engine ownership. */
    const BuildingMapGridPlanner& BuildingMapTarget::GetGridPlannerShared() noexcept
    {
        return SharedOwner().gridPlanner_;
    }

    /** Reports whether the target owner reconciled the latest complete remote snapshot. */
    bool BuildingMapTarget::HasCurrentGridSourcesShared() noexcept
    {
        return SharedOwner().sourceSnapshotValid_;
    }

    /** Exposes the bounded metadata-only source membership for the current assigned tiles. */
    const std::vector<BuildingMapTarget::CompositeSectionSource>&
        BuildingMapTarget::GetGridSourcesShared() noexcept
    {
        return SharedOwner().compositeSources_;
    }

    /** Copies the six current assignment and retained-target identities for read-only publication checks. */
    std::array<BuildingMapTarget::CompositeTargetInfo,
        BuildingMapGridPlanner::kPhysicalSlotCount>
        BuildingMapTarget::GetCompositeTargetInfosShared() noexcept
    {
        const BuildingMapTarget& owner = SharedOwner();
        std::array<CompositeTargetInfo, BuildingMapGridPlanner::kPhysicalSlotCount> output{};
        const auto& assignments = owner.gridPlanner_.Assignments();
        for (std::size_t slotIndex = 0; slotIndex < output.size(); ++slotIndex)
        {
            output[slotIndex].Assignment = assignments[slotIndex];
            const CompositeSlotResources& resources = owner.compositeTargets_[slotIndex];
            output[slotIndex].Target = static_cast<SDK::UTextureRenderTarget2D*>(
                resources.Resources.RenderTarget.Get());
            output[slotIndex].TargetMatchesAssignment = resources.HasAssignment &&
                !resources.TargetResourceInvalid &&
                resources.ResourceSessionEpoch == assignments[slotIndex].SessionEpoch &&
                assignments[slotIndex].IsAssigned &&
                resources.SessionEpoch == assignments[slotIndex].SessionEpoch &&
                resources.AssignmentGeneration == assignments[slotIndex].AssignmentGeneration &&
                resources.Tile.GroupingN == assignments[slotIndex].Tile.GroupingN &&
                resources.Tile.X == assignments[slotIndex].Tile.X &&
                resources.Tile.Y == assignments[slotIndex].Tile.Y &&
                resources.Bounds.MinX == assignments[slotIndex].Bounds.MinX &&
                resources.Bounds.MinY == assignments[slotIndex].Bounds.MinY &&
                resources.Bounds.MaxX == assignments[slotIndex].Bounds.MaxX &&
                resources.Bounds.MaxY == assignments[slotIndex].Bounds.MaxY &&
                IsCompositeTargetAllocationValid(output[slotIndex].Target);
#if defined(_DEBUG)
            output[slotIndex].TargetWasReusedForAssignment =
                resources.LastReusedAssignmentGeneration == assignments[slotIndex].AssignmentGeneration &&
                assignments[slotIndex].HasCommittedContent && !assignments[slotIndex].IsDirty;
#endif
        }
        return output;
    }

    /** Prepares assigned composites after BuildingLayer withdraws any affected publication. */
    void BuildingMapTarget::PrepareGridCompositesShared(SDK::UObject* worldContext,
        const BuildingMapRemote& remote, std::uint64_t sessionEpoch,
        bool materialResourcesReady) noexcept
    {
        SharedOwner().PrepareGridComposites(worldContext, remote, sessionEpoch, materialResourcesReady);
    }

    /** Reports whether a fully composed target is currently retained. */
    bool BuildingMapTarget::IsReady() const noexcept
    {
        return ready_ && current_.RenderTarget.Get() != nullptr;
    }

    /** Returns the last resource state observed by Update or Retire. */
    BuildingMapTarget::Status BuildingMapTarget::GetStatus() const noexcept
    {
        return status_;
    }

    /** Exposes only the retained target and the Section bounds paired with its pixels. */
    SectionTargetInfo BuildingMapTarget::GetTargetInfo() const noexcept
    {
        SectionTargetInfo result = targetInfo_;
        result.Target = IsReady()
            ? static_cast<SDK::UTextureRenderTarget2D*>(current_.RenderTarget.Get())
            : nullptr;
        return result;
    }

    /** Keeps the selected visible Section stable and rebuilds only when its revision changes. */
    void BuildingMapTarget::Update(SDK::UObject* worldContext,
        const BuildingMapRemote::ImageCollection& images,
        const MainMapBuildingLayer::WorldRect& visibleWorld, bool materialResourcesReady)
    {
        if (!MainMapNative::CanOperate() || !worldContext)
        {
            status_ = Status::Failure;
            return;
        }
        if (!std::isfinite(visibleWorld.minX) || !std::isfinite(visibleWorld.minY) ||
            !std::isfinite(visibleWorld.maxX) || !std::isfinite(visibleWorld.maxY) ||
            visibleWorld.maxX <= visibleWorld.minX || visibleWorld.maxY <= visibleWorld.minY)
        {
            status_ = Status::Failure;
            return;
        }
        const char* selectionReason = nullptr;
        const SectionImage* selected = SelectVisibleImage(images, visibleWorld, selectionReason);
        if (!selected)
        {
            Retire();
            return;
        }
        if (!materialResourcesReady)
        {
            status_ = Status::ResourcesUnavailable;
            return;
        }
        if (materialLoadAttempted_ && !materialValidated_)
        {
            status_ = Status::ResourcesUnavailable;
            return;
        }
        if (pendingReady_ && MatchesSelectedImage(*selected))
        {
            PromotePendingResources(*selected);
            return;
        }
        if (!ClearStalePendingResources(*selected))
        {
            status_ = Status::Failure;
            return;
        }
        if (IsCurrentImage(*selected))
        {
            ready_ = true;
            status_ = Status::Ready;
            return;
        }
        BuildSelectedImage(worldContext, *selected, selectionReason);
    }

    /** Retires current and pending objects while preserving any lease that cannot release. */
    bool BuildingMapTarget::Retire() noexcept
    {
        ready_ = false;
        targetInfo_ = {};
        pendingInfo_ = {};
        revision_ = 0;
        pendingRevision_ = 0;
        pendingReady_ = false;
        pendingSelectionReason_ = nullptr;
        if (!ResetResources(pending_) || !ResetResources(current_))
        {
            status_ = Status::Failure;
            return false;
        }
        if (!material_.Reset())
        {
            status_ = Status::Failure;
            return false;
        }
        materialValidated_ = false;
        materialLoadAttempted_ = false;
        status_ = Status::NoImage;
        return true;
    }

    /** Validates the byte contract and writes a lossless RGBA PNG into memory. */
    bool BuildingMapTarget::EncodePng(const SectionImage& image, std::vector<uint8_t>& pngBytes)
    {
        pngBytes.clear();
        if (image.Width == 0 || image.Height == 0 || image.Revision == 0 ||
            image.Width > static_cast<uint32_t>((std::numeric_limits<int32_t>::max)()) ||
            image.Height > static_cast<uint32_t>((std::numeric_limits<int32_t>::max)()) ||
            image.Width > (std::numeric_limits<uint64_t>::max)() / 4)
        {
            return false;
        }
        const uint64_t expectedStride = uint64_t(image.Width) * 4;
        if (image.RowStride != expectedStride ||
            expectedStride > (std::numeric_limits<uint64_t>::max)() / image.Height ||
            image.ByteCount != expectedStride * image.Height ||
            image.Pixels.size() != image.ByteCount)
        {
            return false;
        }

        std::vector<uint8_t> deflateBytes;
        try
        {
            if (!EncodeDeflate(image, deflateBytes) ||
                deflateBytes.size() > (std::numeric_limits<uint32_t>::max)() ||
                deflateBytes.size() >
                    static_cast<size_t>((std::numeric_limits<int32_t>::max)()) - 57)
            {
                return false;
            }
            pngBytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            std::vector<uint8_t> header;
            AppendBigEndian32(image.Width, header);
            AppendBigEndian32(image.Height, header);
            header.insert(header.end(), {8, 6, 0, 0, 0});
            AppendPngChunk("IHDR", header.data(), header.size(), pngBytes);
            AppendPngChunk("IDAT", deflateBytes.data(), deflateBytes.size(), pngBytes);
            AppendPngChunk("IEND", nullptr, 0, pngBytes);
        }
        catch (...)
        {
            pngBytes.clear();
            return false;
        }
        return pngBytes.size() <= static_cast<size_t>((std::numeric_limits<int32_t>::max)());
    }

    /** Builds PNG scanlines and a standards-compliant stored DEFLATE stream. */
    bool BuildingMapTarget::EncodeDeflate(const SectionImage& image, std::vector<uint8_t>& deflateBytes)
    {
        if (image.ByteCount > (std::numeric_limits<size_t>::max)() - image.Height)
        {
            return false;
        }
        const size_t rawSize = static_cast<size_t>(image.ByteCount + image.Height);
        if (rawSize > (std::numeric_limits<uint32_t>::max)())
        {
            return false;
        }
        std::vector<uint8_t> scanlines;
        scanlines.reserve(rawSize);
        const size_t stride = static_cast<size_t>(image.RowStride);
        for (uint32_t row = 0; row < image.Height; ++row)
        {
            scanlines.push_back(0);
            const size_t rowStart = size_t(row) * stride;
            scanlines.insert(scanlines.end(), image.Pixels.begin() + rowStart,
                image.Pixels.begin() + rowStart + stride);
        }

        deflateBytes.clear();
        deflateBytes.reserve(rawSize + 6 + (rawSize / 65535 + 1) * 5);
        deflateBytes.push_back(0x78);
        deflateBytes.push_back(0x01);
        for (size_t offset = 0; offset < rawSize;)
        {
            const size_t blockSize = (std::min)(size_t{65535}, rawSize - offset);
            const bool finalBlock = offset + blockSize == rawSize;
            deflateBytes.push_back(finalBlock ? 1 : 0);
            deflateBytes.push_back(static_cast<uint8_t>(blockSize));
            deflateBytes.push_back(static_cast<uint8_t>(blockSize >> 8));
            const uint16_t inverseSize = static_cast<uint16_t>(~static_cast<uint16_t>(blockSize));
            deflateBytes.push_back(static_cast<uint8_t>(inverseSize));
            deflateBytes.push_back(static_cast<uint8_t>(inverseSize >> 8));
            deflateBytes.insert(deflateBytes.end(), scanlines.begin() + offset,
                scanlines.begin() + offset + blockSize);
            offset += blockSize;
        }
        AppendAdler32(scanlines, deflateBytes);
        return true;
    }

    /** Adds one PNG chunk and its CRC without writing any data to disk. */
    void BuildingMapTarget::AppendPngChunk(const char type[4], const uint8_t* payload,
        size_t payloadSize, std::vector<uint8_t>& pngBytes)
    {
        AppendBigEndian32(static_cast<uint32_t>(payloadSize), pngBytes);
        const size_t crcStart = pngBytes.size();
        pngBytes.insert(pngBytes.end(), type, type + 4);
        if (payloadSize != 0)
        {
            pngBytes.insert(pngBytes.end(), payload, payload + payloadSize);
        }
        uint32_t crc = 0xFFFFFFFF;
        for (size_t index = crcStart; index < pngBytes.size(); ++index)
        {
            crc = UpdateCrc32(crc, pngBytes[index]);
        }
        AppendBigEndian32(crc ^ 0xFFFFFFFF, pngBytes);
    }

    /** Applies the PNG CRC-32 polynomial to one input byte. */
    uint32_t BuildingMapTarget::UpdateCrc32(uint32_t crc, uint8_t value) noexcept
    {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
        }
        return crc;
    }

    /** Appends one unsigned integer in PNG network byte order. */
    void BuildingMapTarget::AppendBigEndian32(uint32_t value, std::vector<uint8_t>& bytes)
    {
        bytes.push_back(static_cast<uint8_t>(value >> 24));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    }

    /** Appends the Adler-32 trailer required by a zlib-wrapped DEFLATE stream. */
    void BuildingMapTarget::AppendAdler32(const std::vector<uint8_t>& bytes,
        std::vector<uint8_t>& deflateBytes)
    {
        uint32_t first = 1;
        uint32_t second = 0;
        for (const uint8_t value : bytes)
        {
            first = (first + value) % 65521;
            second = (second + first) % 65521;
        }
        const uint32_t adler = (second << 16) | first;
        AppendBigEndian32(adler, deflateBytes);
    }

    /** Imports source bytes and validates texture class, dimensions, and sRGB state. */
    bool BuildingMapTarget::CreateInputTexture(SDK::UObject* worldContext,
        const SectionImage& image, ResourceSet& resources)
    {
        std::vector<uint8_t> pngBytes;
        if (!EncodePng(image, pngBytes))
        {
            return false;
        }
        UC::TArray<UC::uint8> importBuffer(pngBytes.data(),
            static_cast<UC::int32>(pngBytes.size()), static_cast<UC::int32>(pngBytes.size()));
        auto* imported = SDK::UKismetRenderingLibrary::ImportBufferAsTexture2D(worldContext, importBuffer);
        if (!imported || !imported->IsA(SDK::UTexture2D::StaticClass()) ||
            !resources.InputTexture.Acquire(imported))
        {
            return false;
        }
        auto* texture = static_cast<SDK::UTexture2D*>(imported);
        return texture->Blueprint_GetSizeX() == static_cast<UC::int32>(image.Width) &&
            texture->Blueprint_GetSizeY() == static_cast<UC::int32>(image.Height) && texture->SRGB != 0;
    }

    /** Creates the exact-size linear float target and checks all exposed format flags. */
    bool BuildingMapTarget::CreateRenderTarget(SDK::UObject* worldContext,
        const SectionImage& image, ResourceSet& resources)
    {
        SDK::FLinearColor clearColor{};
        clearColor.R = 0.0f;
        clearColor.G = 0.0f;
        clearColor.B = 0.0f;
        clearColor.A = 1.0f;
        auto* target = SDK::UKismetRenderingLibrary::CreateRenderTarget2D(worldContext,
            static_cast<UC::int32>(image.Width), static_cast<UC::int32>(image.Height),
            SDK::ETextureRenderTargetFormat::RTF_RGBA16f, clearColor, false, false);
        if (!target || !target->IsA(SDK::UTextureRenderTarget2D::StaticClass()) ||
            !resources.RenderTarget.Acquire(target))
        {
            return false;
        }
        return target->SizeX == static_cast<UC::int32>(image.Width) &&
            target->SizeY == static_cast<UC::int32>(image.Height) &&
            target->RenderTargetFormat == SDK::ETextureRenderTargetFormat::RTF_RGBA16f &&
            target->bForceLinearGamma != 0 && target->bAutoGenerateMips == 0 &&
            target->ClearColor.R == 0.0f && target->ClearColor.G == 0.0f &&
            target->ClearColor.B == 0.0f && target->ClearColor.A == 1.0f;
    }

    /** Loads the retained parent once, binds SpriteAtlas, and verifies the instance value. */
    bool BuildingMapTarget::CreateCompositionMaterial(SDK::UObject* worldContext,
        SDK::UTexture2D* inputTexture, ResourceSet& resources, bool updateTargetStatus,
        bool* leaseReleaseDeferred)
    {
        if (!EnsureCompositionMaterial(updateTargetStatus, leaseReleaseDeferred)) return false;
        if (updateTargetStatus) status_ = Status::Failure;
        auto* instance = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(worldContext,
            static_cast<SDK::UMaterialInterface*>(material_.Get()), SDK::FName(),
            SDK::EMIDCreationFlags::Transient);
        if (!instance || !instance->IsA(SDK::UMaterialInstanceDynamic::StaticClass()) ||
            !resources.MaterialInstance.Acquire(instance))
        {
            return false;
        }
        if (!inputTexture) return true;
        return BindCompositionSource(instance, inputTexture);
    }

    /** Loads and retains the shared parent, distinguishing temporary lease-release rejection. */
    bool BuildingMapTarget::EnsureCompositionMaterial(bool updateTargetStatus,
        bool* leaseReleaseDeferred)
    {
        if (leaseReleaseDeferred) *leaseReleaseDeferred = false;
        if (materialValidated_) return true;
        if (material_.Get() && !material_.Reset())
        {
            if (leaseReleaseDeferred) *leaseReleaseDeferred = true;
            if (updateTargetStatus) status_ = Status::ResourcesUnavailable;
            return false;
        }
        if (materialLoadAttempted_)
        {
            if (updateTargetStatus) status_ = Status::ResourcesUnavailable;
            return false;
        }
        materialLoadAttempted_ = true;
        auto* material = MainMapProof::LoadMaterial(kComposeMaterialPath,
            SDK::EMaterialDomain::MD_Surface, material_);
        if (!material)
        {
            if (material_.Get() && !material_.Reset())
            {
                materialLoadAttempted_ = false;
                if (leaseReleaseDeferred) *leaseReleaseDeferred = true;
                if (updateTargetStatus) status_ = Status::ResourcesUnavailable;
                return false;
            }
            if (updateTargetStatus) status_ = Status::ResourcesUnavailable;
            return false;
        }
        materialValidated_ = true;
        return true;
    }

    /** Binds the existing material parameter to one texture and confirms the same UObject is read back. */
    bool BuildingMapTarget::BindCompositionSource(SDK::UMaterialInstanceDynamic* material,
        SDK::UTexture2D* inputTexture) const
    {
        if (!material || !inputTexture) return false;
        const SDK::FName parameterName = SDK::UKismetStringLibrary::Conv_StringToName(
            SDK::FString(L"SpriteAtlas"));
        material->SetTextureParameterValue(parameterName, inputTexture);
        return material->K2_GetTextureParameterValue(parameterName) == inputTexture;
    }

    /** Creates the fixed linear RGBA16f target and verifies its required allocation flags. */
    bool BuildingMapTarget::CreateCompositeRenderTarget(SDK::UObject* worldContext,
        ResourceSet& resources)
    {
        SDK::FLinearColor clearColor{};
        clearColor.R = 0.0f;
        clearColor.G = 0.0f;
        clearColor.B = 0.0f;
        clearColor.A = 1.0f;
        auto* target = SDK::UKismetRenderingLibrary::CreateRenderTarget2D(worldContext,
            kCompositeTargetSize, kCompositeTargetSize,
            SDK::ETextureRenderTargetFormat::RTF_RGBA16f, clearColor, false, false);
        if (!target || !target->IsA(SDK::UTextureRenderTarget2D::StaticClass()) ||
            !resources.RenderTarget.Acquire(target)) return false;
        return target->SizeX == kCompositeTargetSize && target->SizeY == kCompositeTargetSize &&
            target->RenderTargetFormat == SDK::ETextureRenderTargetFormat::RTF_RGBA16f &&
            target->bForceLinearGamma != 0 && target->bAutoGenerateMips == 0 &&
            target->ClearColor.R == 0.0f && target->ClearColor.G == 0.0f &&
            target->ClearColor.B == 0.0f && target->ClearColor.A == 1.0f;
    }

    /** Clears, begins, draws two full-extent triangles, and always ends a valid Canvas context. */
    bool BuildingMapTarget::ComposeSection(SDK::UObject* worldContext, int32_t width,
        int32_t height, ResourceSet& resources)
    {
        auto* target = static_cast<SDK::UTextureRenderTarget2D*>(resources.RenderTarget.Get());
        auto* material = static_cast<SDK::UMaterialInstanceDynamic*>(resources.MaterialInstance.Get());
        if (!target || !material || width <= 0 || height <= 0)
        {
            return false;
        }
        SDK::FLinearColor clearColor{};
        clearColor.R = 0.0f;
        clearColor.G = 0.0f;
        clearColor.B = 0.0f;
        clearColor.A = 1.0f;
        SDK::UKismetRenderingLibrary::ClearRenderTarget2D(worldContext, target, clearColor);
        SDK::UCanvas* canvas = nullptr;
        SDK::FVector2D canvasSize{};
        SDK::FDrawToRenderTargetContext context{};
        SDK::UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(worldContext, target,
            &canvas, &canvasSize, &context);
        if (!canvas || context.RenderTarget != target)
        {
            if (canvas || context.RenderTarget)
            {
                SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(worldContext, context);
            }
            return false;
        }
        SDK::FCanvasUVTri triangles[2]{};
        SetTriangle(triangles[0], {0.0f, 0.0f}, {0.0f, 0.0f},
            {static_cast<float>(width), 0.0f}, {1.0f, 0.0f},
            {static_cast<float>(width), static_cast<float>(height)}, {1.0f, 1.0f});
        SetTriangle(triangles[1], {0.0f, 0.0f}, {0.0f, 0.0f},
            {static_cast<float>(width), static_cast<float>(height)}, {1.0f, 1.0f},
            {0.0f, static_cast<float>(height)}, {0.0f, 1.0f});
        SDK::TArray<SDK::FCanvasUVTri> triangleArray(triangles, 2, 2);
        canvas->K2_DrawMaterialTriangle(material, triangleArray);
        SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(worldContext, context);
        return true;
    }

    /** Reuses a valid same-session target or creates the slot's first physical target. */
    bool BuildingMapTarget::EnsureCompositeSlotResources(SDK::UObject* worldContext,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        std::size_t physicalSlotIndex, CompositeSlotResources& slotResources)
    {
        if (physicalSlotIndex >= compositeTargets_.size() ||
            assignment.PhysicalSlotIndex != physicalSlotIndex || !assignment.IsAssigned) return false;
        auto* target = static_cast<SDK::UTextureRenderTarget2D*>(
            slotResources.Resources.RenderTarget.Get());
        if (target && (slotResources.TargetResourceInvalid ||
            slotResources.ResourceSessionEpoch != assignment.SessionEpoch ||
            !IsCompositeTargetAllocationValid(target)))
        {
            if (!RetireInvalidCompositeTarget(physicalSlotIndex, assignment,
                slotResources)) return false;
        }
        target = static_cast<SDK::UTextureRenderTarget2D*>(
            slotResources.Resources.RenderTarget.Get());
        if (target) return ReassignCompositeTarget(assignment, slotResources);
        return AllocateCompositeTarget(worldContext, assignment, physicalSlotIndex, slotResources);
    }

    /** Retires invalid storage without losing the target lease if admitted release is rejected. */
    bool BuildingMapTarget::RetireInvalidCompositeTarget(std::size_t physicalSlotIndex,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        CompositeSlotResources& resources)
    {
#if defined(_DEBUG)
        auto* target = resources.Resources.RenderTarget.Get();
        const auto oldTile = resources.Tile;
        const std::uint64_t oldEpoch = resources.SessionEpoch;
        const std::uint64_t oldGeneration = resources.AssignmentGeneration;
        const bool hadAssignment = resources.HasAssignment;
        const bool releaseWasDeferred = resources.LeaseReleaseDeferred;
#endif
        if (!RetireCompositeSlotResources(resources))
        {
#if defined(_DEBUG)
            if (!resources.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                LogGridTargetTransition(physicalSlotIndex, "invalid-target-retirement-deferred",
                    target, oldTile, oldEpoch, oldGeneration, assignment, 0);
            }
            resources.LeaseReleaseDeferred = true;
#endif
            return false;
        }
#if defined(_DEBUG)
        if (hadAssignment)
        {
            LogGridTargetTransition(physicalSlotIndex, "target-retired-before-reallocation",
                target, oldTile, oldEpoch, oldGeneration, {}, 0);
        }
        if (releaseWasDeferred) ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
        ++gridDiagnosticBatch_.RetiredTargets;
        resources.LeaseReleaseDeferred = false;
#endif
        return true;
    }

    /** Preserves a valid physical target while advancing only its logical assignment identity. */
    bool BuildingMapTarget::ReassignCompositeTarget(
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        CompositeSlotResources& resources)
    {
        if (MatchesCompositeAssignment(resources, assignment)) return true;
#if defined(_DEBUG)
        if (resources.HasAssignment)
        {
            resources.ReuseFromTile = resources.Tile;
            resources.ReuseFromSessionEpoch = resources.SessionEpoch;
            resources.ReuseFromAssignmentGeneration = resources.AssignmentGeneration;
        }
        else if (resources.ReuseFromSessionEpoch != resources.ResourceSessionEpoch)
        {
            resources.ReuseFromTile = {};
            resources.ReuseFromSessionEpoch = resources.ResourceSessionEpoch;
            resources.ReuseFromAssignmentGeneration = 0;
        }
        resources.HasPendingReuseLog = true;
        ++gridDiagnosticBatch_.ReusedTargets;
#endif
        AssignCompositeSlot(resources, assignment);
        return true;
    }

    /** Allocates into an empty slot only after any partially-created resource releases succeed. */
    bool BuildingMapTarget::AllocateCompositeTarget(SDK::UObject* worldContext,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        std::size_t physicalSlotIndex, CompositeSlotResources& slotResources)
    {
        if (slotResources.Resources.MaterialInstance || slotResources.Resources.InputTexture)
        {
            if (!ResetResources(slotResources.Resources)) return false;
        }
        ResourceSet candidate{};
        const bool targetCreated = CreateCompositeRenderTarget(worldContext, candidate);
        auto* candidateTarget = static_cast<SDK::UTextureRenderTarget2D*>(
            candidate.RenderTarget.Get());
        if (!targetCreated || !IsCompositeTargetAllocationValid(candidateTarget))
        {
            if (!ResetResources(candidate))
            {
                slotResources.Resources = std::move(candidate);
                slotResources.ResourceSessionEpoch = assignment.SessionEpoch;
#if defined(_DEBUG)
                slotResources.LeaseReleaseDeferred = true;
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                LogGridTargetTransition(physicalSlotIndex, "candidate-target-retirement-deferred",
                    slotResources.Resources.RenderTarget.Get(),
                    {}, 0, 0, assignment, 0);
#endif
            }
            return false;
        }
        slotResources.Resources.RenderTarget = std::move(candidate.RenderTarget);
        AssignCompositeSlot(slotResources, assignment);
        slotResources.ResourceSessionEpoch = assignment.SessionEpoch;
#if defined(_DEBUG)
        ++gridDiagnosticBatch_.CreatedTargets;
        auto* target = static_cast<SDK::UTextureRenderTarget2D*>(
            slotResources.Resources.RenderTarget.Get());
        LogGridTargetTransition(physicalSlotIndex, "target-allocated", target,
            {}, 0, 0, assignment, 0);
#endif
        return true;
    }

    /** Compares tile, epoch, exact bounds, and assignment generation without inspecting pixels. */
    bool BuildingMapTarget::MatchesCompositeAssignment(const CompositeSlotResources& resources,
        const BuildingMapGridPlanner::SlotAssignment& assignment) noexcept
    {
        return resources.HasAssignment && resources.AssignmentGeneration == assignment.AssignmentGeneration &&
            resources.SessionEpoch == assignment.SessionEpoch &&
            resources.Tile.GroupingN == assignment.Tile.GroupingN &&
            resources.Tile.X == assignment.Tile.X && resources.Tile.Y == assignment.Tile.Y &&
            resources.Bounds.MinX == assignment.Bounds.MinX &&
            resources.Bounds.MinY == assignment.Bounds.MinY &&
            resources.Bounds.MaxX == assignment.Bounds.MaxX &&
            resources.Bounds.MaxY == assignment.Bounds.MaxY;
    }

    /** Clears a dirty target and submits its current accepted Section contributions. */
    bool BuildingMapTarget::ComposeCompositeTile(SDK::UObject* worldContext,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        const std::vector<const CompositeSectionSource*>& sources,
        const BuildingMapRemote& remote)
    {
#if defined(_DEBUG)
        currentCompositeFailureKind_ = 0;
        currentCompositeDrawSubmissions_ = 0;
#endif
        const std::size_t slotIndex = assignment.PhysicalSlotIndex;
        if (slotIndex >= compositeTargets_.size() ||
            !gridPlanner_.InvalidateCommittedContent(slotIndex, assignment.AssignmentGeneration))
        {
#if defined(_DEBUG)
            currentCompositeFailureKind_ = 2;
#endif
            return false;
        }
        auto* target = static_cast<SDK::UTextureRenderTarget2D*>(
            compositeTargets_[slotIndex].Resources.RenderTarget.Get());
        if (!target)
        {
#if defined(_DEBUG)
            currentCompositeFailureKind_ = 1;
#endif
            return false;
        }
        SDK::UCanvas* canvas = nullptr;
        SDK::FDrawToRenderTargetContext context{};
        if (!BeginCompositeCanvas(worldContext, target, canvas, context))
        {
            compositeTargets_[slotIndex].TargetResourceInvalid = true;
#if defined(_DEBUG)
            currentCompositeFailureKind_ = 1;
            auto& resources = compositeTargets_[slotIndex];
            if (ShouldLogGridCompositeFailure(resources, assignment, 3))
            {
                ++gridDiagnosticBatch_.ResourceFailures;
                LogGridTargetTransition(slotIndex, "target-resource-validation-failed", target,
                    resources.Tile, resources.SessionEpoch, resources.AssignmentGeneration,
                    assignment, 0);
            }
#endif
            return false;
        }
        const bool submitted = DrawCompositeSources(assignment, sources, remote, canvas);
        SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(worldContext, context);
#if defined(_DEBUG)
        if (!submitted) currentCompositeFailureKind_ = 2;
#endif
        return submitted;
    }

    /** Clears one target and accepts its Canvas context before any Section triangles are submitted. */
    bool BuildingMapTarget::BeginCompositeCanvas(SDK::UObject* worldContext,
        SDK::UTextureRenderTarget2D* target, SDK::UCanvas*& canvas,
        SDK::FDrawToRenderTargetContext& context)
    {
        if (!target) return false;
        SDK::FLinearColor clearColor{};
        clearColor.R = 0.0f;
        clearColor.G = 0.0f;
        clearColor.B = 0.0f;
        clearColor.A = 1.0f;
        SDK::UKismetRenderingLibrary::ClearRenderTarget2D(worldContext, target, clearColor);
        SDK::FVector2D canvasSize{};
        SDK::UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(worldContext, target,
            &canvas, &canvasSize, &context);
        if (canvas && context.RenderTarget == target) return true;
        if (canvas || context.RenderTarget)
        {
            SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(worldContext, context);
        }
        return false;
    }

    /** Draws each currently available image after checking source and target assignment identities. */
    bool BuildingMapTarget::DrawCompositeSources(const BuildingMapGridPlanner::SlotAssignment& assignment,
        const std::vector<const CompositeSectionSource*>& sources,
        const BuildingMapRemote& remote, SDK::UCanvas* canvas)
    {
        const std::size_t slotIndex = assignment.PhysicalSlotIndex;
        if (slotIndex >= compositeTargets_.size()) return false;
        for (const CompositeSectionSource* source : sources)
        {
            if (!source || !source->HasImage || source->PhysicalSlotIndex != slotIndex) return false;
            const auto image = remote.Images().find({source->GridX, source->GridY});
            if (image == remote.Images().end() || !DrawCompositeSource(*source, image->second,
                assignment, canvas)) return false;
        }
        return true;
    }

    /** Validates provider-published bounds and maps one Section to its exact target rectangle. */
    bool BuildingMapTarget::DrawCompositeSource(const CompositeSectionSource& source,
        const SectionImage& image, const BuildingMapGridPlanner::SlotAssignment& assignment,
        SDK::UCanvas* canvas)
    {
        const auto texture = sourceTextures_.find({source.GridX, source.GridY});
        if (!canvas || texture == sourceTextures_.end() ||
            !texture->second.HasAcceptedTexture ||
            !texture->second.MaterialInstance.Get() ||
            !SameSourceTextureIdentity(texture->second.Accepted, source) ||
            !MatchesAcceptedSource(source, image) || source.SessionEpoch != assignment.SessionEpoch ||
            source.GroupingN != assignment.Tile.GroupingN ||
            FloorDivideSectionCoordinate(source.GridX, source.GroupingN) != assignment.Tile.X ||
            FloorDivideSectionCoordinate(source.GridY, source.GroupingN) != assignment.Tile.Y) return false;
        CompositeDestinationRectangle destination{};
        if (!TryGetCompositeDestination(image, assignment, destination)) return false;
        auto* material = static_cast<SDK::UMaterialInstanceDynamic*>(
            texture->second.MaterialInstance.Get());
        SDK::FCanvasUVTri triangles[2]{};
        SetTriangle(triangles[0], {static_cast<float>(destination.Left),
            static_cast<float>(destination.Top)}, {0.0f, 0.0f},
            {static_cast<float>(destination.Right), static_cast<float>(destination.Top)}, {1.0f, 0.0f},
            {static_cast<float>(destination.Right), static_cast<float>(destination.Bottom)}, {1.0f, 1.0f});
        SetTriangle(triangles[1], {static_cast<float>(destination.Left),
            static_cast<float>(destination.Top)}, {0.0f, 0.0f},
            {static_cast<float>(destination.Right), static_cast<float>(destination.Bottom)}, {1.0f, 1.0f},
            {static_cast<float>(destination.Left), static_cast<float>(destination.Bottom)}, {0.0f, 1.0f});
        SDK::TArray<SDK::FCanvasUVTri> triangleArray(triangles, 2, 2);
        canvas->K2_DrawMaterialTriangle(material, triangleArray);
#if defined(_DEBUG)
        ++currentCompositeDrawSubmissions_;
        ++gridDiagnosticBatch_.DrawSubmissions;
#endif
        return true;
    }

    /** Converts one provider Section rectangle to its exact checked composite target footprint. */
    bool BuildingMapTarget::TryGetCompositeDestination(const SectionImage& image,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        CompositeDestinationRectangle& output) noexcept
    {
        output = {};
        const double minimumX = static_cast<double>(assignment.Bounds.MinX);
        const double minimumY = static_cast<double>(assignment.Bounds.MinY);
        const double maximumX = static_cast<double>(assignment.Bounds.MaxX);
        const double maximumY = static_cast<double>(assignment.Bounds.MaxY);
        const double sectionMaximumX = image.XMapUnits + image.SectionSizeMapUnits;
        const double sectionMaximumY = image.YMapUnits + image.SectionSizeMapUnits;
        const double worldSide = maximumX - minimumX;
        if (!std::isfinite(image.XMapUnits) || !std::isfinite(image.YMapUnits) ||
            !std::isfinite(image.SectionSizeMapUnits) || image.SectionSizeMapUnits <= 0.0 ||
            !std::isfinite(sectionMaximumX) || !std::isfinite(sectionMaximumY) ||
            !std::isfinite(worldSide) || worldSide <= 0.0 || image.XMapUnits < minimumX ||
            image.YMapUnits < minimumY || sectionMaximumX > maximumX || sectionMaximumY > maximumY)
        {
            return false;
        }
        const double scale = static_cast<double>(kCompositeTargetSize) / worldSide;
        const double left = (image.XMapUnits - minimumX) * scale;
        const double top = (image.YMapUnits - minimumY) * scale;
        const double right = (sectionMaximumX - minimumX) * scale;
        const double bottom = (sectionMaximumY - minimumY) * scale;
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
            !std::isfinite(bottom) || left < 0.0 || top < 0.0 || right > kCompositeTargetSize ||
            bottom > kCompositeTargetSize || right <= left || bottom <= top) return false;
        output = {left, top, right, bottom};
        return true;
    }

    /** Assigns one Canvas triangle's positions, UVs, and opaque white vertex tint. */
    void BuildingMapTarget::SetTriangle(SDK::FCanvasUVTri& triangle,
        const SDK::FVector2D& position0, const SDK::FVector2D& uv0,
        const SDK::FVector2D& position1, const SDK::FVector2D& uv1,
        const SDK::FVector2D& position2, const SDK::FVector2D& uv2) noexcept
    {
        triangle.V0_Pos = position0;
        triangle.V0_UV = uv0;
        triangle.V0_Color = {1.0f, 1.0f, 1.0f, 1.0f};
        triangle.V1_Pos = position1;
        triangle.V1_UV = uv1;
        triangle.V1_Color = {1.0f, 1.0f, 1.0f, 1.0f};
        triangle.V2_Pos = position2;
        triangle.V2_UV = uv2;
        triangle.V2_Color = {1.0f, 1.0f, 1.0f, 1.0f};
    }

    /** Releases an instance before its input and target, keeping failed lease records intact. */
    bool BuildingMapTarget::ResetResources(ResourceSet& resources) noexcept
    {
        if (!resources.MaterialInstance.Reset())
        {
            return false;
        }
        if (!resources.InputTexture.Reset())
        {
            return false;
        }
        return resources.RenderTarget.Reset();
    }

    /** Moves successfully composed candidate leases into empty current slots. */
    void BuildingMapTarget::AdoptResources(ResourceSet& resources) noexcept
    {
        current_.InputTexture = std::move(resources.InputTexture);
        current_.MaterialInstance = std::move(resources.MaterialInstance);
        current_.RenderTarget = std::move(resources.RenderTarget);
    }

    /** Builds, composes, and commits a changed selection without discarding the prior target. */
    void BuildingMapTarget::BuildSelectedImage(SDK::UObject* worldContext,
        const SectionImage& image, const char* selectionReason)
    {
        status_ = Status::Failure;
        if (image.Revision == 0 || image.Width == 0 || image.Height == 0)
        {
            status_ = Status::Failure;
            return;
        }
        ResourceSet candidate{};
        auto* input = CreateInputTexture(worldContext, image, candidate)
            ? static_cast<SDK::UTexture2D*>(candidate.InputTexture.Get())
            : nullptr;
        const bool targetCreated = input && CreateRenderTarget(worldContext, image, candidate);
        const bool materialCreated = targetCreated &&
            CreateCompositionMaterial(worldContext, input, candidate);
        const bool composed = materialCreated && ComposeSection(worldContext,
            static_cast<int32_t>(image.Width), static_cast<int32_t>(image.Height), candidate);
        if (!composed)
        {
            if (!ResetResources(candidate))
            {
                PreservePendingResources(candidate);
            }
            if (status_ != Status::ResourcesUnavailable)
            {
                status_ = Status::Failure;
            }
            return;
        }
        if (!ResetResources(current_))
        {
            PreservePendingResources(candidate);
            pendingInfo_.GridX = image.GridX;
            pendingInfo_.GridY = image.GridY;
            pendingInfo_.XMapUnits = image.XMapUnits;
            pendingInfo_.YMapUnits = image.YMapUnits;
            pendingInfo_.SectionSizeMapUnits = image.SectionSizeMapUnits;
            pendingInfo_.Width = static_cast<int32_t>(image.Width);
            pendingInfo_.Height = static_cast<int32_t>(image.Height);
            pendingRevision_ = image.Revision;
            pendingReady_ = true;
            pendingSelectionReason_ = selectionReason;
            status_ = Status::Failure;
            return;
        }
        AdoptResources(candidate);
        SetSelectedImage(image, selectionReason);
    }

    /** Reports whether pending resources match a selected coordinate and revision. */
    bool BuildingMapTarget::MatchesSelectedImage(const SectionImage& image) const noexcept
    {
        return pendingReady_ && pendingRevision_ == image.Revision &&
            pendingInfo_.GridX == image.GridX && pendingInfo_.GridY == image.GridY;
    }

    /** Retries cleanup of stale or partially-created pending objects. */
    bool BuildingMapTarget::ClearStalePendingResources(const SectionImage& image) noexcept
    {
        if (pendingReady_ && MatchesSelectedImage(image))
        {
            return true;
        }
        if (!ResetResources(pending_))
        {
            return false;
        }
        pendingReady_ = false;
        pendingInfo_ = {};
        pendingRevision_ = 0;
        pendingSelectionReason_ = nullptr;
        return true;
    }

    /** Promotes a previously composed pending target after old leases release. */
    void BuildingMapTarget::PromotePendingResources(const SectionImage& image) noexcept
    {
        if (!ResetResources(current_))
        {
            status_ = Status::Failure;
            return;
        }
        AdoptResources(pending_);
        pendingReady_ = false;
        pendingInfo_ = {};
        pendingRevision_ = 0;
        const char* selectionReason = pendingSelectionReason_;
        SetSelectedImage(image, selectionReason);
        pendingSelectionReason_ = nullptr;
    }

    /** Reports whether current retained content already represents this revision. */
    bool BuildingMapTarget::IsCurrentImage(const SectionImage& image) const noexcept
    {
        return current_.RenderTarget.Get() && revision_ == image.Revision &&
            targetInfo_.GridX == image.GridX && targetInfo_.GridY == image.GridY;
    }

    /** Updates the CPU grid model only when visible geometry or session epoch changed. */
    bool BuildingMapTarget::UpdateGridPlan(const MainMapBuildingLayer::WorldRect& visibleWorld,
        std::uint64_t sessionEpoch) noexcept
    {
        if (hasGridPlanInput_ && sessionEpoch == lastGridPlanEpoch_ &&
            SameGridPlanInput(visibleWorld, lastGridPlanWorld_)) return gridPlanInputValid_;
        lastGridPlanWorld_ = visibleWorld;
        lastGridPlanEpoch_ = sessionEpoch;
        hasGridPlanInput_ = true;
        const BuildingMapGridPlanner::WorldRectangle rectangle{
            visibleWorld.minX, visibleWorld.minY, visibleWorld.maxX, visibleWorld.maxY};
        BuildingMapGridPlanner::PlanFailure failure{};
        gridPlanInputValid_ = gridPlanner_.TryUpdate(rectangle, sessionEpoch, failure);
        if (!gridPlanInputValid_)
        {
            InvalidateGridPlanState();
            return false;
        }
        return gridPlanInputValid_;
    }

    /** Clears publishable plan/source state while preserving GPU leases until presentation withdraws. */
    void BuildingMapTarget::InvalidateGridPlanState() noexcept
    {
        hasGridPlanInput_ = false;
        gridPlanInputValid_ = false;
        gridPlanner_.RetireAllAssignments();
        compositeSources_.clear();
        sourceSnapshotValid_ = false;
    }

    /** Retires assignments and source state so invalid geometry cannot preserve publishable content. */
    bool BuildingMapTarget::InvalidateGridPlanInput() noexcept
    {
        InvalidateGridPlanState();
        return RetireGridResources();
    }

    /** Reconciles only complete provider snapshots and leaves failed observations uncertain. */
    void BuildingMapTarget::ReconcileGridSources(const BuildingMapRemote& remote,
        std::uint64_t sessionEpoch) noexcept
    {
        if (!gridPlanInputValid_ || !gridPlanner_.CurrentPlan().IsValid) return;
        if (remote.GetPollState() != BuildingMapRemote::PollState::Complete)
        {
            MarkAllAssignedGridSlotsDirty();
            sourceSnapshotValid_ = false;
            return;
        }
        if (!NeedsGridSourceRebuild(remote, sessionEpoch)) return;
        std::vector<CompositeSectionSource> nextSources;
        if (!BuildGridSourceSnapshot(remote, nextSources))
        {
            MarkAllAssignedGridSlotsDirty();
            sourceSnapshotValid_ = false;
            return;
        }
        if (sourceSnapshotValid_)
        {
            MarkGridSourceDifferences(nextSources);
        }
        else
        {
            MarkAllAssignedGridSlotsDirty();
        }
        CommitGridSourceSnapshot(remote, sessionEpoch, nextSources);
    }

    /** Imports accepted available sources once per coordinate and composes dirty assigned tiles. */
    void BuildingMapTarget::PrepareGridComposites(SDK::UObject* worldContext,
        const BuildingMapRemote& remote, std::uint64_t sessionEpoch,
        bool materialResourcesReady) noexcept
    {
#if defined(_DEBUG)
        gridDiagnosticBatch_ = {};
#endif
        if (!MainMapNative::CanOperate() || !MainMapNative::IsGameThread() ||
            !worldContext || !sessionEpoch) return;
        if (!RetireTargetsOutsideSession(sessionEpoch))
        {
#if defined(_DEBUG)
            EmitGridDiagnosticSummary(&remote, sessionEpoch,
                "prior-session-target-release-deferred", false);
#endif
            return;
        }
        if (!ValidateGridPreparationSnapshot(remote, sessionEpoch, materialResourcesReady)) return;
        std::array<std::vector<const CompositeSectionSource*>, BuildingMapGridPlanner::kPhysicalSlotCount>
            sourcesBySlot{};
        std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount> slotSourcesReady{};
        slotSourcesReady.fill(true);
        if (!GroupCompositeSources(sourcesBySlot))
        {
#if defined(_DEBUG)
            ++gridDiagnosticBatch_.SnapshotFailures;
            EmitGridDiagnosticSummary(&remote, sessionEpoch, "source-grouping-failed", false);
#endif
            return;
        }
        SynchronizeGridSourceSet(worldContext, remote, slotSourcesReady);
        ComposeDirtyGridSlots(worldContext, remote, sourcesBySlot, slotSourcesReady);
        RetireDepartedSourceTextures();
#if defined(_DEBUG)
        EmitGridDiagnosticSummary(&remote, sessionEpoch, "preparation", false);
#endif
    }

    /** Rejects incomplete source state before any assignment can be composed or committed. */
    bool BuildingMapTarget::ValidateGridPreparationSnapshot(const BuildingMapRemote& remote,
        std::uint64_t sessionEpoch, bool materialResourcesReady) noexcept
    {
        if (remote.GetPollState() != BuildingMapRemote::PollState::Complete)
        {
#if defined(_DEBUG)
            EmitGridDiagnosticSummary(&remote, sessionEpoch, "poll-incomplete", false);
#endif
            return false;
        }
        if (!sourceSnapshotValid_ || sourceSnapshotEpoch_ != sessionEpoch ||
            lastSourceObservationRevision_ != remote.GetSourceObservationRevision())
        {
#if defined(_DEBUG)
            ++gridDiagnosticBatch_.SnapshotFailures;
            EmitGridDiagnosticSummary(&remote, sessionEpoch, "snapshot-not-current", false);
#endif
            return false;
        }
        if (materialResourcesReady) return true;
        RetireDepartedSourceTextures();
#if defined(_DEBUG)
        EmitGridDiagnosticSummary(&remote, sessionEpoch, "material-container-unavailable", false);
#endif
        return false;
    }

#if defined(_DEBUG)
    /** Emits changed source, assignment, resource, and poll state after duplicate suppression. */
    void BuildingMapTarget::EmitGridDiagnosticSummary(const BuildingMapRemote* remote,
        std::uint64_t sessionEpoch, const char* event, bool force) noexcept
    {
        std::uint32_t assignedSlots = 0;
        std::uint32_t dirtySlots = 0;
        const bool stateChanged = UpdateGridDiagnosticState(remote, sessionEpoch,
            assignedSlots, dirtySlots);
        if (!force && !stateChanged && !HasGridDiagnosticActivity()) return;
        const char* pollName = "unavailable";
        if (remote && remote->GetPollState() == BuildingMapRemote::PollState::Incomplete)
        {
            pollName = "incomplete";
        }
        if (remote && remote->GetPollState() == BuildingMapRemote::PollState::Complete)
        {
            pollName = "complete-visible-demand";
        }
        std::uint32_t availableSources = 0;
        std::uint32_t noImageSources = 0;
        for (const CompositeSectionSource& source : compositeSources_)
        {
            if (source.HasImage) ++availableSources;
            else ++noImageSources;
        }
        std::uint32_t retainedTargets = 0;
        for (const CompositeSlotResources& resources : compositeTargets_)
        {
            if (resources.Resources.RenderTarget.Get()) ++retainedTargets;
        }
        std::uint32_t retainedSourceTextures = 0;
        std::uint32_t retainedSourceMaterials = 0;
        for (const auto& entry : sourceTextures_)
        {
            if (entry.second.Texture.Get()) ++retainedSourceTextures;
            if (entry.second.PendingTexture.Get()) ++retainedSourceTextures;
            if (entry.second.MaterialInstance.Get()) ++retainedSourceMaterials;
            if (entry.second.PendingMaterialInstance.Get()) ++retainedSourceMaterials;
        }
        LogGridDiagnosticSummary(remote, sessionEpoch, event, assignedSlots, dirtySlots,
            availableSources, noImageSources, retainedTargets, retainedSourceTextures,
            retainedSourceMaterials);
    }

    /** Writes diagnostic counters with visible-demand and CPU-submission terminology. */
    void BuildingMapTarget::LogGridDiagnosticSummary(const BuildingMapRemote* remote,
        std::uint64_t sessionEpoch, const char* event, std::uint32_t assignedSlots,
        std::uint32_t dirtySlots, std::uint32_t availableSources,
        std::uint32_t noImageSources, std::uint32_t retainedTargets,
        std::uint32_t retainedSourceTextures, std::uint32_t retainedSourceMaterials) noexcept
    {
        const std::int64_t groupingN = gridPlanner_.CurrentPlan().GroupingN;
        const char* pollName = "unavailable";
        if (remote && remote->GetPollState() == BuildingMapRemote::PollState::Incomplete)
        {
            pollName = "incomplete";
        }
        if (remote && remote->GetPollState() == BuildingMapRemote::PollState::Complete)
        {
            pollName = "complete-visible-demand";
        }
        LOG_INFO("MainMap grid prep: event=%s epoch=%llu N=%lld assigned=%u dirty=%u poll=%s snapshot=%s available=%u no-image-observed=%u imported=%u reused=%u replaced=%u source-retired=%u target-created=%u target-reused=%u target-retired=%u canvas-submitted-slots=%u section-draw-calls=%u cpu-commits=%u snapshot-fail=%u import-fail=%u resource-fail=%u canvas-fail=%u composition-fail=%u release-deferred=%u release-retried=%u source-records=%llu source-textures=%u source-materials=%u composite-targets=%u first-failed-source=%lld,%lld first-release-source=%lld,%lld",
            event ? event : "state", static_cast<unsigned long long>(sessionEpoch),
            static_cast<long long>(groupingN), assignedSlots, dirtySlots, pollName,
            sourceSnapshotValid_ ? "accepted-demand" : "invalid", availableSources, noImageSources,
            gridDiagnosticBatch_.ImportedSources, gridDiagnosticBatch_.ReusedSources,
            gridDiagnosticBatch_.ReplacedSources, gridDiagnosticBatch_.RetiredSources,
            gridDiagnosticBatch_.CreatedTargets, gridDiagnosticBatch_.ReusedTargets,
            gridDiagnosticBatch_.RetiredTargets,
            gridDiagnosticBatch_.ComposedSlots, gridDiagnosticBatch_.DrawSubmissions,
            gridDiagnosticBatch_.CpuCommits, gridDiagnosticBatch_.SnapshotFailures,
            gridDiagnosticBatch_.ImportFailures, gridDiagnosticBatch_.ResourceFailures,
            gridDiagnosticBatch_.CanvasFailures, gridDiagnosticBatch_.CompositionFailures,
            gridDiagnosticBatch_.ReleaseDeferrals, gridDiagnosticBatch_.SuccessfulReleaseRetries,
            static_cast<unsigned long long>(sourceTextures_.size()), retainedSourceTextures,
            retainedSourceMaterials, retainedTargets,
            static_cast<long long>(gridDiagnosticBatch_.FirstFailureGridX),
            static_cast<long long>(gridDiagnosticBatch_.FirstFailureGridY),
            static_cast<long long>(gridDiagnosticBatch_.FirstReleaseGridX),
            static_cast<long long>(gridDiagnosticBatch_.FirstReleaseGridY));
    }

    /** Compares and stores poll, source, session, grouping, and six assignment states. */
    bool BuildingMapTarget::UpdateGridDiagnosticState(const BuildingMapRemote* remote,
        std::uint64_t sessionEpoch, std::uint32_t& assignedSlots,
        std::uint32_t& dirtySlots) noexcept
    {
        const auto pollState = remote ? remote->GetPollState() : BuildingMapRemote::PollState::Unavailable;
        const std::uint64_t observationRevision = remote
            ? remote->GetSourceObservationRevision() : lastSourceObservationRevision_;
        const std::int64_t groupingN = gridPlanner_.CurrentPlan().GroupingN;
        bool changed = !hasLastDiagnosticState_ || lastDiagnosticEpoch_ != sessionEpoch ||
            lastDiagnosticObservationRevision_ != observationRevision ||
            lastDiagnosticGroupingN_ != groupingN || lastDiagnosticPollState_ != pollState ||
            lastDiagnosticSnapshotValid_ != sourceSnapshotValid_;
        const auto& assignments = gridPlanner_.Assignments();
        for (std::size_t index = 0; index < assignments.size(); ++index)
        {
            const auto& assignment = assignments[index];
            const std::uint8_t state = (assignment.IsAssigned ? 1u : 0u) |
                (assignment.IsDirty ? 2u : 0u) | (assignment.HasCommittedContent ? 4u : 0u);
            if (lastDiagnosticAssignmentGenerations_[index] != assignment.AssignmentGeneration ||
                lastDiagnosticAssignmentStates_[index] != state) changed = true;
            lastDiagnosticAssignmentGenerations_[index] = assignment.AssignmentGeneration;
            lastDiagnosticAssignmentStates_[index] = state;
            if (assignment.IsAssigned) ++assignedSlots;
            if (assignment.IsAssigned && assignment.IsDirty) ++dirtySlots;
        }
        lastDiagnosticEpoch_ = sessionEpoch;
        lastDiagnosticObservationRevision_ = observationRevision;
        lastDiagnosticGroupingN_ = groupingN;
        lastDiagnosticPollState_ = pollState;
        lastDiagnosticSnapshotValid_ = sourceSnapshotValid_;
        hasLastDiagnosticState_ = true;
        return changed;
    }

    /** Detects new operation counters while ignoring unchanged per-tick texture cache hits. */
    bool BuildingMapTarget::HasGridDiagnosticActivity() const noexcept
    {
        return gridDiagnosticBatch_.ImportedSources != 0 || gridDiagnosticBatch_.ReplacedSources != 0 ||
            gridDiagnosticBatch_.RetiredSources != 0 || gridDiagnosticBatch_.CreatedTargets != 0 ||
            gridDiagnosticBatch_.ReusedTargets != 0 ||
            gridDiagnosticBatch_.RetiredTargets != 0 || gridDiagnosticBatch_.ComposedSlots != 0 ||
            gridDiagnosticBatch_.CpuCommits != 0 || gridDiagnosticBatch_.SnapshotFailures != 0 ||
            gridDiagnosticBatch_.ImportFailures != 0 || gridDiagnosticBatch_.ResourceFailures != 0 ||
            gridDiagnosticBatch_.CanvasFailures != 0 || gridDiagnosticBatch_.CompositionFailures != 0 ||
            gridDiagnosticBatch_.ReleaseDeferrals != 0 ||
            gridDiagnosticBatch_.SuccessfulReleaseRetries != 0;
    }

    /** Emits one dirty composite result with its slot, tile, assignment, and CPU content identity. */
    void BuildingMapTarget::LogGridCompositeResult(
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        std::uint32_t drawSubmissions, bool submitted, bool committed,
        const char* result) noexcept
    {
        LOG_INFO("MainMap grid composite: result=%s epoch=%llu slot=%u N=%lld tile=%lld,%lld assignment=%llu section-draw-calls=%u canvas-submitted=%u cpu-committed=%u content-generation=%llu",
            result ? result : "unknown", static_cast<unsigned long long>(assignment.SessionEpoch),
            assignment.PhysicalSlotIndex, static_cast<long long>(assignment.Tile.GroupingN),
            static_cast<long long>(assignment.Tile.X), static_cast<long long>(assignment.Tile.Y),
            static_cast<unsigned long long>(assignment.AssignmentGeneration), drawSubmissions,
            submitted ? 1u : 0u, committed ? 1u : 0u,
            static_cast<unsigned long long>(assignment.CommittedContentGeneration));
    }

    /** Logs one physical target identity transition without equating it to a content commit. */
    void BuildingMapTarget::LogGridTargetTransition(std::size_t slotIndex, const char* event,
        SDK::UObject* target, const BuildingMapGridPlanner::TileIdentity& oldTile,
        std::uint64_t oldSessionEpoch, std::uint64_t oldAssignmentGeneration,
        const BuildingMapGridPlanner::SlotAssignment& newAssignment,
        std::uint64_t committedContentGeneration) noexcept
    {
        LOG_INFO("MainMap grid target: event=%s slot=%u target=%p old-epoch=%llu old=(N=%lld,tile=%lld,%lld,assignment=%llu) new-epoch=%llu new=(N=%lld,tile=%lld,%lld,assignment=%llu) committed-content=%llu",
            event ? event : "state", static_cast<unsigned int>(slotIndex), static_cast<void*>(target),
            static_cast<unsigned long long>(oldSessionEpoch),
            static_cast<long long>(oldTile.GroupingN), static_cast<long long>(oldTile.X),
            static_cast<long long>(oldTile.Y),
            static_cast<unsigned long long>(oldAssignmentGeneration),
            static_cast<unsigned long long>(newAssignment.SessionEpoch),
            static_cast<long long>(newAssignment.IsAssigned ? newAssignment.Tile.GroupingN : 0),
            static_cast<long long>(newAssignment.IsAssigned ? newAssignment.Tile.X : 0),
            static_cast<long long>(newAssignment.IsAssigned ? newAssignment.Tile.Y : 0),
            static_cast<unsigned long long>(newAssignment.IsAssigned
                ? newAssignment.AssignmentGeneration : 0),
            static_cast<unsigned long long>(committedContentGeneration));
    }

    /** Logs source changes against the currently assigned composite without certifying pixels. */
    void BuildingMapTarget::LogGridSourceEvent(const CompositeSectionSource& source,
        const char* event) noexcept
    {
        const auto& assignments = gridPlanner_.Assignments();
        const bool hasAssignment = source.PhysicalSlotIndex < assignments.size();
        const auto* assignment = hasAssignment ? &assignments[source.PhysicalSlotIndex] : nullptr;
        LOG_INFO("MainMap grid source: event=%s epoch=%llu slot=%u N=%lld tile=%lld,%lld assignment=%llu section=%lld,%lld revision=%llu",
            event ? event : "state", static_cast<unsigned long long>(source.SessionEpoch),
            source.PhysicalSlotIndex, static_cast<long long>(source.GroupingN),
            static_cast<long long>(assignment && assignment->IsAssigned ? assignment->Tile.X : 0),
            static_cast<long long>(assignment && assignment->IsAssigned ? assignment->Tile.Y : 0),
            static_cast<unsigned long long>(assignment && assignment->IsAssigned
                ? assignment->AssignmentGeneration : 0),
            static_cast<long long>(source.GridX), static_cast<long long>(source.GridY),
            static_cast<unsigned long long>(source.Revision));
    }

    /** Suppresses repeated failure records for one slot assignment and failure class. */
    bool BuildingMapTarget::ShouldLogGridCompositeFailure(CompositeSlotResources& resources,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        std::uint8_t failureKind) noexcept
    {
        if (resources.HasLastFailure &&
            resources.LastFailureAssignmentGeneration == assignment.AssignmentGeneration &&
            resources.LastFailureKind == failureKind) return false;
        resources.LastFailureAssignmentGeneration = assignment.AssignmentGeneration;
        resources.LastFailureKind = failureKind;
        resources.HasLastFailure = true;
        return true;
    }

    /** Retains the first coordinate whose admitted source-lease release was deferred. */
    void BuildingMapTarget::RecordGridSourceReleaseFailure(std::int64_t gridX,
        std::int64_t gridY) noexcept
    {
        if (gridDiagnosticBatch_.HasReleaseSource) return;
        gridDiagnosticBatch_.FirstReleaseGridX = gridX;
        gridDiagnosticBatch_.FirstReleaseGridY = gridY;
        gridDiagnosticBatch_.HasReleaseSource = true;
    }

#endif

    /** Groups accepted available source records once by their assigned physical slot. */
    bool BuildingMapTarget::GroupCompositeSources(
        std::array<std::vector<const CompositeSectionSource*>,
            BuildingMapGridPlanner::kPhysicalSlotCount>& sourcesBySlot) const noexcept
    {
        try
        {
            for (const CompositeSectionSource& source : compositeSources_)
            {
                if (!source.HasImage) continue;
                if (source.PhysicalSlotIndex >= sourcesBySlot.size()) return false;
                sourcesBySlot[source.PhysicalSlotIndex].push_back(&source);
            }
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

    /** Synchronizes all available inputs and blocks only slots whose source import failed. */
    void BuildingMapTarget::SynchronizeGridSourceSet(SDK::UObject* worldContext,
        const BuildingMapRemote& remote,
        std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount>& slotSourcesReady) noexcept
    {
        for (const CompositeSectionSource& source : compositeSources_)
        {
            if (!source.HasImage) continue;
            const auto image = remote.Images().find({source.GridX, source.GridY});
            if (image == remote.Images().end() || !MatchesAcceptedSource(source, image->second) ||
                !SynchronizeSourceTexture(worldContext, source, image->second))
            {
                slotSourcesReady[source.PhysicalSlotIndex] = false;
                MarkGridSourceSlot(source.PhysicalSlotIndex);
            }
        }
    }

    /** Composes ready dirty assignments while leaving failures dirty for a later tick. */
    void BuildingMapTarget::ComposeDirtyGridSlots(SDK::UObject* worldContext,
        const BuildingMapRemote& remote,
        const std::array<std::vector<const CompositeSectionSource*>,
            BuildingMapGridPlanner::kPhysicalSlotCount>& sourcesBySlot,
        const std::array<bool, BuildingMapGridPlanner::kPhysicalSlotCount>& slotSourcesReady)
    {
        const auto& assignments = gridPlanner_.Assignments();
        for (std::size_t slotIndex = 0; slotIndex < assignments.size(); ++slotIndex)
        {
            PrepareDirtyGridSlot(worldContext, remote, assignments[slotIndex],
                sourcesBySlot[slotIndex], slotSourcesReady[slotIndex]);
        }
    }

    /** Prepares one assigned dirty slot after its current accepted inputs have synchronized. */
    void BuildingMapTarget::PrepareDirtyGridSlot(SDK::UObject* worldContext,
        const BuildingMapRemote& remote,
        const BuildingMapGridPlanner::SlotAssignment& assignment,
        const std::vector<const CompositeSectionSource*>& sources, bool slotSourcesReady)
    {
        const std::size_t slotIndex = assignment.PhysicalSlotIndex;
        auto& resources = compositeTargets_[slotIndex];
        if (!assignment.IsAssigned)
        {
            RetireUnassignedGridSlot(resources);
            return;
        }
        if (!assignment.IsDirty && resources.HasAssignment &&
            resources.AssignmentGeneration == assignment.AssignmentGeneration &&
            resources.Resources.RenderTarget.Get()) return;
        if (!assignment.IsDirty) gridPlanner_.MarkDirty(slotIndex, assignment.AssignmentGeneration);
        if (!slotSourcesReady) return;
        if (!EnsureCompositeSlotResources(worldContext, assignment, slotIndex, resources))
        {
#if defined(_DEBUG)
            if (ShouldLogGridCompositeFailure(resources, assignment, 1))
            {
                ++gridDiagnosticBatch_.ResourceFailures;
                LogGridCompositeResult(assignment, 0, false, false, "resource-failed");
            }
#endif
            return;
        }
        const bool submitted = ComposeCompositeTile(worldContext, assignment, sources, remote);
        if (!submitted) return;
#if defined(_DEBUG)
        const bool committed = gridPlanner_.RecordCommittedContent(
            slotIndex, assignment.AssignmentGeneration);
        RecordCompositeSubmission(resources, assignment, submitted, committed);
#else
        gridPlanner_.RecordCommittedContent(slotIndex, assignment.AssignmentGeneration);
#endif
    }

#if defined(_DEBUG)
    /** Commits one target result and emits it once, suppressing unchanged failure retries. */
    void BuildingMapTarget::RecordCompositeSubmission(CompositeSlotResources& resources,
        const BuildingMapGridPlanner::SlotAssignment& assignment, bool submitted, bool committed)
    {
        if (submitted) ++gridDiagnosticBatch_.ComposedSlots;
        if (committed) ++gridDiagnosticBatch_.CpuCommits;
        if (submitted && committed)
        {
            resources.HasLastFailure = false;
            if (resources.HasPendingReuseLog &&
                assignment.PhysicalSlotIndex < gridPlanner_.Assignments().size())
            {
                const auto& committedAssignment =
                    gridPlanner_.Assignments()[assignment.PhysicalSlotIndex];
                auto* target = resources.Resources.RenderTarget.Get();
                LogGridTargetTransition(assignment.PhysicalSlotIndex, "target-reused-and-committed",
                    target, resources.ReuseFromTile, resources.ReuseFromSessionEpoch,
                    resources.ReuseFromAssignmentGeneration, committedAssignment,
                    committedAssignment.CommittedContentGeneration);
                resources.LastReusedAssignmentGeneration = committedAssignment.AssignmentGeneration;
                resources.HasPendingReuseLog = false;
            }
            LogGridCompositeResult(assignment, currentCompositeDrawSubmissions_, true,
                true, "cpu-committed");
            return;
        }
        if (!ShouldLogGridCompositeFailure(resources, assignment,
            currentCompositeFailureKind_ == 0 ? 2 : currentCompositeFailureKind_)) return;
        if (currentCompositeFailureKind_ == 1) ++gridDiagnosticBatch_.ResourceFailures;
        else if (currentCompositeFailureKind_ == 3) ++gridDiagnosticBatch_.CanvasFailures;
        else ++gridDiagnosticBatch_.CompositionFailures;
        LogGridCompositeResult(assignment, currentCompositeDrawSubmissions_, submitted,
            false, "submission-failed");
    }
#endif

    /** Clears a vacant slot's logical assignment while retaining its session target. */
    void BuildingMapTarget::RetireUnassignedGridSlot(CompositeSlotResources& resources)
    {
#if defined(_DEBUG)
        if (resources.HasAssignment)
        {
            resources.ReuseFromTile = resources.Tile;
            resources.ReuseFromSessionEpoch = resources.SessionEpoch;
            resources.ReuseFromAssignmentGeneration = resources.AssignmentGeneration;
        }
#endif
        ClearCompositeSlotAssignment(resources);
    }

    /** Reuses an exact accepted texture or imports and safely adopts a changed source revision. */
    bool BuildingMapTarget::SynchronizeSourceTexture(SDK::UObject* worldContext,
        const CompositeSectionSource& source, const SectionImage& image) noexcept
    {
        const auto key = std::make_pair(source.GridX, source.GridY);
        auto sourceEntry = sourceTextures_.end();
        try
        {
            sourceEntry = sourceTextures_.try_emplace(key).first;
        }
        catch (...)
        {
            return false;
        }
        SourceTextureEntry& entry = sourceEntry->second;
        if (entry.HasAcceptedTexture && entry.Texture.Get() && entry.MaterialInstance.Get() &&
            SameSourceTextureIdentity(entry.Accepted, source))
        {
            entry.Accepted = source;
#if defined(_DEBUG)
            ++gridDiagnosticBatch_.ReusedSources;
#endif
            return true;
        }
        if (entry.HasFailedAttempt && entry.FailedObservationRevision == lastSourceObservationRevision_ &&
            SameSourceTextureIdentity(entry.FailedAttempt, source))
        {
            if (entry.HasPendingTexture && !entry.PendingTextureValidated)
            {
                ResetPendingSourceTexture(source, entry);
            }
            return false;
        }
        entry.HasFailedAttempt = false;
        if (entry.HasPendingTexture && entry.PendingTexture.Get() &&
            entry.PendingMaterialInstance.Get() && entry.PendingTextureValidated &&
            SameSourceTextureIdentity(entry.Pending, source))
        {
            return PromotePendingSourceTexture(source, entry);
        }
        if (entry.HasPendingTexture && !ResetPendingSourceTexture(source, entry)) return false;
        return ImportReplacementSourceTexture(worldContext, source, image, entry);
    }

    /** Promotes validated pending resources only after both accepted leases release successfully. */
    bool BuildingMapTarget::PromotePendingSourceTexture(const CompositeSectionSource& source,
        SourceTextureEntry& entry) noexcept
    {
#if defined(_DEBUG)
        const bool replacingTexture = entry.HasAcceptedTexture;
#endif
        if (!entry.MaterialInstance.Reset() || !entry.Texture.Reset())
        {
#if defined(_DEBUG)
            if (!entry.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                LogGridSourceEvent(source, "replacement-release-deferred");
            }
            entry.LeaseReleaseDeferred = true;
            RecordGridSourceReleaseFailure(source.GridX, source.GridY);
#endif
            return false;
        }
#if defined(_DEBUG)
        if (entry.LeaseReleaseDeferred)
        {
            ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
            entry.LeaseReleaseDeferred = false;
            LogGridSourceEvent(source, "replacement-release-retried");
        }
#endif
        entry.Texture = std::move(entry.PendingTexture);
        entry.MaterialInstance = std::move(entry.PendingMaterialInstance);
        entry.Accepted = entry.Pending;
        entry.HasAcceptedTexture = true;
        entry.HasPendingTexture = false;
        entry.PendingTextureValidated = false;
        entry.Pending = {};
        MarkGridSourceSlot(source.PhysicalSlotIndex);
#if defined(_DEBUG)
        if (replacingTexture) ++gridDiagnosticBatch_.ReplacedSources;
        else ++gridDiagnosticBatch_.ImportedSources;
#endif
        return true;
    }

    /** Clears pending resources when their admitted releases succeed, retaining rejected leases. */
    bool BuildingMapTarget::ResetPendingSourceTexture(const CompositeSectionSource& source,
        SourceTextureEntry& entry) noexcept
    {
        if (!entry.PendingMaterialInstance.Reset() || !entry.PendingTexture.Reset())
        {
#if defined(_DEBUG)
            if (!entry.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                LogGridSourceEvent(source, "pending-release-deferred");
            }
            entry.LeaseReleaseDeferred = true;
            RecordGridSourceReleaseFailure(source.GridX, source.GridY);
#endif
            return false;
        }
#if defined(_DEBUG)
        if (entry.LeaseReleaseDeferred)
        {
            ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
            entry.LeaseReleaseDeferred = false;
            LogGridSourceEvent(source, "pending-release-retried");
        }
#endif
        entry.Pending = {};
        entry.HasPendingTexture = false;
        entry.PendingTextureValidated = false;
        return true;
    }

    /** Imports one candidate and keeps failed-release ownership pending for the next admitted retry. */
    bool BuildingMapTarget::ImportReplacementSourceTexture(SDK::UObject* worldContext,
        const CompositeSectionSource& source, const SectionImage& image,
        SourceTextureEntry& entry) noexcept
    {
        ResourceSet candidate{};
        bool leaseReleaseDeferred = false;
        bool materialFailure = false;
        const bool imported = BuildSourceTextureCandidate(worldContext, source, image, entry,
            candidate, leaseReleaseDeferred, materialFailure);
        if (leaseReleaseDeferred)
        {
#if defined(_DEBUG)
            if (!entry.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                LogGridSourceEvent(source, "material-release-deferred");
            }
            entry.LeaseReleaseDeferred = true;
            RecordGridSourceReleaseFailure(source.GridX, source.GridY);
#endif
            return false;
        }
        if (!imported)
        {
            RecordFailedSourceTextureBuild(source, candidate, entry, materialFailure);
            return false;
        }
        return AdoptSourceTextureCandidate(source, candidate, entry);
    }

    /** Imports the image only after the parent material lease has been admitted successfully. */
    bool BuildingMapTarget::BuildSourceTextureCandidate(SDK::UObject* worldContext,
        const CompositeSectionSource& source, const SectionImage& image,
        SourceTextureEntry& entry, ResourceSet& candidate,
        bool& leaseReleaseDeferred, bool& materialFailure) noexcept
    {
        static_cast<void>(source);
        static_cast<void>(entry);
        leaseReleaseDeferred = false;
        materialFailure = false;
        bool imageImportAttempted = false;
        try
        {
            if (!EnsureCompositionMaterial(false, &leaseReleaseDeferred))
            {
                materialFailure = !leaseReleaseDeferred;
                return false;
            }
#if defined(_DEBUG)
            if (entry.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
                entry.LeaseReleaseDeferred = false;
                LogGridSourceEvent(source, "material-release-retried");
            }
#endif
            imageImportAttempted = true;
            if (!CreateInputTexture(worldContext, image, candidate)) return false;
            const bool materialCreated = CreateCompositionMaterial(worldContext,
                static_cast<SDK::UTexture2D*>(candidate.InputTexture.Get()), candidate, false,
                &leaseReleaseDeferred);
            materialFailure = !materialCreated && !leaseReleaseDeferred;
            return materialCreated;
        }
        catch (...)
        {
            materialFailure = !imageImportAttempted;
            return false;
        }
    }

    /** Records one genuine failed identity and retains acquired candidate ownership for cleanup. */
    void BuildingMapTarget::RecordFailedSourceTextureBuild(const CompositeSectionSource& source,
        ResourceSet& candidate, SourceTextureEntry& entry, bool materialFailure) noexcept
    {
        PreserveFailedSourceImport(source, candidate, entry);
        entry.FailedAttempt = source;
        entry.FailedObservationRevision = lastSourceObservationRevision_;
        entry.HasFailedAttempt = true;
#if defined(_DEBUG)
        if (materialFailure) ++gridDiagnosticBatch_.ResourceFailures;
        else ++gridDiagnosticBatch_.ImportFailures;
        LogGridSourceEvent(source, materialFailure ? "material-failed" : "import-failed");
        if (!gridDiagnosticBatch_.HasSourceFailure)
        {
            gridDiagnosticBatch_.FirstFailureGridX = source.GridX;
            gridDiagnosticBatch_.FirstFailureGridY = source.GridY;
            gridDiagnosticBatch_.HasSourceFailure = true;
        }
#endif
    }

    /** Adopts a validated source pair or retains it pending after a rejected old-pair release. */
    bool BuildingMapTarget::AdoptSourceTextureCandidate(const CompositeSectionSource& source,
        ResourceSet& candidate, SourceTextureEntry& entry) noexcept
    {
#if defined(_DEBUG)
        const bool replacingTexture = entry.HasAcceptedTexture;
#endif
        if (!entry.MaterialInstance.Reset() || !entry.Texture.Reset())
        {
            entry.PendingTexture = std::move(candidate.InputTexture);
            entry.PendingMaterialInstance = std::move(candidate.MaterialInstance);
            entry.Pending = source;
            entry.HasPendingTexture = true;
            entry.PendingTextureValidated = true;
#if defined(_DEBUG)
            if (!entry.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                LogGridSourceEvent(source, "accepted-release-deferred");
            }
            entry.LeaseReleaseDeferred = true;
            RecordGridSourceReleaseFailure(source.GridX, source.GridY);
#endif
            return false;
        }
        entry.Texture = std::move(candidate.InputTexture);
        entry.MaterialInstance = std::move(candidate.MaterialInstance);
        entry.Accepted = source;
        entry.HasAcceptedTexture = true;
        entry.HasPendingTexture = false;
        entry.PendingTextureValidated = false;
        entry.Pending = {};
        entry.FailedAttempt = {};
        entry.HasFailedAttempt = false;
        MarkGridSourceSlot(source.PhysicalSlotIndex);
#if defined(_DEBUG)
        if (entry.LeaseReleaseDeferred)
        {
            ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
            entry.LeaseReleaseDeferred = false;
        }
        if (replacingTexture)
        {
            ++gridDiagnosticBatch_.ReplacedSources;
        }
        else
        {
            ++gridDiagnosticBatch_.ImportedSources;
        }
#endif
        return true;
    }

    /** Retains an acquired but invalid import lease if its admitted release is rejected. */
    void BuildingMapTarget::PreserveFailedSourceImport(const CompositeSectionSource& source,
        ResourceSet& candidate, SourceTextureEntry& entry) noexcept
    {
        if (!candidate.InputTexture.Get()) return;
        entry.PendingTexture = std::move(candidate.InputTexture);
        entry.PendingMaterialInstance = std::move(candidate.MaterialInstance);
        entry.Pending = source;
        entry.HasPendingTexture = true;
        entry.PendingTextureValidated = false;
    }

    /** Confirms the remote image metadata matches the completed Stage 3D-B source record. */
    bool BuildingMapTarget::MatchesAcceptedSource(const CompositeSectionSource& source,
        const SectionImage& image) noexcept
    {
        return source.HasImage && source.GridX == image.GridX && source.GridY == image.GridY &&
            source.XMapUnits == image.XMapUnits && source.YMapUnits == image.YMapUnits &&
            source.SectionSizeMapUnits == image.SectionSizeMapUnits && source.Width == image.Width &&
            source.Height == image.Height && source.RowStride == image.RowStride &&
            source.ByteCount == image.ByteCount && source.Revision == image.Revision;
    }

    /** Reuses one texture only when its accepted revision, world metadata and session still match. */
    bool BuildingMapTarget::SameSourceTextureIdentity(const CompositeSectionSource& first,
        const CompositeSectionSource& second) noexcept
    {
        return first.HasImage && second.HasImage && first.GridX == second.GridX &&
            first.GridY == second.GridY && first.SessionEpoch == second.SessionEpoch &&
            first.XMapUnits == second.XMapUnits && first.YMapUnits == second.YMapUnits &&
            first.SectionSizeMapUnits == second.SectionSizeMapUnits && first.Width == second.Width &&
            first.Height == second.Height && first.RowStride == second.RowStride &&
            first.ByteCount == second.ByteCount && first.Revision == second.Revision;
    }

    /** Releases pending then accepted source materials/textures while preserving failed leases. */
    bool BuildingMapTarget::ResetSourceTextureEntry(SourceTextureEntry& source) noexcept
    {
        if (!source.PendingMaterialInstance.Reset()) return false;
        if (!source.PendingTexture.Reset()) return false;
        source.Pending = {};
        source.HasPendingTexture = false;
        source.PendingTextureValidated = false;
        if (!source.MaterialInstance.Reset()) return false;
        if (!source.Texture.Reset()) return false;
        source.Accepted = {};
        source.FailedAttempt = {};
        source.FailedObservationRevision = 0;
        source.HasAcceptedTexture = false;
        source.HasFailedAttempt = false;
        return true;
    }

    /** Clears only a logical tile assignment while leaving the physical target in its session slot. */
    void BuildingMapTarget::ClearCompositeSlotAssignment(CompositeSlotResources& resources) noexcept
    {
        resources.Tile = {};
        resources.Bounds = {};
        resources.SessionEpoch = 0;
        resources.AssignmentGeneration = 0;
        resources.HasAssignment = false;
#if defined(_DEBUG)
        resources.HasPendingReuseLog = false;
#endif
    }

    /** Retires one physical target and clears its identities only after admitted releases succeed. */
    bool BuildingMapTarget::RetireCompositeSlotResources(CompositeSlotResources& resources) noexcept
    {
        if (!ResetResources(resources.Resources)) return false;
        ClearCompositeSlotAssignment(resources);
        resources.ResourceSessionEpoch = 0;
        resources.TargetResourceInvalid = false;
#if defined(_DEBUG)
        resources.HasPendingReuseLog = false;
        resources.ReuseFromTile = {};
        resources.ReuseFromSessionEpoch = 0;
        resources.ReuseFromAssignmentGeneration = 0;
#endif
        return true;
    }

    /** Associates the retained physical target with its current tile and assignment generation. */
    void BuildingMapTarget::AssignCompositeSlot(CompositeSlotResources& destination,
        const BuildingMapGridPlanner::SlotAssignment& assignment) noexcept
    {
        destination.Tile = assignment.Tile;
        destination.Bounds = assignment.Bounds;
        destination.SessionEpoch = assignment.SessionEpoch;
        destination.AssignmentGeneration = assignment.AssignmentGeneration;
        destination.HasAssignment = true;
    }

    /** Checks immutable target allocation properties before reassignment or publication. */
    bool BuildingMapTarget::IsCompositeTargetAllocationValid(
        const SDK::UTextureRenderTarget2D* target) noexcept
    {
        return target && target->IsA(SDK::UTextureRenderTarget2D::StaticClass()) &&
            target->SizeX == kCompositeTargetSize && target->SizeY == kCompositeTargetSize &&
            target->RenderTargetFormat == SDK::ETextureRenderTargetFormat::RTF_RGBA16f &&
            target->bForceLinearGamma != 0 && target->bAutoGenerateMips == 0 &&
            target->ClearColor.R == 0.0f && target->ClearColor.G == 0.0f &&
            target->ClearColor.B == 0.0f && target->ClearColor.A == 1.0f;
    }

    /** Retires cached textures outside the current successfully accepted demand and assignments. */
    void BuildingMapTarget::RetireDepartedSourceTextures() noexcept
    {
        for (auto texture = sourceTextures_.begin(); texture != sourceTextures_.end();)
        {
            const auto source = std::lower_bound(compositeSources_.begin(), compositeSources_.end(),
                texture->first, [](const CompositeSectionSource& candidate,
                    const BuildingMapRemote::SectionCoordinate& coordinate)
                {
                    return candidate.GridX < coordinate.first ||
                        (candidate.GridX == coordinate.first && candidate.GridY < coordinate.second);
                });
            if (source != compositeSources_.end() && source->GridX == texture->first.first &&
                source->GridY == texture->first.second && source->HasImage &&
                source->SessionEpoch == sourceSnapshotEpoch_)
            {
                ++texture;
                continue;
            }
            texture->second.HasAcceptedTexture = false;
            if (ResetSourceTextureEntry(texture->second))
            {
#if defined(_DEBUG)
                if (texture->second.LeaseReleaseDeferred)
                {
                    ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
                }
                ++gridDiagnosticBatch_.RetiredSources;
#endif
                texture = sourceTextures_.erase(texture);
            }
            else
            {
#if defined(_DEBUG)
                if (!texture->second.LeaseReleaseDeferred) ++gridDiagnosticBatch_.ReleaseDeferrals;
                texture->second.LeaseReleaseDeferred = true;
                RecordGridSourceReleaseFailure(texture->first.first, texture->first.second);
#endif
                ++texture;
            }
        }
    }

    /** Releases all source and composite leases while retaining any entry whose release is rejected. */
    bool BuildingMapTarget::RetireGridResources() noexcept
    {
#if defined(_DEBUG)
        gridDiagnosticBatch_ = {};
#endif
        bool released = RetireCompositeGridTargets();
        if (!RetireSourceGridTextures()) released = false;
#if defined(_DEBUG)
        EmitGridDiagnosticSummary(nullptr, lastGridPlanEpoch_, "cleanup", false);
#endif
        return released;
    }

    /** Releases the bounded physical target pool and retains rejected leases for admitted retry. */
    bool BuildingMapTarget::RetireCompositeGridTargets() noexcept
    {
        bool released = true;
        for (std::size_t slotIndex = 0; slotIndex < compositeTargets_.size(); ++slotIndex)
        {
            if (!RetireCompositeTargetSlot(slotIndex, compositeTargets_[slotIndex])) released = false;
        }
        return released;
    }

    /** Retires every retained target whose physical lifetime belongs to an older terrain epoch. */
    bool BuildingMapTarget::RetireTargetsOutsideSession(std::uint64_t sessionEpoch) noexcept
    {
        bool released = true;
        for (std::size_t slotIndex = 0; slotIndex < compositeTargets_.size(); ++slotIndex)
        {
            auto& resources = compositeTargets_[slotIndex];
            if (!resources.Resources.RenderTarget.Get() ||
                resources.ResourceSessionEpoch == sessionEpoch) continue;
            if (!RetireCompositeTargetSlot(slotIndex, resources)) released = false;
        }
        return released;
    }

    /** Retires one physical target while preserving every rejected ownership record. */
    bool BuildingMapTarget::RetireCompositeTargetSlot(std::size_t slotIndex,
        CompositeSlotResources& resources) noexcept
    {
#if defined(_DEBUG)
        auto* target = resources.Resources.RenderTarget.Get();
        const bool hadTarget = target != nullptr;
        BuildingMapGridPlanner::SlotAssignment oldAssignment{};
        oldAssignment.PhysicalSlotIndex = static_cast<std::uint32_t>(slotIndex);
        oldAssignment.IsAssigned = resources.HasAssignment;
        oldAssignment.Tile = resources.Tile;
        oldAssignment.SessionEpoch = resources.SessionEpoch;
        oldAssignment.AssignmentGeneration = resources.AssignmentGeneration;
        const bool releaseWasDeferred = resources.LeaseReleaseDeferred;
#endif
        if (!RetireCompositeSlotResources(resources))
        {
#if defined(_DEBUG)
            if (!resources.LeaseReleaseDeferred)
            {
                ++gridDiagnosticBatch_.ReleaseDeferrals;
                ++gridDiagnosticBatch_.ResourceFailures;
                LogGridTargetTransition(slotIndex, "target-retirement-deferred", target,
                    oldAssignment.Tile, oldAssignment.SessionEpoch,
                    oldAssignment.AssignmentGeneration, {}, 0);
            }
            resources.LeaseReleaseDeferred = true;
#endif
            return false;
        }
#if defined(_DEBUG)
        resources.LeaseReleaseDeferred = false;
        if (releaseWasDeferred) ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
        if (hadTarget)
        {
            ++gridDiagnosticBatch_.RetiredTargets;
            LogGridTargetTransition(slotIndex, releaseWasDeferred
                ? "target-retirement-retried" : "target-retired", target,
                oldAssignment.Tile, oldAssignment.SessionEpoch,
                oldAssignment.AssignmentGeneration, {}, 0);
        }
#endif
        return true;
    }

    /** Releases coordinate-keyed source pairs without discarding rejected lease records. */
    bool BuildingMapTarget::RetireSourceGridTextures() noexcept
    {
        bool released = true;
        for (auto texture = sourceTextures_.begin(); texture != sourceTextures_.end();)
        {
            texture->second.HasAcceptedTexture = false;
            if (ResetSourceTextureEntry(texture->second))
            {
#if defined(_DEBUG)
                if (texture->second.LeaseReleaseDeferred)
                {
                    ++gridDiagnosticBatch_.SuccessfulReleaseRetries;
                }
                ++gridDiagnosticBatch_.RetiredSources;
#endif
                texture = sourceTextures_.erase(texture);
            }
            else
            {
#if defined(_DEBUG)
                if (!texture->second.LeaseReleaseDeferred)
                {
                    ++gridDiagnosticBatch_.ReleaseDeferrals;
                    ++gridDiagnosticBatch_.ResourceFailures;
                }
                texture->second.LeaseReleaseDeferred = true;
                RecordGridSourceReleaseFailure(texture->first.first, texture->first.second);
#endif
                ++texture;
                released = false;
            }
        }
        return released;
    }

    /** Marks each currently assigned tile dirty after a failed or incomplete observation. */
    void BuildingMapTarget::MarkAllAssignedGridSlotsDirty() noexcept
    {
        const auto& assignments = gridPlanner_.Assignments();
        for (std::size_t slotIndex = 0; slotIndex < assignments.size(); ++slotIndex)
        {
            if (assignments[slotIndex].IsAssigned)
            {
                gridPlanner_.MarkDirty(slotIndex, assignments[slotIndex].AssignmentGeneration);
            }
        }
    }

    /** Checks all cached identities that determine whether source membership can be reused. */
    bool BuildingMapTarget::NeedsGridSourceRebuild(const BuildingMapRemote& remote,
        std::uint64_t sessionEpoch) const noexcept
    {
        return !sourceSnapshotValid_ ||
            lastSourceObservationRevision_ != remote.GetSourceObservationRevision() ||
            sourceSnapshotEpoch_ != sessionEpoch ||
            sourceSnapshotGroupingN_ != gridPlanner_.CurrentPlan().GroupingN ||
            !SameSourceAssignments();
    }

    /** Commits sorted source metadata with the observation and assignment generations it represents. */
    void BuildingMapTarget::CommitGridSourceSnapshot(const BuildingMapRemote& remote,
        std::uint64_t sessionEpoch, std::vector<CompositeSectionSource>& nextSources) noexcept
    {
        compositeSources_.swap(nextSources);
        lastSourceObservationRevision_ = remote.GetSourceObservationRevision();
        sourceSnapshotEpoch_ = sessionEpoch;
        sourceSnapshotGroupingN_ = gridPlanner_.CurrentPlan().GroupingN;
        for (std::size_t slotIndex = 0; slotIndex < sourceAssignmentGenerations_.size(); ++slotIndex)
        {
            sourceAssignmentGenerations_[slotIndex] =
                gridPlanner_.Assignments()[slotIndex].AssignmentGeneration;
        }
        sourceSnapshotValid_ = true;
    }

    /** Builds a sorted metadata-only snapshot for Sections assigned to the current six tiles. */
    bool BuildingMapTarget::BuildGridSourceSnapshot(const BuildingMapRemote& remote,
        std::vector<CompositeSectionSource>& output) const noexcept
    {
        output.clear();
        try
        {
            output.reserve(remote.Images().size());
            const auto& plan = gridPlanner_.CurrentPlan();
            const auto& assignments = gridPlanner_.Assignments();
            for (const auto& entry : remote.Images())
            {
                uint32_t slotIndex = 0;
                if (!FindAssignedCompositeSlot(entry.first.first, entry.first.second, slotIndex)) continue;
                const SectionImage& image = entry.second;
                const auto& assignment = assignments[slotIndex];
                output.push_back({image.GridX, image.GridY, plan.GroupingN, assignment.SessionEpoch,
                    slotIndex, true, image.XMapUnits, image.YMapUnits, image.SectionSizeMapUnits,
                    image.Width, image.Height, image.RowStride, image.ByteCount, image.Revision});
            }
            for (const auto& coordinate : remote.NoImageSections())
            {
                uint32_t slotIndex = 0;
                if (!FindAssignedCompositeSlot(coordinate.first, coordinate.second, slotIndex)) continue;
                const auto& assignment = assignments[slotIndex];
                output.push_back({coordinate.first, coordinate.second, plan.GroupingN,
                    assignment.SessionEpoch, slotIndex, false});
            }
            std::sort(output.begin(), output.end(), [](const CompositeSectionSource& first,
                const CompositeSectionSource& second)
            {
                return first.GridX < second.GridX ||
                    (first.GridX == second.GridX && first.GridY < second.GridY);
            });
        }
        catch (...)
        {
            output.clear();
            return false;
        }
        return true;
    }

    /** Marks only old or new composite slots whose current demanded source state differs. */
    void BuildingMapTarget::MarkGridSourceDifferences(
        const std::vector<CompositeSectionSource>& nextSources) noexcept
    {
        std::size_t oldIndex = 0;
        std::size_t nextIndex = 0;
        while (oldIndex < compositeSources_.size() || nextIndex < nextSources.size())
        {
            if (oldIndex == compositeSources_.size())
            {
                MarkGridSourceSlot(nextSources[nextIndex++].PhysicalSlotIndex);
                continue;
            }
            if (nextIndex == nextSources.size())
            {
                MarkGridSourceSlot(compositeSources_[oldIndex++].PhysicalSlotIndex);
                continue;
            }
            const auto& oldSource = compositeSources_[oldIndex];
            const auto& nextSource = nextSources[nextIndex];
            if (oldSource.GridX == nextSource.GridX && oldSource.GridY == nextSource.GridY)
            {
                if (!SameGridSource(oldSource, nextSource))
                {
                    MarkGridSourceSlot(oldSource.PhysicalSlotIndex);
                    MarkGridSourceSlot(nextSource.PhysicalSlotIndex);
                }
                ++oldIndex;
                ++nextIndex;
            }
            else if (oldSource.GridX < nextSource.GridX ||
                (oldSource.GridX == nextSource.GridX && oldSource.GridY < nextSource.GridY))
            {
                MarkGridSourceSlot(oldSource.PhysicalSlotIndex);
                ++oldIndex;
            }
            else
            {
                MarkGridSourceSlot(nextSource.PhysicalSlotIndex);
                ++nextIndex;
            }
        }
    }

    /** Marks a physical slot only while its current assignment still exists. */
    void BuildingMapTarget::MarkGridSourceSlot(uint32_t slotIndex) noexcept
    {
        if (slotIndex >= BuildingMapGridPlanner::kPhysicalSlotCount) return;
        const auto& assignment = gridPlanner_.Assignments()[slotIndex];
        if (assignment.IsAssigned)
        {
            gridPlanner_.MarkDirty(slotIndex, assignment.AssignmentGeneration);
        }
    }

    /** Finds the assigned composite containing a Section using mathematical floor division. */
    bool BuildingMapTarget::FindAssignedCompositeSlot(int64_t gridX, int64_t gridY,
        uint32_t& slotIndex) const noexcept
    {
        const auto& plan = gridPlanner_.CurrentPlan();
        if (!plan.IsValid || plan.GroupingN < 4) return false;
        const int64_t tileX = FloorDivideSectionCoordinate(gridX, plan.GroupingN);
        const int64_t tileY = FloorDivideSectionCoordinate(gridY, plan.GroupingN);
        const auto& assignments = gridPlanner_.Assignments();
        for (std::size_t index = 0; index < assignments.size(); ++index)
        {
            const auto& assignment = assignments[index];
            if (assignment.IsAssigned && assignment.SessionEpoch == lastGridPlanEpoch_ &&
                assignment.Tile.GroupingN == plan.GroupingN &&
                assignment.Tile.X == tileX && assignment.Tile.Y == tileY)
            {
                slotIndex = static_cast<uint32_t>(index);
                return true;
            }
        }
        return false;
    }

    /** Compares the complete accepted source identity without revisiting pixel bytes. */
    bool BuildingMapTarget::SameGridSource(const CompositeSectionSource& first,
        const CompositeSectionSource& second) noexcept
    {
        return first.GridX == second.GridX && first.GridY == second.GridY &&
            first.GroupingN == second.GroupingN && first.SessionEpoch == second.SessionEpoch &&
            first.PhysicalSlotIndex == second.PhysicalSlotIndex && first.HasImage == second.HasImage &&
            first.XMapUnits == second.XMapUnits && first.YMapUnits == second.YMapUnits &&
            first.SectionSizeMapUnits == second.SectionSizeMapUnits && first.Width == second.Width &&
            first.Height == second.Height && first.RowStride == second.RowStride &&
            first.ByteCount == second.ByteCount && first.Revision == second.Revision;
    }

    /** Divides signed Section coordinates toward negative infinity for the zero-anchored lattice. */
    int64_t BuildingMapTarget::FloorDivideSectionCoordinate(int64_t coordinate,
        int64_t groupingN) noexcept
    {
        int64_t quotient = coordinate / groupingN;
        const int64_t remainder = coordinate % groupingN;
        if (remainder < 0) --quotient;
        return quotient;
    }

    /** Confirms that cached source-to-slot associations still match all six logical assignments. */
    bool BuildingMapTarget::SameSourceAssignments() const noexcept
    {
        const auto& assignments = gridPlanner_.Assignments();
        for (std::size_t slotIndex = 0; slotIndex < assignments.size(); ++slotIndex)
        {
            if (sourceAssignmentGenerations_[slotIndex] != assignments[slotIndex].AssignmentGeneration)
            {
                return false;
            }
        }
        return true;
    }

    /** Reports validity for the latest cached geometry/session input. */
    bool BuildingMapTarget::HasCurrentGridPlan() const noexcept
    {
        return gridPlanInputValid_;
    }

    /** Compares the last grid input exactly to avoid replanning unchanged ticks. */
    bool BuildingMapTarget::SameGridPlanInput(const MainMapBuildingLayer::WorldRect& first,
        const MainMapBuildingLayer::WorldRect& second) noexcept
    {
        return first.minX == second.minX && first.minY == second.minY &&
            first.maxX == second.maxX && first.maxY == second.maxY;
    }

    /** Retains the pending/current Section when visible, otherwise chooses the largest visible overlap. */
    const SectionImage* BuildingMapTarget::SelectVisibleImage(
        const BuildingMapRemote::ImageCollection& images,
        const MainMapBuildingLayer::WorldRect& visibleWorld, const char*& selectionReason) const noexcept
    {
        selectionReason = "initial visible candidate";
        const bool hasPendingSelection = pendingReady_ && pending_.RenderTarget.Get();
        const bool hasCurrentSelection = !hasPendingSelection && ready_ && current_.RenderTarget.Get();
        if (hasPendingSelection || hasCurrentSelection)
        {
            const int64_t gridX = hasPendingSelection ? pendingInfo_.GridX : targetInfo_.GridX;
            const int64_t gridY = hasPendingSelection ? pendingInfo_.GridY : targetInfo_.GridY;
            const auto previous = images.find({gridX, gridY});
            if (previous == images.end())
            {
                selectionReason = "previous candidate no longer available";
            }
            else
            {
                double intersectionArea = 0.0;
                if (GetIntersectionArea(previous->second, visibleWorld, intersectionArea))
                {
                    selectionReason = hasPendingSelection && pendingSelectionReason_
                        ? pendingSelectionReason_ : "retained existing candidate";
                    return &previous->second;
                }
                selectionReason = "previous candidate outside viewport";
            }
        }

        const SectionImage* bestImage = nullptr;
        double bestIntersectionArea = 0.0;
        for (const auto& entry : images)
        {
            double intersectionArea = 0.0;
            if (!GetIntersectionArea(entry.second, visibleWorld, intersectionArea) ||
                (bestImage && intersectionArea <= bestIntersectionArea)) continue;
            bestImage = &entry.second;
            bestIntersectionArea = intersectionArea;
        }
        return bestImage;
    }

    /** Validates the provider's world bounds and computes their half-open visible intersection area. */
    bool BuildingMapTarget::GetIntersectionArea(const SectionImage& image,
        const MainMapBuildingLayer::WorldRect& visibleWorld, double& area) const noexcept
    {
        area = 0.0;
        if (!std::isfinite(image.XMapUnits) || !std::isfinite(image.YMapUnits) ||
            !std::isfinite(image.SectionSizeMapUnits) || image.SectionSizeMapUnits <= 0.0) return false;
        const double sectionMaxX = image.XMapUnits + image.SectionSizeMapUnits;
        const double sectionMaxY = image.YMapUnits + image.SectionSizeMapUnits;
        if (!std::isfinite(sectionMaxX) || !std::isfinite(sectionMaxY)) return false;
        const double overlapX = (std::min)(sectionMaxX, visibleWorld.maxX) -
            (std::max)(image.XMapUnits, visibleWorld.minX);
        const double overlapY = (std::min)(sectionMaxY, visibleWorld.maxY) -
            (std::max)(image.YMapUnits, visibleWorld.minY);
        if (!std::isfinite(overlapX) || !std::isfinite(overlapY) || overlapX <= 0.0 || overlapY <= 0.0)
            return false;
        area = overlapX * overlapY;
        return std::isfinite(area) && area > 0.0;
    }

    /** Moves every staged lease into the pending owner without losing failed releases. */
    void BuildingMapTarget::PreservePendingResources(ResourceSet& resources) noexcept
    {
        pending_.InputTexture = std::move(resources.InputTexture);
        pending_.MaterialInstance = std::move(resources.MaterialInstance);
        pending_.RenderTarget = std::move(resources.RenderTarget);
    }

    /** Commits the coordinate, revision, and published world bounds paired with the target. */
    void BuildingMapTarget::SetSelectedImage(const SectionImage& image,
        const char* selectionReason) noexcept
    {
        const SectionTargetInfo oldInfo = targetInfo_;
        const bool hadOldTarget = ready_;
        targetInfo_.GridX = image.GridX;
        targetInfo_.GridY = image.GridY;
        targetInfo_.XMapUnits = image.XMapUnits;
        targetInfo_.YMapUnits = image.YMapUnits;
        targetInfo_.SectionSizeMapUnits = image.SectionSizeMapUnits;
        targetInfo_.Width = static_cast<int32_t>(image.Width);
        targetInfo_.Height = static_cast<int32_t>(image.Height);
        targetInfo_.Revision = image.Revision;
        revision_ = image.Revision;
        ready_ = true;
        status_ = Status::Ready;
        if (!firstCompositionLogged_)
        {
            LOG_INFO("MainMap diagnostics: first Section composition succeeded grid=%lld,%lld revision=%llu",
                static_cast<long long>(image.GridX), static_cast<long long>(image.GridY),
                static_cast<unsigned long long>(image.Revision));
            firstCompositionLogged_ = true;
        }
        if (!hadOldTarget || oldInfo.GridX != image.GridX || oldInfo.GridY != image.GridY)
        {
            LOG_INFO("MainMap diagnostics: selected visible Section grid=%lld,%lld reason=%s",
                static_cast<long long>(image.GridX), static_cast<long long>(image.GridY),
                selectionReason ? selectionReason : "selection reason unavailable");
        }
        if (hadOldTarget && (oldInfo.GridX != image.GridX || oldInfo.GridY != image.GridY ||
            oldInfo.Revision != image.Revision))
        {
            LOG_INFO("MainMap diagnostics: composition target changed old=%lld,%lld revision=%llu new=%lld,%lld revision=%llu",
                static_cast<long long>(oldInfo.GridX), static_cast<long long>(oldInfo.GridY),
                static_cast<unsigned long long>(oldInfo.Revision), static_cast<long long>(image.GridX),
                static_cast<long long>(image.GridY), static_cast<unsigned long long>(image.Revision));
        }
    }
}
