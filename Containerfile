# syntax=docker/dockerfile:1
ARG PORTMASTER_IMAGE=ghcr.io/monkeyx-net/portmaster-build-templates/portmaster-builder:aarch64-latest

FROM ${PORTMASTER_IMAGE} AS portmaster-base
USER root
WORKDIR /workspace

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libmpg123-dev \
        libx11-dev \
        libwayland-dev \
        libxkbcommon-dev \
        libxtst-dev \
        libdrm-dev \
        libgbm-dev \
        libpulse-dev \
        libasound2-dev \
        wayland-protocols && \
    rm -rf /var/lib/apt/lists/*

FROM portmaster-base AS game-build
ARG GAME=gtasa
ARG LAUNCHER_SCRIPT="Grand Theft Auto San Andreas.sh"
ARG BINARY_NAME=gtasa_linux
ARG GAME_TITLE="Grand Theft Auto: San Andreas"
ARG CONFIG_NAME=gtasa_nx.cfg
ARG APPSTATE_NAME=appstate.txt

ARG SDL_COMMIT=6057d79baf8321bf190479a699655f06cc2a962f
ARG SPIRV_CROSS_COMMIT=be71ee8c12cd7dc5ca8fa9581f708c2e8561fe2a

COPY . /workspace
RUN chmod +x /workspace/scripts/portmaster-build.sh && \
    SDL_COMMIT="$SDL_COMMIT" \
    SPIRV_CROSS_COMMIT="$SPIRV_CROSS_COMMIT" \
    GAME="$GAME" \
    LAUNCHER_SCRIPT="$LAUNCHER_SCRIPT" \
    BINARY_NAME="$BINARY_NAME" \
    GAME_TITLE="$GAME_TITLE" \
    CONFIG_NAME="$CONFIG_NAME" \
    APPSTATE_NAME="$APPSTATE_NAME" \

    /workspace/scripts/portmaster-build.sh build

FROM scratch AS artifact
COPY --from=game-build /workspace/package/ /
