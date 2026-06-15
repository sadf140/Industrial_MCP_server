FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    ninja-build \
    python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DINDUSTRIAL_MCP_WITH_OPCUA=ON \
    -DINDUSTRIAL_MCP_BUILD_TESTS=OFF
RUN cmake --build build --target industrial-mcp-server --parallel 2

FROM ubuntu:24.04

RUN useradd --create-home --uid 10001 mcpuser \
    && mkdir -p /etc/industrial-mcp /var/lib/industrial-mcp \
    && chown -R mcpuser:mcpuser /var/lib/industrial-mcp

COPY --from=builder /src/build/industrial-mcp-server /usr/local/bin/industrial-mcp-server
COPY config/config.example.json /etc/industrial-mcp/config.json

USER mcpuser
EXPOSE 8080 9090

ENTRYPOINT ["/usr/local/bin/industrial-mcp-server"]
CMD ["--config", "/etc/industrial-mcp/config.json"]
