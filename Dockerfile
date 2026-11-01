# ---- build stage ----
FROM --platform=linux/amd64 debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt .
COPY include ./include
COPY src ./src
COPY tests ./tests

# FetchContent downloads cpp-httplib and nlohmann_json at configure time.
RUN cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)"

# ---- runtime stage ----
FROM --platform=linux/amd64 debian:bookworm-slim

# libssl3 + ca-certificates: outbound HTTPS to api.telegram.org.
# curl: used only by the container healthcheck.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m -u 1000 relay

COPY --from=build /src/build/telegram_relay_server /usr/local/bin/telegram_relay_server

USER relay
ENV PORT=8080
EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${PORT}/health" || exit 1

CMD ["telegram_relay_server"]
