# ============================================================================
# Multi-stage Dockerfile for BACnet Event Server
# ============================================================================

# Build Stage
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libhiredis-dev \
    libcjson-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Clone and build BACnet-Stack
RUN git clone --depth 1 https://github.com/bacnet-stack/bacnet-stack.git external/bacnet-stack

# Copy sources
COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/
COPY config/ config/

# Build
RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc)

# ============================================================================
# Runtime Stage
# ============================================================================

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libhiredis0.14 \
    libcjson1 \
    && rm -rf /var/lib/apt/lists/*

# Create user
RUN useradd -r -s /bin/false bacnet

# Create directories
RUN mkdir -p /etc/bacnet-gateway /var/log/bacnet-gateway && \
    chown -R bacnet:bacnet /var/log/bacnet-gateway

# Copy binary
COPY --from=builder /build/build/bacnet-kurrentdb-server /usr/local/bin/

# Configuration
COPY config/server-config.json /etc/bacnet-gateway/server-config.json.example

# Port
EXPOSE 47808/udp

# User
USER bacnet

# Health Check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD pgrep bacnet-kurrentdb || exit 1

# Entrypoint
ENTRYPOINT ["/usr/local/bin/bacnet-kurrentdb-server"]
CMD ["/etc/bacnet-gateway/server-config.json"]
