# Multi-stage build for a slim AgentChain node image.

ARG ALPINE=alpine:3.20

# -------- builder --------
FROM ${ALPINE} AS builder

RUN apk add --no-cache build-base cmake libsodium-dev linux-headers

WORKDIR /src
COPY CMakeLists.txt PROTOCOL.md README.md LICENSE TECHNICAL-IMPLEMENTATION.md ./
COPY src/ ./src/
COPY include/ ./include/
COPY tests/ ./tests/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAGENTCHAIN_BUILD_TESTS=OFF \
 && cmake --build build -j \
 && strip build/agentchain

# -------- runtime --------
FROM ${ALPINE}

RUN apk add --no-cache libsodium ca-certificates \
 && addgroup -S agentchain \
 && adduser  -S -G agentchain agentchain \
 && mkdir -p /var/lib/agentchain \
 && chown agentchain:agentchain /var/lib/agentchain

COPY --from=builder /src/build/agentchain /usr/local/bin/agentchain
COPY deploy/docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

USER agentchain
WORKDIR /var/lib/agentchain

EXPOSE 30303 30304

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["node", "--data-dir", "/var/lib/agentchain", "--port", "30303", "--rpc-port", "30304", "--rpc-host", "0.0.0.0", "--validator"]
