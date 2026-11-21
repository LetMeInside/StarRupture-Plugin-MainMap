// Imports a selected MainMap Section image and composes one retained RGBA16f target.
#include "BuildingMapTarget.h"

#include "MaterialProof.h"
#include "Engine_classes.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace MainMapBuildingMap
{
    namespace
    {
        constexpr char kComposeMaterialPath[] =
            "/Game/Mods/MiniMap/Materials/M_BuildingCompose.M_BuildingCompose";
    }

    /** Returns a process-lifetime owner whose leases survive a rejected shutdown release. */
    BuildingMapTarget& BuildingMapTarget::SharedOwner() noexcept
    {
        alignas(BuildingMapTarget) static uint8_t storage[sizeof(BuildingMapTarget)]{};
        static BuildingMapTarget* target = ::new (storage) BuildingMapTarget();
        return *target;
    }

    /** Updates the one lifecycle-owned target instance. */
    void BuildingMapTarget::UpdateShared(SDK::UObject* worldContext,
        const BuildingMapRemote::ImageCollection& images, bool materialResourcesReady)
    {
        SharedOwner().Update(worldContext, images, materialResourcesReady);
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

    /** Reuses an unchanged Section revision or transactionally builds its replacement. */
    void BuildingMapTarget::Update(SDK::UObject* worldContext,
        const BuildingMapRemote::ImageCollection& images, bool materialResourcesReady)
    {
        if (!MainMapNative::CanOperate() || !worldContext)
        {
            status_ = Status::Failure;
            return;
        }
        if (images.empty())
        {
            Retire();
            return;
        }
        const SectionImage& selected = images.begin()->second;
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
        if (pendingReady_ && MatchesSelectedImage(selected))
        {
            PromotePendingResources(selected);
            return;
        }
        if (!ClearStalePendingResources(selected))
        {
            status_ = Status::Failure;
            return;
        }
        if (IsCurrentImage(selected))
        {
            ready_ = true;
            status_ = Status::Ready;
            return;
        }
        BuildSelectedImage(worldContext, selected);
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
        SDK::UTexture2D* inputTexture, ResourceSet& resources)
    {
        if (!materialValidated_)
        {
            if (material_.Get() && !material_.Reset())
            {
                status_ = Status::ResourcesUnavailable;
                return false;
            }
            if (materialLoadAttempted_)
            {
                status_ = Status::ResourcesUnavailable;
                return false;
            }
            materialLoadAttempted_ = true;
            auto* material = MainMapProof::LoadMaterial(kComposeMaterialPath,
                SDK::EMaterialDomain::MD_Surface, material_);
            if (!material)
            {
                material_.Reset();
                status_ = Status::ResourcesUnavailable;
                return false;
            }
            materialValidated_ = true;
        }
        status_ = Status::Failure;
        auto* instance = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(worldContext,
            static_cast<SDK::UMaterialInterface*>(material_.Get()), SDK::FName(),
            SDK::EMIDCreationFlags::Transient);
        if (!instance || !instance->IsA(SDK::UMaterialInstanceDynamic::StaticClass()) ||
            !resources.MaterialInstance.Acquire(instance))
        {
            return false;
        }
        const SDK::FName parameterName = SDK::UKismetStringLibrary::Conv_StringToName(
            SDK::FString(L"SpriteAtlas"));
        instance->SetTextureParameterValue(parameterName, inputTexture);
        return instance->K2_GetTextureParameterValue(parameterName) == inputTexture;
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
        const SectionImage& image)
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
            status_ = Status::Failure;
            return;
        }
        AdoptResources(candidate);
        SetSelectedImage(image);
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
        SetSelectedImage(image);
    }

    /** Reports whether current retained content already represents this revision. */
    bool BuildingMapTarget::IsCurrentImage(const SectionImage& image) const noexcept
    {
        return current_.RenderTarget.Get() && revision_ == image.Revision &&
            targetInfo_.GridX == image.GridX && targetInfo_.GridY == image.GridY;
    }

    /** Moves every staged lease into the pending owner without losing failed releases. */
    void BuildingMapTarget::PreservePendingResources(ResourceSet& resources) noexcept
    {
        pending_.InputTexture = std::move(resources.InputTexture);
        pending_.MaterialInstance = std::move(resources.MaterialInstance);
        pending_.RenderTarget = std::move(resources.RenderTarget);
    }

    /** Commits the coordinate, revision, and published world bounds paired with the target. */
    void BuildingMapTarget::SetSelectedImage(const SectionImage& image) noexcept
    {
        targetInfo_.GridX = image.GridX;
        targetInfo_.GridY = image.GridY;
        targetInfo_.XMapUnits = image.XMapUnits;
        targetInfo_.YMapUnits = image.YMapUnits;
        targetInfo_.SectionSizeMapUnits = image.SectionSizeMapUnits;
        targetInfo_.Width = static_cast<int32_t>(image.Width);
        targetInfo_.Height = static_cast<int32_t>(image.Height);
        revision_ = image.Revision;
        ready_ = true;
        status_ = Status::Ready;
    }
}
