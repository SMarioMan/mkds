# syntax=docker/dockerfile:1
# Reproducible Podman build & test environment for MKDS Simple-Mode 32-Track Mod.
# Contains NO Nintendo ROMs, BIOSes, firmware, or game assets; mount those at runtime.

# -----------------------------------------------------------------------------
# Stage 1: Build pinned wibo (commit 420a75b, 53-bit x87 guest FPU precision)
# -----------------------------------------------------------------------------
FROM docker.io/library/ubuntu:24.04 AS wibo-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    clang \
    cmake \
    git \
    libclang-dev \
    liburing-dev \
    lld \
    make \
    ninja-build \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

COPY tools/build_wibo.sh /tmp/mkds/tools/build_wibo.sh
RUN chmod +x /tmp/mkds/tools/build_wibo.sh && \
    /tmp/mkds/tools/build_wibo.sh /opt/mkds-tools/bin/wibo

# -----------------------------------------------------------------------------
# Stage 2: Build headless melonDS core static library (pinned commit 906e9ebb)
#          for deterministic in-container Download Play validation
# -----------------------------------------------------------------------------
FROM docker.io/library/ubuntu:24.04 AS melonds-builder

ENV DEBIAN_FRONTEND=noninteractive
ARG MELONDS_COMMIT=906e9ebb27da8c6a715cd7abab4abfe8a8d29427

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    g++ \
    gcc \
    git \
    ninja-build \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --filter=blob:none https://github.com/melonDS-emu/melonDS.git /opt/melonds-src && \
    git -C /opt/melonds-src checkout "${MELONDS_COMMIT}" && \
    cmake -S /opt/melonds-src -B /opt/melonds-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_QT_SDL=OFF \
        -DENABLE_OGLRENDERER=OFF \
        -DENABLE_JIT=OFF \
        -DENABLE_GDBSTUB=OFF \
        -DENABLE_LTO_RELEASE=OFF && \
    cmake --build /opt/melonds-build --target core

# -----------------------------------------------------------------------------
# Stage 3: Final reproducible MKDS build + static test + emulator test image
# -----------------------------------------------------------------------------
FROM docker.io/library/ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ARG DSD_TAG=v0.12.1
ARG OBJDIFF_TAG=v3.8.2

RUN apt-get update && apt-get install -y --no-install-recommends \
    binutils-arm-none-eabi \
    ca-certificates \
    cmake \
    g++ \
    gcc \
    git \
    liburing2 \
    make \
    ninja-build \
    python3 \
    python3-pil \
    python3-pyelftools \
    python3-yaml \
    && rm -rf /var/lib/apt/lists/*

COPY --from=wibo-builder /opt/mkds-tools/bin/wibo /opt/mkds-tools/bin/wibo
COPY --from=melonds-builder /opt/melonds-src /opt/melonds-src
COPY --from=melonds-builder /opt/melonds-build /opt/melonds-build

COPY tools/download_tool.py /tmp/download_tool.py
RUN python3 /tmp/download_tool.py dsd "${DSD_TAG}" --path /opt/mkds-tools/bin/dsd && \
    python3 /tmp/download_tool.py mwccarm latest --path /opt/mkds-tools && \
    python3 /tmp/download_tool.py objdiff "${OBJDIFF_TAG}" --path /opt/mkds-tools/bin/objdiff-cli && \
    rm -f /tmp/download_tool.py && \
    /opt/mkds-tools/bin/wibo --version && \
    /opt/mkds-tools/bin/dsd --version

ENV MKDS_WIBO=/opt/mkds-tools/bin/wibo \
    MKDS_DSD=/opt/mkds-tools/bin/dsd \
    MKDS_MWCCARM=/opt/mkds-tools/mwccarm \
    MKDS_OBJDIFF=/opt/mkds-tools/bin/objdiff-cli \
    MELONDS_SRC=/opt/melonds-src \
    MELONDS_BUILD=/opt/melonds-build

WORKDIR /workspace
