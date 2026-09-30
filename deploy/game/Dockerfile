# syntax=docker/dockerfile:1
# The web build (ADR 0049): the wasm GDExtension, the OpenNova Web export and
# the static site that serves it at game.<domain>. Built from the repo root with
# the submodules checked out and the shipped assets/ pulled from LFS:
#
#   docker build -f deploy/game/Dockerfile -t opennova-game .
#   docker run --rm -p 8090:8090 opennova-game     # then open http://localhost:8090
#
# localhost is a secure context, so the threaded build and the folder picker
# work there as they do behind TLS. Dockerfile.dockerignore (beside this file)
# keeps the context to the directories the stages copy.

ARG EMSCRIPTEN_VERSION=4.0.20
ARG GODOT_VERSION=4.6.1-stable

# --- the web side module ------------------------------------------------------
# Must be the Emscripten that built Godot 4.6's web templates
# (scripts/build_godot_web.sh checks). The image's CMake predates the sqlite
# fetch's options.
FROM emscripten/emsdk:${EMSCRIPTEN_VERSION} AS wasm
RUN pip install --no-cache-dir "cmake>=3.28,<4" ninja
WORKDIR /src
COPY third_party/ third_party/
COPY engine/ engine/
COPY godot/src/ godot/src/
COPY godot/bin/opennova.gdextension godot/bin/
COPY scripts/build_godot_web.sh scripts/
RUN scripts/build_godot_web.sh --release --build-dir /build

# --- the exporting editor's own extension -------------------------------------
# The headless editor that exports loads the Linux build so the engine's classes
# resolve while it imports the project.
FROM ubuntu:24.04 AS native
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake curl ninja-build python3 unzip \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY third_party/ third_party/
COPY engine/ engine/
COPY godot/src/ godot/src/
COPY godot/bin/opennova.gdextension godot/bin/
RUN cmake -S godot/src -B /build -G Ninja \
        -DGODOTCPP_TARGET=template_debug -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --target opennova

# --- the export ---------------------------------------------------------------
FROM native AS export
ARG GODOT_VERSION
WORKDIR /opt/godot
RUN base="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}" \
    && editor="Godot_v${GODOT_VERSION}_linux.x86_64" \
    && templates="Godot_v${GODOT_VERSION}_export_templates.tpz" \
    && curl -fsSL --retry 3 -O "$base/$editor.zip" -O "$base/$templates" -O "$base/SHA512-SUMS.txt" \
    && grep -E " ($editor\.zip|$templates)\$" SHA512-SUMS.txt | sha512sum -c - \
    && unzip -q "$editor.zip" \
    && mv "$editor" /usr/local/bin/godot \
    && dest="/root/.local/share/godot/export_templates/$(echo "$GODOT_VERSION" | tr - .)" \
    && mkdir -p "$dest" \
    && unzip -q -j "$templates" "templates/version.txt" "templates/web_dlink_*" -d "$dest" \
    && rm -f "$editor.zip" "$templates" SHA512-SUMS.txt
WORKDIR /src
COPY godot/ godot/
COPY assets/ assets/
COPY scripts/godot_bin.sh scripts/bootstrap_imgui_godot.sh scripts/package_godot_web.sh scripts/
COPY --from=wasm /src/godot/bin/libopennova.web.template_release.wasm32.wasm godot/bin/
# The addon's export hook drops its ImGuiRoot autoload from the web export.
RUN scripts/bootstrap_imgui_godot.sh \
    && GODOT_BIN=/usr/local/bin/godot scripts/package_godot_web.sh --out /site

# --- the site -----------------------------------------------------------------
FROM nginx:1.27-alpine
COPY deploy/game/nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=export /site/ /usr/share/nginx/html/
EXPOSE 8090
