#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"

#include <array>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    // Banded rendering (GSBandedBackend): this instance draws only framebuffer rows whose band
    // ((y >> kBandShift) % count) equals index. count <= 1 draws everything (the default).
    void SetBand(int index, int count);
    static constexpr int kBandShift = 2; // 4-row bands, interleaved across instances

    // Drops the decoded-texture cache. GSBandedBackend calls it on the instances that skip a
    // command which changes VRAM (transfers, uploads, WriteVram and clears run on instance 0 only).
    void InvalidateDecodedTextures();
    // A transfer / upload chunk this instance skips (it runs on instance 0): drop the decoded
    // textures in the blocks it writes.
    void NoteSkippedTransfer(const GSTransferCommand &command);
    void NoteSkippedUpload();
    // Takes the CLUT state of another instance at the same point of the command stream (a band
    // that slept through batches; LoadClut is the only command whose state it would have missed).
    void CopyClutStateFrom(const GSCpuBackend &other);

private:
    bool ownsRow(int y) const
    {
        return m_bandCount <= 1 || (static_cast<int>(static_cast<unsigned>(y) >> kBandShift) % m_bandCount) == m_bandIndex;
    }
    // Smallest y >= y0 with ownsRow(y) (y0 >= 0), and the owned row after an owned row y.
    int firstOwnedRow(int y0) const
    {
        if (m_bandCount <= 1)
            return y0;
        const int band = y0 >> kBandShift;
        const int skip = (m_bandIndex - band % m_bandCount + m_bandCount) % m_bandCount;
        return skip == 0 ? y0 : (band + skip) << kBandShift;
    }
    int nextOwnedRow(int y) const
    {
        ++y;
        if (m_bandCount > 1 && (y & ((1 << kBandShift) - 1)) == 0)
            y += (m_bandCount - 1) << kBandShift;
        return y;
    }
    int m_bandIndex = 0;
    int m_bandCount = 1;

    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    uint32_t ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y);
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t DecodeTexel(const GSDrawState &state, int u, int v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint16_t, 512> m_clut{};
    std::array<uint32_t, 2> m_clutCbp{};
    GSMem::TexturePageCache m_texturePageCache;

    // Whole textures decoded to RGBA8888 (unswizzle + CLUT + TEXA) for triangles, reused across
    // draws until something may have changed their texels or the CLUT. A texture is decoded once
    // the triangles sampling it have taken about half its texel count in samples.
    struct DecodedTextureKey
    {
        uint32_t tbp0 = 0;
        uint16_t width = 0, height = 0;
        uint8_t tbw = 0, psm = 0, cpsm = 0, csm = 0, csa = 0, ta0 = 0, ta1 = 0;
        bool aem = false;
        bool operator==(const DecodedTextureKey &) const = default;
    };
    struct DecodedTexture
    {
        DecodedTextureKey key;
        bool used = false;    // slot holds a key (decoded or still counting samples)
        bool decoded = false; // texels are valid
        uint64_t samples = 0;
        uint64_t lastUse = 0;
        uint64_t blockFirst = 0, blockLast = 0; // GS blocks the texture occupies
        std::vector<uint32_t> texels;           // width * height
    };
    std::array<DecodedTexture, 4> m_decodedTextures{};
    uint64_t m_decodedUseClock = 0;
    bool m_hasDecodedTextures = false;
    bool m_transferWrites = false; // current transfer writes VRAM in [first, last) blocks
    uint64_t m_transferWriteFirst = 0, m_transferWriteLast = 0;
    void InvalidateDecodedTexturesUnlocked();
    void InvalidateDecodedTexturesInBlocks(uint64_t first, uint64_t last);
    void InvalidateDecodedTexturesWrittenBy(const GSPrimitiveBatch &batch);
    void NoteTransferStart(const GSTransferCommand &command);
    void NoteTransferData();
    const uint32_t *DecodedTriangleTexture(const GSDrawState &state, int minY, int maxY, uint64_t samples);

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
};
