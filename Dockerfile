# QSVEnc runtime image
# qsvencc 静态链入了 ffmpeg，不依赖系统 libav*。
# 系统的 ffmpeg/ffprobe 仅作为外部辅助工具（探测/混流等）保留，
# 用 apt 自带版本即可，不必从源码编。
FROM ubuntu:jammy
ENV TZ=UTC \
    DEBIAN_FRONTEND=noninteractive

# Intel Graphics 仓库 + 运行时依赖 + ffmpeg/ffprobe
RUN apt-get update && apt-get install -y --no-install-recommends \
    gpg gpg-agent wget ca-certificates \
    && wget -qO - https://repositories.intel.com/graphics/intel-graphics.key | \
       gpg --dearmor --output /usr/share/keyrings/intel-graphics.gpg \
    && echo 'deb [arch=amd64,i386 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.intel.com/graphics/ubuntu jammy arc' \
       | tee /etc/apt/sources.list.d/intel.gpu.jammy.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
       intel-media-va-driver-non-free intel-opencl-icd \
       libmfx1 libmfxgen1 \
       libva-drm2 libva-x11-2 libigfxcmrt7 \
       ffmpeg \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# QSVENC_DEB_URL 由 CI 通过 gh api（已认证，限额 1000/h）解析后注入，
# 避免 docker build 内未认证调用 api.github.com 触发 60/h 速率限制。
ARG QSVENC_DEB_URL
RUN test -n "$QSVENC_DEB_URL" || (echo "QSVENC_DEB_URL build-arg is required" >&2 && exit 1) \
    && echo "Downloading QSVEnc from: $QSVENC_DEB_URL" \
    && wget -O /tmp/qsvencc.deb "$QSVENC_DEB_URL" \
    && apt-get update \
    && apt-get install -y --no-install-recommends /tmp/qsvencc.deb \
    && rm /tmp/qsvencc.deb \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

ENTRYPOINT ["/usr/bin/qsvencc"]
