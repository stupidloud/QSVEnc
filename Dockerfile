# Build stage for FFmpeg
FROM ubuntu:jammy AS builder
ENV TZ=UTC \
    DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -qq -y \
    build-essential libtool pkg-config git cmake \
    curl libfdk-aac-dev libass-dev g++ make nasm yasm

# Build FFmpeg
RUN cd /tmp && curl https://www.ffmpeg.org/releases/ffmpeg-4.4.5.tar.xz | tar xJf - && \
    cd ffmpeg-4.4.* && \
    ./configure --enable-shared --enable-nonfree --enable-libfdk-aac --enable-libass && \
    make -s -j$(nproc) && \
    make install && \
    ldconfig && \
    ls -la /usr/local/lib/libavcodec* && \
    ffmpeg -version

# Final stage
FROM ubuntu:jammy
ENV TZ=UTC \
    DEBIAN_FRONTEND=noninteractive

# Install Intel Media SDK and runtime dependencies
RUN apt update && apt install -y --no-install-recommends \
    gpg-agent wget software-properties-common curl jq ca-certificates && \
    wget -qO - https://repositories.intel.com/graphics/intel-graphics.key | \
    gpg --dearmor --output /usr/share/keyrings/intel-graphics.gpg && \
    echo 'deb [arch=amd64,i386 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.intel.com/graphics/ubuntu jammy arc' | \
    tee /etc/apt/sources.list.d/intel.gpu.jammy.list && \
    apt update && \
    apt install -y --no-install-recommends \
    intel-media-va-driver-non-free intel-opencl-icd opencl-headers \
    intel-level-zero-gpu level-zero libmfx1 libmfxgen1 \
    libva-drm2 libva-x11-2 libva-glx2 libigfxcmrt7 \
    libfdk-aac2 libass9 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Copy FFmpeg from builder stage
COPY --from=builder /usr/local/lib /usr/local/lib
COPY --from=builder /usr/local/bin /usr/local/bin
COPY --from=builder /usr/local/include /usr/local/include
COPY --from=builder /usr/local/share /usr/local/share

# Update library cache
RUN ldconfig && \
    ls -la /usr/local/lib/libavcodec* && \
    ffmpeg -version

# Install the latest QSVEnc release for Ubuntu
RUN LATEST_URL=$(curl -s https://api.github.com/repos/rigaya/QSVEnc/releases/latest | jq -r '.assets[] | select(.name | contains("Ubuntu20.04_amd64.deb")) | .browser_download_url') && \
    echo "Downloading latest QSVEnc from: $LATEST_URL" && \
    curl -L -o /qsvencc.deb "$LATEST_URL" && \
    apt-get update && \
    apt-get install -y --no-install-recommends ./qsvencc.deb && \
    rm /qsvencc.deb && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

ENTRYPOINT ["/usr/bin/qsvencc"]

