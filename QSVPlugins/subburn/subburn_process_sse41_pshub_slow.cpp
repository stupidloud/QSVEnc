﻿//  -----------------------------------------------------------------------------------------
//    QSVEnc by rigaya
//  -----------------------------------------------------------------------------------------
//   ソースコードについて
//   ・無保証です。
//   ・本ソースコードを使用したことによるいかなる損害・トラブルについてrigayaは責任を負いません。
//   以上に了解して頂ける場合、本ソースコードの使用、複製、改変、再頒布を行って頂いて構いません。
//  -----------------------------------------------------------------------------------------

#define USE_SSE2    1
#define USE_SSSE3   1
#define PSHUFB_SLOW 1
#define USE_SSE41   1
#define USE_AVX     0
#define USE_AVX2    0
#define USE_FMA3    0
#define USE_POPCNT  0
#include "subburn_process.h"
#include "subburn_process_simd.h"
#if ENABLE_AVCODEC_QSV_READER && ENABLE_LIBASS_SUBBURN

ProcessorSubBurnSSE41PshufbSlow::ProcessorSubBurnSSE41PshufbSlow() : ProcessorSubBurn() {
}

ProcessorSubBurnSSE41PshufbSlow::~ProcessorSubBurnSSE41PshufbSlow() {
}

void ProcessorSubBurnSSE41PshufbSlow::CopyFrameY() {
    const uint8_t *pFrameSrc = m_pIn->Data.Y;
    uint8_t *pFrameOut = m_pOut->Data.Y;
    const int w = m_pIn->Info.CropW;
    const int h = m_pIn->Info.CropH;
    const int pitch = m_pIn->Data.Pitch;
    for (int y = 0; y < h; y++, pFrameSrc += pitch, pFrameOut += pitch) {
        sse_memcpy(pFrameOut, pFrameSrc, w);
    }
}

void ProcessorSubBurnSSE41PshufbSlow::CopyFrameUV() {
    const uint8_t *pFrameSrc = m_pIn->Data.UV;
    uint8_t *pFrameOut = m_pOut->Data.UV;
    const int w = m_pIn->Info.CropW;
    const int h = m_pIn->Info.CropH;
    const int pitch = m_pIn->Data.Pitch;
    for (int y = 0; y < h; y += 2, pFrameSrc += pitch, pFrameOut += pitch) {
        sse_memcpy(pFrameOut, pFrameSrc, w);
    }
}

#pragma warning(push)
#pragma warning(disable: 4100)
void ProcessorSubBurnSSE41PshufbSlow::BlendSubY(const uint8_t *pAlpha, int bufX, int bufY, int bufW, int bufStride, int bufH, uint8_t subcolory, uint8_t subTransparency, uint8_t *pBuf) {
    uint8_t *pFrame = m_pOut->Data.Y;
    const int w = m_pOut->Info.CropW;
    const int h = m_pOut->Info.CropH;
    const int pitch = m_pOut->Data.Pitch;
    bufW = (std::min)(w, bufX + bufW) - bufX;
    bufH = (std::min)(h, bufY + bufH) - bufY;
    blend_sub<false, false>(pFrame, pitch, pAlpha, bufX, bufY, bufW, bufStride, bufH, subcolory, subcolory, subTransparency, nullptr);
}

void ProcessorSubBurnSSE41PshufbSlow::BlendSubUV(const uint8_t *pAlpha, int bufX, int bufY, int bufW, int bufStride, int bufH, uint8_t subcoloru, uint8_t subcolorv, uint8_t subTransparency, uint8_t *pBuf) {
    uint8_t *pFrame = m_pOut->Data.UV;
    const int w = m_pOut->Info.CropW;
    const int h = m_pOut->Info.CropH;
    const int pitch = m_pOut->Data.Pitch;
    bufW = (std::min)(w, bufX + bufW) - bufX;
    bufH = (std::min)(h, bufY + bufH) - bufY;
    blend_sub<true, false>(pFrame, pitch, pAlpha, bufX, bufY, bufW, bufStride, bufH, subcoloru, subcolorv, subTransparency, nullptr);
}
#pragma warning(pop)

ProcessorSubBurnD3DSSE41PshufbSlow::ProcessorSubBurnD3DSSE41PshufbSlow() : ProcessorSubBurn() {
}

ProcessorSubBurnD3DSSE41PshufbSlow::~ProcessorSubBurnD3DSSE41PshufbSlow() {
}

void ProcessorSubBurnD3DSSE41PshufbSlow::CopyFrameY() {
}

void ProcessorSubBurnD3DSSE41PshufbSlow::CopyFrameUV() {
}

void ProcessorSubBurnD3DSSE41PshufbSlow::BlendSubY(const uint8_t *pAlpha, int bufX, int bufY, int bufW, int bufStride, int bufH, uint8_t subcolory, uint8_t subTransparency, uint8_t *pBuf) {
    uint8_t *pFrame = m_pOut->Data.Y;
    const int w = m_pOut->Info.CropW;
    const int h = m_pOut->Info.CropH;
    const int pitch = m_pOut->Data.Pitch;
    bufW = (std::min)(w, bufX + bufW) - bufX;
    bufH = (std::min)(h, bufY + bufH) - bufY;
    blend_sub<false, true>(pFrame, pitch, pAlpha, bufX, bufY, bufW, bufStride, bufH, subcolory, subcolory, subTransparency, pBuf);
}

void ProcessorSubBurnD3DSSE41PshufbSlow::BlendSubUV(const uint8_t *pAlpha, int bufX, int bufY, int bufW, int bufStride, int bufH, uint8_t subcoloru, uint8_t subcolorv, uint8_t subTransparency, uint8_t *pBuf) {
    uint8_t *pFrame = m_pOut->Data.UV;
    const int w = m_pOut->Info.CropW;
    const int h = m_pOut->Info.CropH;
    const int pitch = m_pOut->Data.Pitch;
    bufW = (std::min)(w, bufX + bufW) - bufX;
    bufH = (std::min)(h, bufY + bufH) - bufY;
    blend_sub<true, true>(pFrame, pitch, pAlpha, bufX, bufY, bufW, bufStride, bufH, subcoloru, subcolorv, subTransparency, pBuf);
}

#endif //#if ENABLE_AVCODEC_QSV_READER && ENABLE_LIBASS_SUBBURN