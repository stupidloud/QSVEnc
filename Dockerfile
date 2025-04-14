# Build stage for FFmpeg
FROM ubuntu:jammy AS builder
ENV TZ=UTC \
    DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -qq -y \
    build-essential libtool pkg-config git cmake wget \
    libfdk-aac-dev libass-dev g++ make nasm yasm \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Build FFmpeg
RUN cd /tmp && wget -qO- https://www.ffmpeg.org/releases/ffmpeg-4.4.5.tar.xz | tar xJf - && \
    cd ffmpeg-4.4.* && \
    ./configure --prefix=/usr --enable-shared --enable-nonfree --enable-libfdk-aac --enable-libass && \
    make -s -j$(nproc) && \
    make install && \
    ldconfig && \
    ffmpeg -version

# Final stage
FROM ubuntu:jammy
ENV TZ=UTC \
    DEBIAN_FRONTEND=noninteractive

# Install Intel Media SDK and runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    gpg gpg-agent wget ca-certificates \
    && wget -qO - https://repositories.intel.com/graphics/intel-graphics.key | \
    gpg --dearmor --output /usr/share/keyrings/intel-graphics.gpg \
    && echo 'deb [arch=amd64,i386 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.intel.com/graphics/ubuntu jammy arc' | \
    tee /etc/apt/sources.list.d/intel.gpu.jammy.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
    intel-media-va-driver-non-free intel-opencl-icd \
    libmfx1 libmfxgen1 \
    libva-drm2 libva-x11-2 libigfxcmrt7 \
    libass9 libfdk-aac2 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Copy FFmpeg from builder stage - only necessary files
# Copy FFmpeg and FFprobe binaries
COPY --from=builder /usr/bin/ffmpeg /usr/bin/
COPY --from=builder /usr/bin/ffprobe /usr/bin/

# Copy FFmpeg libraries
COPY --from=builder /usr/lib/libavcodec*.so* /usr/lib/
COPY --from=builder /usr/lib/libavdevice*.so* /usr/lib/
COPY --from=builder /usr/lib/libavfilter*.so* /usr/lib/
COPY --from=builder /usr/lib/libavformat*.so* /usr/lib/
COPY --from=builder /usr/lib/libavutil*.so* /usr/lib/
COPY --from=builder /usr/lib/libpostproc*.so* /usr/lib/
COPY --from=builder /usr/lib/libswresample*.so* /usr/lib/
COPY --from=builder /usr/lib/libswscale*.so* /usr/lib/

# Update library cache
RUN ldconfig && \
    ffmpeg -version

# Install the latest QSVEnc release for Ubuntu
RUN LATEST_URL=$(wget -qO- https://api.github.com/repos/rigaya/QSVEnc/releases/latest | grep -o 'https://github.com/rigaya/QSVEnc/releases/download/[^"]*Ubuntu20.04_amd64.deb') \
    && echo "Downloading latest QSVEnc from: $LATEST_URL" \
    && wget -O /tmp/qsvencc.deb "$LATEST_URL" \
    && apt-get update \
    && dpkg -i --force-depends /tmp/qsvencc.deb \
    && rm /tmp/qsvencc.deb \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

ENTRYPOINT ["/usr/bin/qsvencc"]

