﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#include <map>
#include <array>
#include "convert_csp.h"
#include "rgy_filter_overlay.h"
#include "rgy_avutil.h"
#include "rgy_util.h"
#include "rgy_aspect_ratio.h"
#include "cpu_info.h"

RGY_ERR RGYFilterOverlay::overlayPlane(RGYFrameInfo *pOutputPlane, const RGYFrameInfo *pInputPlane, const RGYFrameInfo *pOverlay, const RGYFrameInfo *pAlpha, const int posX, const int posY,
    RGYOpenCLQueue& queue, const std::vector<RGYOpenCLEvent>& wait_events, RGYOpenCLEvent *event) {
    auto prm = std::dynamic_pointer_cast<RGYFilterParamOverlay>(m_param);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    {
        const char *kernel_name = "kernel_overlay";
        RGYWorkSize local(32, 8);
        RGYWorkSize global(pOutputPlane->width, pOutputPlane->height);
        auto err = m_overlay.get()->kernel(kernel_name).config(queue, local, global, wait_events, event).launch(
            (cl_mem)pOutputPlane->ptr[0], pOutputPlane->pitch[0],
            (cl_mem)pInputPlane->ptr[0], pInputPlane->pitch[0], pOutputPlane->width, pOutputPlane->height,
            (cl_mem)pOverlay->ptr[0], pOverlay->pitch[0],
            (cl_mem)pAlpha->ptr[0], pAlpha->pitch[0], pOverlay->width, pOverlay->height,
            posX, posY);
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("error at %s (overlayPlane(%s)): %s.\n"),
                char_to_tstring(kernel_name).c_str(), RGY_CSP_NAMES[pInputPlane->csp], get_err_mes(err));
            return err;
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR RGYFilterOverlay::overlayFrame(RGYFrameInfo *pOutputFrame, const RGYFrameInfo *pInputFrame, RGYOpenCLQueue& queue, const std::vector<RGYOpenCLEvent>& wait_events, RGYOpenCLEvent *event) {
    auto prm = std::dynamic_pointer_cast<RGYFilterParamOverlay>(m_param);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    for (int i = 0; i < RGY_CSP_PLANES[pOutputFrame->csp]; i++) {
        const auto planeTarget = (RGY_PLANE)i;
        auto planeDst = getPlane(pOutputFrame, planeTarget);
        auto planeSrc = getPlane(pInputFrame, planeTarget);
        auto planeOverlay = getPlane(m_frame.inputPtr, planeTarget);
        auto planeAlpha = getPlane(m_alpha.inputPtr, planeTarget);
        const std::vector<RGYOpenCLEvent> &plane_wait_event = (i == 0) ? wait_events : std::vector<RGYOpenCLEvent>();
        RGYOpenCLEvent *plane_event = (i == RGY_CSP_PLANES[pOutputFrame->csp] - 1) ? event : nullptr;
        const auto posX = (planeTarget != RGY_PLANE_Y && RGY_CSP_CHROMA_FORMAT[pInputFrame->csp] == RGY_CHROMAFMT_YUV420) ? prm->overlay.posX >> 1 : prm->overlay.posX;
        const auto posY = (planeTarget != RGY_PLANE_Y && RGY_CSP_CHROMA_FORMAT[pInputFrame->csp] == RGY_CHROMAFMT_YUV420) ? prm->overlay.posY >> 1 : prm->overlay.posY;
        auto err = overlayPlane(&planeDst, &planeSrc, &planeOverlay, &planeAlpha, posX, posY, queue, plane_wait_event, plane_event);
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to overlay frame(%d) %s: %s\n"), i, cl_errmes(err));
            return err_cl_to_rgy(err);
        }
    }
    return RGY_ERR_NONE;
}

tstring RGYFilterParamOverlay::print() const {
    return overlay.print();
}

void RGYFilterOverlay::RGYFilterOverlayFrame::close() {
    crop.reset();
    resize.reset();
    dev->clear();
    inputPtr = nullptr;
}

RGYFilterOverlay::RGYFilterOverlay(std::shared_ptr<RGYOpenCLContext> context) :
    RGYFilter(context),
    m_formatCtx(std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)>(nullptr, avformat_free_context)),
    m_codecCtxDec(),
    m_inputFrames(0),
    m_convert(),
    m_inputCsp(RGY_CSP_NA),
    m_stream(nullptr),
    m_frame(),
    m_alpha(),
    m_overlay(),
    m_bInterlacedWarn(false) {
    m_name = _T("overlay");
}

RGYFilterOverlay::~RGYFilterOverlay() {
    close();
}

RGY_ERR RGYFilterOverlay::init(shared_ptr<RGYFilterParam> pParam, shared_ptr<RGYLog> pPrintMes) {
    RGY_ERR sts = RGY_ERR_NONE;
    m_pLog = pPrintMes;
    auto prm = std::dynamic_pointer_cast<RGYFilterParamOverlay>(pParam);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }

    if (RGY_CSP_CHROMA_FORMAT[pParam->frameIn.csp] != RGY_CHROMAFMT_YUV420 && RGY_CSP_CHROMA_FORMAT[pParam->frameIn.csp] != RGY_CHROMAFMT_YUV444) {
        AddMessage(RGY_LOG_ERROR, _T("this filter does not support csp %s.\n"), RGY_CSP_NAMES[pParam->frameIn.csp]);
        return RGY_ERR_UNSUPPORTED;
    }

    //パラメータチェック
    if (prm->frameOut.height <= 0 || prm->frameOut.width <= 0) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (prm->overlay.width < 0 || prm->frameOut.height < 0) {
        AddMessage(RGY_LOG_ERROR, _T("width/height must be a positive value.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (prm->overlay.alpha < 0.0 || 1.0 < prm->overlay.alpha) {
        AddMessage(RGY_LOG_ERROR, _T("alpha should be 0.0 - 1.0.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (!m_param
        || std::dynamic_pointer_cast<RGYFilterParamOverlay>(m_param)->overlay != prm->overlay) {
        auto options = strsprintf("-D Type=%s -D bit_depth=%d",
            RGY_CSP_BIT_DEPTH[prm->frameOut.csp] > 8 ? "ushort" : "uchar",
            RGY_CSP_BIT_DEPTH[prm->frameOut.csp]);
        m_overlay.set(m_cl->buildResourceAsync(_T("RGY_FILTER_OVERLAY_CL"), _T("EXE_DATA"), options.c_str()));
    }

    sts = initInput(prm.get());
    if (sts != RGY_ERR_NONE) {
        return sts;
    }

    auto err = AllocFrameBuf(prm->frameOut, 1);
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), get_err_mes(err));
        return RGY_ERR_MEMORY_ALLOC;
    }
    for (int i = 0; i < RGY_CSP_PLANES[m_frameBuf[0]->frame.csp]; i++) {
        prm->frameOut.pitch[i] = m_frameBuf[0]->frame.pitch[i];
    }

    //コピーを保存
    setFilterInfo(prm->print());
    m_param = prm;
    return sts;
}

RGY_ERR RGYFilterOverlay::initInput(RGYFilterParamOverlay *prm) {
    m_codecCtxDec.reset();
    m_formatCtx.reset();

    std::string filename_char;
    if (0 == tchar_to_string(prm->overlay.inputFile.c_str(), filename_char, CP_UTF8)) {
        AddMessage(RGY_LOG_ERROR, _T("failed to convert filename to utf-8 characters.\n"));
        return RGY_ERR_UNSUPPORTED;
    }

    { //ファイルのオープン
        AVFormatContext *formatCtx = avformat_alloc_context();
        int ret = 0;
        if ((ret = avformat_open_input(&formatCtx, filename_char.c_str(), nullptr, nullptr)) != 0) {
            AddMessage(RGY_LOG_ERROR, _T("error opening file \"%s\": %s\n"), char_to_tstring(filename_char, CP_UTF8).c_str(), qsv_av_err2str(ret).c_str());
            avformat_free_context(formatCtx);
            return RGY_ERR_FILE_OPEN; // Couldn't open file
        }
        AddMessage(RGY_LOG_DEBUG, _T("opened file \"%s\".\n"), char_to_tstring(filename_char, CP_UTF8).c_str());
        m_formatCtx = std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)>(formatCtx, avformat_free_context);
    }
    m_formatCtx->flags |= AVFMT_FLAG_NONBLOCK; // ffmpeg_opt.cのopen_input_file()と同様にフラグを立てる
    if (avformat_find_stream_info(m_formatCtx.get(), nullptr) < 0) {
        AddMessage(RGY_LOG_ERROR, _T("error finding stream information.\n"));
        return RGY_ERR_UNKNOWN; // Couldn't find stream information
    }
    AddMessage(RGY_LOG_DEBUG, _T("got stream information.\n"));
    av_dump_format(m_formatCtx.get(), 0, filename_char.c_str(), 0);
    m_inputFrames = 0;

    m_stream = nullptr;
    for (uint32_t i = 0; i < m_formatCtx->nb_streams; i++) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_stream = m_formatCtx->streams[i];
        }
    }
    if (!m_stream) {
        AddMessage(RGY_LOG_ERROR, _T("Could not find video stream from \"%s\".\n"), prm->overlay.inputFile.c_str());
        return RGY_ERR_NOT_FOUND; // Couldn't find stream information
    }

    auto codecDecode = avcodec_find_decoder(m_stream->codecpar->codec_id);
    if (!codecDecode) {
        AddMessage(RGY_LOG_ERROR, errorMesForCodec(_T("Failed to find decoder"), m_stream->codecpar->codec_id).c_str());
        return RGY_ERR_NOT_FOUND;
    }
    m_codecCtxDec = std::unique_ptr<AVCodecContext, RGYAVDeleter<AVCodecContext>>(avcodec_alloc_context3(codecDecode), RGYAVDeleter<AVCodecContext>(avcodec_free_context));
    if (!m_codecCtxDec) {
        AddMessage(RGY_LOG_ERROR, errorMesForCodec(_T("Failed to allocate decoder"), m_stream->codecpar->codec_id).c_str());
        return RGY_ERR_NULL_PTR;
    }
    int ret = 0;
    if (0 > (ret = avcodec_parameters_to_context(m_codecCtxDec.get(), m_stream->codecpar))) {
        AddMessage(RGY_LOG_ERROR, _T("failed to set codec param to context for decoder: %s.\n"), qsv_av_err2str(ret).c_str());
        return RGY_ERR_UNKNOWN;
    }
    cpu_info_t cpu_info;
    if (get_cpu_info(&cpu_info)) {
        AVDictionary *pDict = nullptr;
        av_dict_set_int(&pDict, "threads", std::min(cpu_info.logical_cores, 16), 0);
        if (0 > (ret = av_opt_set_dict(m_codecCtxDec.get(), &pDict))) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to set threads for decode (codec: %s): %s\n"),
                char_to_tstring(avcodec_get_name(m_stream->codecpar->codec_id)).c_str(), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNKNOWN;
        }
        av_dict_free(&pDict);
    }
    m_codecCtxDec->time_base = av_stream_get_codec_timebase(m_stream);
    m_codecCtxDec->pkt_timebase = m_stream->time_base;
    if (0 > (ret = avcodec_open2(m_codecCtxDec.get(), codecDecode, nullptr))) {
        AddMessage(RGY_LOG_ERROR, _T("Failed to open decoder for %s: %s\n"), char_to_tstring(avcodec_get_name(m_stream->codecpar->codec_id)).c_str(), qsv_av_err2str(ret).c_str());
        return RGY_ERR_UNSUPPORTED;
    }

    static const std::pair<AVPixelFormat, RGY_CSP> pixfmtDataList[] = {
        { AV_PIX_FMT_YUV420P,     RGY_CSP_NV12 },
        { AV_PIX_FMT_YUVJ420P,    RGY_CSP_NV12 },
        { AV_PIX_FMT_NV12,        RGY_CSP_NV12 },
        { AV_PIX_FMT_NV21,        RGY_CSP_NV12 },
        { AV_PIX_FMT_YUVJ422P,    RGY_CSP_NA },
        { AV_PIX_FMT_YUYV422,     RGY_CSP_YUY2 },
        { AV_PIX_FMT_UYVY422,     RGY_CSP_NA },
#if ENCODER_QSV || ENCODER_VCEENC     
        { AV_PIX_FMT_YUV422P,     RGY_CSP_NV12 },
        { AV_PIX_FMT_NV16,        RGY_CSP_NV12 },
#else                                 
        { AV_PIX_FMT_YUV422P,     RGY_CSP_NV16 },
        { AV_PIX_FMT_NV16,        RGY_CSP_NV16 },
#endif                                
        { AV_PIX_FMT_YUV444P,     RGY_CSP_YUV444 },
        { AV_PIX_FMT_YUVJ444P,    RGY_CSP_YUV444 },
        { AV_PIX_FMT_YUV420P16LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV420P14LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV420P12LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV420P10LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV420P9LE,  RGY_CSP_P010 },
        { AV_PIX_FMT_NV20LE,      RGY_CSP_NA },
#if ENCODER_QSV || ENCODER_VCEENC     
        { AV_PIX_FMT_YUV422P16LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV422P14LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV422P12LE, RGY_CSP_P010 },
        { AV_PIX_FMT_YUV422P10LE, RGY_CSP_P010 },
#else                                 
        { AV_PIX_FMT_YUV422P16LE, RGY_CSP_P210 },
        { AV_PIX_FMT_YUV422P14LE, RGY_CSP_P210 },
        { AV_PIX_FMT_YUV422P12LE, RGY_CSP_P210 },
        { AV_PIX_FMT_YUV422P10LE, RGY_CSP_P210 },
#endif                                
        { AV_PIX_FMT_YUV444P16LE, RGY_CSP_YUV444_16 },
        { AV_PIX_FMT_YUV444P14LE, RGY_CSP_YUV444_16 },
        { AV_PIX_FMT_YUV444P12LE, RGY_CSP_YUV444_16 },
        { AV_PIX_FMT_YUV444P10LE, RGY_CSP_YUV444_16 },
        { AV_PIX_FMT_YUV444P9LE,  RGY_CSP_YUV444_16 },
        { AV_PIX_FMT_RGB24,      (ENCODER_NVENC) ? RGY_CSP_RGB : RGY_CSP_RGB32 },
        { AV_PIX_FMT_BGR24,      (ENCODER_NVENC) ? RGY_CSP_RGB : RGY_CSP_RGB32 },
        { AV_PIX_FMT_RGBA,       (ENCODER_NVENC) ? RGY_CSP_RGB : RGY_CSP_RGB32 },
        { AV_PIX_FMT_BGRA,       (ENCODER_NVENC) ? RGY_CSP_RGB : RGY_CSP_RGB32 },
        { AV_PIX_FMT_GBRP,       RGY_CSP_RGB32 },
        { AV_PIX_FMT_GBRAP,      RGY_CSP_RGB32 }
    };

    const auto pixfmt = (AVPixelFormat)m_stream->codecpar->format;
    const auto pixfmtData = std::find_if(pixfmtDataList, pixfmtDataList + _countof(pixfmtDataList), [pixfmt](const auto& tableData) {
        return tableData.first == pixfmt;
        });
    if (pixfmtData == (pixfmtDataList + _countof(pixfmtDataList)) || pixfmtData->second == RGY_CSP_NA) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid pixel format \"%s\" from input file.\n"), char_to_tstring(av_get_pix_fmt_name(pixfmt)).c_str());
        return RGY_ERR_INVALID_COLOR_FORMAT;
    }

    m_inputCsp = csp_avpixfmt_to_rgy(m_codecCtxDec->pix_fmt);
    const auto hostCsp = pixfmtData->second;
    if (!m_convert) {
        m_convert = std::make_unique<RGYConvertCSP>(0, prm->threadPrm);
        if (m_convert->getFunc(m_inputCsp, hostCsp, false, RGY_SIMD::SIMD_ALL) == nullptr) {
            AddMessage(RGY_LOG_ERROR, _T("color conversion not supported: %s -> %s.\n"),
                RGY_CSP_NAMES[m_inputCsp], RGY_CSP_NAMES[prm->frameIn.csp]);
            return RGY_ERR_INVALID_COLOR_FORMAT;
        }
        AddMessage(RGY_LOG_DEBUG, _T("color conversion selected: %s -> %s.\n"), RGY_CSP_NAMES[m_inputCsp], RGY_CSP_NAMES[hostCsp]);
    }

    const int frameWidth = (RGY_CSP_CHROMA_FORMAT[prm->frameIn.csp] == RGY_CHROMAFMT_YUV420) ? (m_codecCtxDec->width & (~1)) : m_codecCtxDec->width;
    const int frameHeight = (RGY_CSP_CHROMA_FORMAT[prm->frameIn.csp] == RGY_CHROMAFMT_YUV420) ? (m_codecCtxDec->height & (~1)) : m_codecCtxDec->height;
    if (RGY_CSP_CHROMA_FORMAT[prm->frameIn.csp] == RGY_CHROMAFMT_YUV420) {
        if (prm->overlay.width == m_codecCtxDec->width) {
            prm->overlay.width = frameWidth;
        }
        if (prm->overlay.height == m_codecCtxDec->height) {
            prm->overlay.height = frameHeight;
        }
        if (prm->overlay.width > 0) {
            prm->overlay.width = ALIGN(prm->overlay.width, 2);
        }
        if (prm->overlay.height > 0) {
            prm->overlay.height = ALIGN(prm->overlay.height, 2);
        }
    }
    if (!m_frame.dev) {
        m_frame.dev = m_cl->createFrameBuffer(frameWidth, frameHeight, hostCsp, RGY_CSP_BIT_DEPTH[hostCsp]);
        AddMessage(RGY_LOG_DEBUG, _T("Allocated frame(dev) %s %dx%d.\n"), RGY_CSP_NAMES[m_frame.dev->frame.csp], frameWidth, frameHeight);
    }

    if (!m_alpha.dev) {
        m_alpha.dev = m_cl->createFrameBuffer(frameWidth, frameHeight, RGY_CSP_YUV444, RGY_CSP_BIT_DEPTH[RGY_CSP_YUV444]);
        AddMessage(RGY_LOG_DEBUG, _T("Allocated alpha frame(dev) %s %dx%d.\n"), RGY_CSP_NAMES[m_alpha.dev->frame.csp], frameWidth, frameHeight);
    }

    auto inFrame = m_frame.dev->frame;
    if (!m_frame.crop
        && prm->frameIn.csp != hostCsp) {
        unique_ptr<RGYFilterCspCrop> filterCrop(new RGYFilterCspCrop(m_cl));
        shared_ptr<RGYFilterParamCrop> paramCrop(new RGYFilterParamCrop());
        paramCrop->frameIn = inFrame;
        paramCrop->frameOut = inFrame;
        paramCrop->frameOut.csp = prm->frameIn.csp;
        paramCrop->baseFps = prm->baseFps;
        paramCrop->frameIn.mem_type = RGY_MEM_TYPE_GPU;
        paramCrop->frameOut.mem_type = RGY_MEM_TYPE_GPU;
        paramCrop->bOutOverwrite = false;
        if (auto sts = filterCrop->init(paramCrop, m_pLog); sts != RGY_ERR_NONE) {
            return sts;
        }
        m_frame.crop = std::move(filterCrop);
        AddMessage(RGY_LOG_DEBUG, _T("created %s for frame.\n"), m_frame.crop->GetInputMessage().c_str());
        inFrame = paramCrop->frameOut;
    }
    auto inAlpha = m_alpha.dev->frame;
    const auto alphaCsp = RGY_CSP_CHROMA_FORMAT[prm->frameIn.csp] == RGY_CHROMAFMT_YUV420 ? RGY_CSP_YV12 : RGY_CSP_YUV444;
    if (!m_alpha.crop
        && alphaCsp != inAlpha.csp) {
        unique_ptr<RGYFilterCspCrop> filterCrop(new RGYFilterCspCrop(m_cl));
        shared_ptr<RGYFilterParamCrop> paramCrop(new RGYFilterParamCrop());
        paramCrop->frameIn = inAlpha;
        paramCrop->frameOut = inAlpha;
        paramCrop->frameOut.csp = alphaCsp;
        paramCrop->baseFps = prm->baseFps;
        paramCrop->frameIn.mem_type = RGY_MEM_TYPE_GPU;
        paramCrop->frameOut.mem_type = RGY_MEM_TYPE_GPU;
        paramCrop->bOutOverwrite = false;
        if (auto sts = filterCrop->init(paramCrop, m_pLog); sts != RGY_ERR_NONE) {
            return sts;
        }
        m_alpha.crop = std::move(filterCrop);
        AddMessage(RGY_LOG_DEBUG, _T("created %s for alpha.\n"), m_alpha.crop->GetInputMessage().c_str());
        inAlpha = paramCrop->frameOut;
    }

    const int mod = (RGY_CSP_CHROMA_FORMAT[prm->frameIn.csp] == RGY_CHROMAFMT_YUV420) ? 2 : 1;
    sInputCrop crop = { 0 };
    set_auto_resolution(
        prm->overlay.width, prm->overlay.height, 1, 1,
        m_frame.dev->frame.width, m_frame.dev->frame.height,
        m_stream->codecpar->sample_aspect_ratio.num, m_stream->codecpar->sample_aspect_ratio.den, mod, mod,
        RGYResizeResMode::Normal, false, crop);

    if (!m_frame.resize
        && ((prm->overlay.width > 0 && m_frame.dev->frame.width != prm->overlay.width)
        || (prm->overlay.height > 0 && m_frame.dev->frame.height != prm->overlay.height))) {
        {
            unique_ptr<RGYFilterResize> filterResize(new RGYFilterResize(m_cl));
            shared_ptr<RGYFilterParamResize> paramResize(new RGYFilterParamResize());
            paramResize->frameIn = inFrame;
            paramResize->frameOut = inFrame;
            if (prm->overlay.width > 0) {
                paramResize->frameOut.width = prm->overlay.width;
            }
            if (prm->overlay.height > 0) {
                paramResize->frameOut.height = prm->overlay.height;
            }
            paramResize->interp = RGY_VPP_RESIZE_BILINEAR;
            paramResize->baseFps = prm->baseFps;
            paramResize->frameIn.mem_type = RGY_MEM_TYPE_GPU;
            paramResize->frameOut.mem_type = RGY_MEM_TYPE_GPU;
            paramResize->bOutOverwrite = false;
            if (auto sts = filterResize->init(paramResize, m_pLog); sts != RGY_ERR_NONE) {
                return sts;
            }
            m_frame.resize = std::move(filterResize);
            AddMessage(RGY_LOG_DEBUG, _T("created %s for frame.\n"), m_frame.resize->GetInputMessage().c_str());
        }
        {
            unique_ptr<RGYFilterResize> filterResize(new RGYFilterResize(m_cl));
            shared_ptr<RGYFilterParamResize> paramResize(new RGYFilterParamResize());
            paramResize->frameIn = inAlpha;
            paramResize->frameOut = inAlpha;
            if (prm->overlay.width > 0) {
                paramResize->frameOut.width = prm->overlay.width;
            }
            if (prm->overlay.height > 0) {
                paramResize->frameOut.height = prm->overlay.height;
            }
            paramResize->interp = RGY_VPP_RESIZE_BILINEAR;
            paramResize->baseFps = prm->baseFps;
            paramResize->frameIn.mem_type = RGY_MEM_TYPE_GPU;
            paramResize->frameOut.mem_type = RGY_MEM_TYPE_GPU;
            paramResize->bOutOverwrite = false;
            if (auto sts = filterResize->init(paramResize, m_pLog); sts != RGY_ERR_NONE) {
                return sts;
            }
            m_alpha.resize = std::move(filterResize);
            AddMessage(RGY_LOG_DEBUG, _T("created %s for alpha.\n"), m_alpha.resize->GetInputMessage().c_str());
        }
    }
    return RGY_ERR_NONE;
}

std::tuple<RGY_ERR, std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>> RGYFilterOverlay::getFramePkt() {
    if (!m_formatCtx.get()) {
        AddMessage(RGY_LOG_ERROR, _T("formatCtx not initialized.\n"));
        return { RGY_ERR_NOT_INITIALIZED, nullptr };
    }
    std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>> pkt(av_packet_alloc(), RGYAVDeleter<AVPacket>(av_packet_free));
    for (int ret_read_frame = 0; (ret_read_frame = av_read_frame(m_formatCtx.get(), pkt.get())) >= 0;) {
        if (pkt->stream_index == m_stream->index) {
            return { RGY_ERR_NONE, std::move(pkt) };
        }
        av_packet_unref(pkt.get());
    }
    return { RGY_ERR_MORE_BITSTREAM, nullptr };
}

RGY_ERR RGYFilterOverlay::prepareFrameDev(RGYFilterOverlayFrame& target, RGYOpenCLQueue& queue) {
    target.inputPtr = &target.dev->frame;
    if (target.crop) {
        int outputNum = 0;
        RGYFrameInfo *outInfo[1] = { nullptr };
        auto sts_filter = target.crop->filter(target.inputPtr, (RGYFrameInfo **)&outInfo, &outputNum, queue);
        if (outInfo[0] == nullptr || outputNum != 1) {
            AddMessage(RGY_LOG_ERROR, _T("Unknown behavior \"%s\".\n"), target.crop->name().c_str());
            return sts_filter;
        }
        if (sts_filter != RGY_ERR_NONE || outputNum != 1) {
            AddMessage(RGY_LOG_ERROR, _T("Error while running filter \"%s\".\n"), target.crop->name().c_str());
            return sts_filter;
        }
        target.inputPtr = outInfo[0];
    }
    if (target.resize) {
        int outputNum = 0;
        RGYFrameInfo *outInfo[1] = { nullptr };
        auto sts_filter = target.resize->filter(target.inputPtr, (RGYFrameInfo **)&outInfo, &outputNum, queue);
        if (outInfo[0] == nullptr || outputNum != 1) {
            AddMessage(RGY_LOG_ERROR, _T("Unknown behavior \"%s\".\n"), target.resize->name().c_str());
            return sts_filter;
        }
        if (sts_filter != RGY_ERR_NONE || outputNum != 1) {
            AddMessage(RGY_LOG_ERROR, _T("Error while running filter \"%s\".\n"), target.resize->name().c_str());
            return sts_filter;
        }
        target.inputPtr = outInfo[0];
    }
    return RGY_ERR_NONE;
}

template<typename TypeSrc>
static inline void set_lumakey(uint8_t *ptrDstAlpha, const TypeSrc *ptrSrc, const int width, const int srcBitdepth,
    const float threshold, const float tolerance, const float softness, const float baseAlpha) {
    const float black = std::max(threshold - tolerance, 0.0f);
    const float white = std::min(threshold + tolerance, 1.0f);
    for (int i = 0; i < width; i++) {
        const float lumaf = clamp(((float)ptrSrc[i] * (float)(1.0f / (float)((1 << srcBitdepth) - 1)) - (16.0f / 255.0f)) * (255.0f / (235.0f - 16.0f)), 0.0f, 1.0f);
        float alpha = 255.0f;
        if (lumaf >= black && lumaf <= white) {
            alpha = 0.0f;
        } else if (softness > 0.0f && lumaf > black - softness && lumaf < white + softness) {
            if (lumaf < black) {
                alpha = 255.0f - (lumaf - black + softness) * 255.0f / softness;
            } else {
                alpha = ((lumaf - white) * 255.0f / softness + 0.5f);
            }
        }
        ptrDstAlpha[i] = (uint8_t)clamp((int)(alpha * baseAlpha + 0.5f), 0, 255);
    }
}

RGY_ERR RGYFilterOverlay::getFrame(RGYOpenCLQueue& queue) {
    if (!m_codecCtxDec) {
        AddMessage(RGY_LOG_ERROR, _T("decoder not initialized.\n"));
        return RGY_ERR_NOT_INITIALIZED;
    }
    std::unique_ptr<AVFrame, RGYAVDeleter<AVFrame>> frame(av_frame_alloc(), RGYAVDeleter<AVFrame>(av_frame_free));
    std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>> pkt;
    //動画のデコードを行う
    int got_frame = 0;
    while (!got_frame) {
        if (!pkt) {
            auto [err, pktNew] = getFramePkt();
            if (err == RGY_ERR_MORE_BITSTREAM) {
                pkt.reset();
            } else if (err != RGY_ERR_NONE) {
                return err;
            } else {
                pkt = std::move(pktNew);
            }
        }
        int ret = avcodec_send_packet(m_codecCtxDec.get(), pkt.get());
        //AVERROR(EAGAIN) -> パケットを送る前に受け取る必要がある
        //パケットが受け取られていないのでpopしない
        if (ret != AVERROR(EAGAIN)) {
            pkt.reset();
        }
        if (ret == AVERROR_EOF) { //これ以上パケットを送れない
            AddMessage(RGY_LOG_DEBUG, _T("failed to send packet to video decoder, already flushed: %s.\n"), qsv_av_err2str(ret).c_str());
        } else if (ret < 0 && ret != AVERROR(EAGAIN)) {
            AddMessage(RGY_LOG_ERROR, _T("failed to send packet to video decoder: %s.\n"), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNDEFINED_BEHAVIOR;
        }
        ret = avcodec_receive_frame(m_codecCtxDec.get(), frame.get());
        if (ret == AVERROR(EAGAIN)) { //もっとパケットを送る必要がある
            continue;
        }
        if (ret == AVERROR_EOF) {
            //最後まで読み込んだ
            return RGY_ERR_MORE_DATA;
        }
        if (ret < 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to receive frame from video decoder: %s.\n"), qsv_av_err2str(ret).c_str());
            return RGY_ERR_UNDEFINED_BEHAVIOR;
        }
        got_frame = TRUE;
    }
    //フレームデータをコピー
    {
        auto ret = m_frame.dev->queueMapBuffer(queue, CL_MAP_WRITE);
        if (ret != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to map buffer for frame: %s.\n"), get_err_mes(ret));
            return ret;
        }
        ret = m_frame.dev->mapWait();
        if (ret != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to wait map buffer for frame: %s.\n"), get_err_mes(ret));
            return ret;
        }
        const auto frameHost = m_frame.dev->mappedHost()->frameInfo();
        sInputCrop crop = { 0 };
        void *dst_array_frame[3] = {
            getPlane(&frameHost, RGY_PLANE_Y).ptr[0],
            getPlane(&frameHost, RGY_PLANE_U).ptr[0],
            getPlane(&frameHost, RGY_PLANE_V).ptr[0]
        };
        m_convert->run(rgy_avframe_interlaced(frame.get()),
            dst_array_frame, (const void **)frame->data,
            frameHost.width, frame->linesize[0], frame->linesize[1], frameHost.pitch[0],
            frameHost.height, frameHost.height, crop.c);

        ret = m_frame.dev->unmapBuffer(queue);
        if (ret != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to unmap buffer for frame: %s.\n"), get_err_mes(ret));
            return ret;
        }
        if (m_inputFrames == 0) {
            AddMessage(RGY_LOG_DEBUG, _T("Copied frame to device.\n"));
        }
    }
    {
        //不透明度データをコピー
        auto ret = m_alpha.dev->queueMapBuffer(queue, CL_MAP_WRITE);
        if (ret != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to map buffer for alpha: %s.\n"), get_err_mes(ret));
            return ret;
        }
        ret = m_alpha.dev->mapWait();
        if (ret != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to wait map buffer for alpha: %s.\n"), get_err_mes(ret));
            return ret;
        }
        const auto frameHostAlpha = m_alpha.dev->mappedHost()->frameInfo();
        auto prm = std::dynamic_pointer_cast<RGYFilterParamOverlay>(m_param);
        if (!prm) {
            AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
            return RGY_ERR_INVALID_PARAM;
        }

        const auto pixfmt = (AVPixelFormat)m_stream->codecpar->format;
        uint8_t *dst_array_alpha[3] = {
            getPlane(&frameHostAlpha, RGY_PLANE_Y).ptr[0],
            getPlane(&frameHostAlpha, RGY_PLANE_U).ptr[0],
            getPlane(&frameHostAlpha, RGY_PLANE_V).ptr[0]
        };

        const bool readAlphaFromPixFmt = pixfmt == AV_PIX_FMT_RGBA
                                      || pixfmt == AV_PIX_FMT_BGRA;
        if (readAlphaFromPixFmt) {
            AddMessage(RGY_LOG_DEBUG, _T("Reading alpha from frame.\n"));
            for (int iplane = 0; iplane < _countof(dst_array_alpha); iplane++) {
                const uint8_t *ptrSrcLineA = frame->data[0];
                const auto pitchSrc = frame->linesize[0];
                auto ptrDstLineA = dst_array_alpha[iplane];
                const auto pitchDst = frameHostAlpha.pitch[iplane];
                for (int j = 0; j < frameHostAlpha.height; j++, ptrDstLineA += pitchDst, ptrSrcLineA += pitchSrc) {
                    auto ptrSrc = ptrSrcLineA;
                    auto ptrDst = ptrDstLineA;
                    for (int i = 0; i < frameHostAlpha.width; i++) {
                        ptrDst[i] = ptrSrc[4 * i + 3];
                    }
                }
            }
        } else if (prm->overlay.alphaMode == VppOverlayAlphaMode::Mul) {
            prm->overlay.alphaMode = VppOverlayAlphaMode::Override; // Mulモードは無効
        }
        if (prm->overlay.alphaMode == VppOverlayAlphaMode::LumaKey) {
            const float baseAlpha = (prm->overlay.alpha > 0.0f) ? prm->overlay.alpha : 1.0f;
            int iplane = 0;
            auto ptrSrcLineY = frame->data[0];
            const auto pitchSrc = frame->linesize[0];
            auto ptrDstLineA = dst_array_alpha[iplane];
            const auto pitchDst = frameHostAlpha.pitch[iplane];
            for (int j = 0; j < frameHostAlpha.height; j++, ptrDstLineA += pitchDst, ptrSrcLineY += pitchSrc) {
                auto ptrDst = ptrDstLineA;
                if (RGY_CSP_BIT_DEPTH[m_frame.dev->frame.csp] > 8) {
                    set_lumakey<uint16_t>(ptrDst, (const uint16_t *)ptrSrcLineY, frameHostAlpha.width, RGY_CSP_BIT_DEPTH[m_frame.dev->frame.csp],
                        prm->overlay.lumaKey.threshold, prm->overlay.lumaKey.tolerance, prm->overlay.lumaKey.shoftness, baseAlpha);
                } else {
                    set_lumakey<uint8_t>(ptrDst, (const uint8_t *)ptrSrcLineY, frameHostAlpha.width, RGY_CSP_BIT_DEPTH[m_frame.dev->frame.csp],
                        prm->overlay.lumaKey.threshold, prm->overlay.lumaKey.tolerance, prm->overlay.lumaKey.shoftness, baseAlpha);
                }
            }
            for (iplane = 1; iplane < _countof(dst_array_alpha); iplane++) {
                memcpy(dst_array_alpha[iplane], dst_array_alpha[0], frameHostAlpha.pitch[iplane] * frameHostAlpha.height);
            }
        } else if (prm->overlay.alpha > 0.0f || !readAlphaFromPixFmt) {
            //値を設定する場合の設定値
            const uint8_t alpha8 = prm->overlay.alpha > 0.0f ? (uint8_t)std::min((int)(prm->overlay.alpha * 255.0 + 0.001), 255) : 255;
            AddMessage(RGY_LOG_DEBUG, _T("Set alpha %d (%.3f).\n"), alpha8, prm->overlay.alpha);
            int iplane = 0;
            auto ptrDstLineA = dst_array_alpha[iplane];
            const auto pitchDst = frameHostAlpha.pitch[iplane];
            for (int j = 0; j < frameHostAlpha.height; j++, ptrDstLineA += pitchDst) {
                auto ptrDst = ptrDstLineA;
                if (prm->overlay.alphaMode == VppOverlayAlphaMode::Mul) {
                    for (int i = 0; i < frameHostAlpha.width; i++) {
                        ptrDst[i] = (uint8_t)clamp((int)(ptrDst[i] * prm->overlay.alpha + 0.5), 0, 255);
                    }
                } else {
                    for (int i = 0; i < frameHostAlpha.width; i++) {
                        ptrDst[i] = alpha8;
                    }
                }
            }
            for (iplane = 1; iplane < _countof(dst_array_alpha); iplane++) {
                memcpy(dst_array_alpha[iplane], dst_array_alpha[0], frameHostAlpha.pitch[iplane] * frameHostAlpha.height);
            }
        }
        ret = m_alpha.dev->unmapBuffer(queue);
        if (ret != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("Failed to unmap buffer for alpha: %s.\n"), get_err_mes(ret));
            return ret;
        }
        if (m_inputFrames == 0) {
            AddMessage(RGY_LOG_DEBUG, _T("Copied alpha to device.\n"));
        }
    }
    auto sts = RGY_ERR_NONE;
    sts = prepareFrameDev(m_frame, queue);
    if (sts != RGY_ERR_NONE) {
        return sts;
    }
    sts = prepareFrameDev(m_alpha, queue);
    if (sts != RGY_ERR_NONE) {
        return sts;
    }
    m_inputFrames++;
    return RGY_ERR_NONE;
}


RGY_ERR RGYFilterOverlay::run_filter(const RGYFrameInfo *pInputFrame, RGYFrameInfo **ppOutputFrames, int *pOutputFrameNum, RGYOpenCLQueue& queue_main, const std::vector<RGYOpenCLEvent>& wait_events, RGYOpenCLEvent *event) {
    RGY_ERR sts = RGY_ERR_NONE;

    if (pInputFrame->ptr[0] == nullptr) {
        return sts;
    }
    if (!m_overlay.get()) {
        AddMessage(RGY_LOG_ERROR, _T("failed to load RGY_FILTER_OVERLAY_CL(m_overlay)\n"));
        return RGY_ERR_OPENCL_CRUSH;
    }

    *pOutputFrameNum = 1;
    if (ppOutputFrames[0] == nullptr) {
        auto pOutFrame = m_frameBuf[0].get();
        ppOutputFrames[0] = &pOutFrame->frame;
    }
    ppOutputFrames[0]->picstruct = pInputFrame->picstruct;
    //if (interlaced(*pInputFrame)) {
    //    return filter_as_interlaced_pair(pInputFrame, ppOutputFrames[0], cudaStreamDefault);
    //}
    const auto memcpyKind = getMemcpyKind(pInputFrame->mem_type, ppOutputFrames[0]->mem_type);
    if (memcpyKind != RGYCLMemcpyD2D) {
        AddMessage(RGY_LOG_ERROR, _T("only supported on device memory.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (m_param->frameOut.csp != m_param->frameIn.csp) {
        AddMessage(RGY_LOG_ERROR, _T("csp does not match.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    auto prm = std::dynamic_pointer_cast<RGYFilterParamOverlay>(m_param);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    sts = getFrame(queue_main);
    if (sts == RGY_ERR_MORE_DATA) {
        if (m_inputFrames == 0) {
            AddMessage(RGY_LOG_ERROR, _T("Unknown error.\n"));
            return RGY_ERR_UNKNOWN;
        } else if (m_inputFrames > 1) {
            if (prm->overlay.loop) { // loopさせる場合はファイルを開きなおして再読み込み
                m_codecCtxDec.reset();
                m_formatCtx.reset();
                if ((sts = initInput(prm.get())) != RGY_ERR_NONE) {
                    return sts;
                }
                if ((sts = getFrame(queue_main)) != RGY_ERR_NONE) {
                    return sts;
                }
                if (m_inputFrames == 0 || !m_frame.inputPtr) {
                    AddMessage(RGY_LOG_ERROR, _T("Failed to re-open file.\n"));
                    return RGY_ERR_UNKNOWN;
                }
            } else {
                m_frame.inputPtr = nullptr;
                m_alpha.inputPtr = nullptr;
                sts = RGY_ERR_NONE;
            }
        }
    }

    if (m_frame.inputPtr) {
        sts = overlayFrame(ppOutputFrames[0], pInputFrame, queue_main, wait_events, event);
    } else {
        auto err = m_cl->copyFrame(ppOutputFrames[0], pInputFrame, nullptr, queue_main, wait_events, event, RGYFrameCopyMode::FRAME);
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed to copy frame: %s.\n"), get_err_mes(err));
            return RGY_ERR_CUDA;
        }
        copyFrameProp(ppOutputFrames[0], pInputFrame);
    }
    return sts;
}

void RGYFilterOverlay::close() {
    m_convert.reset();
    m_codecCtxDec.reset();
    m_formatCtx.reset();
    m_frameBuf.clear();
    m_bInterlacedWarn = false;
}
