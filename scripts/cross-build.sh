#!/usr/bin/env bash
# Cross-compila per Raspberry Pi (aarch64) dal Mac, in container.
# Il binario esce in build-arm64/namzero-lite e si copia sul Pi.
#
# Prerequisiti: Docker sul Mac, sorgenti NAM in ~/nam (rsync dal Pi).
# Uso: ./scripts/cross-build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Docker Desktop vuole scrivere in ~/.docker (fuori dal workspace): si tiene
# la config nel repo e si usa il builder classico, che non passa da buildx.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$PWD/.docker}"
export DOCKER_BUILDKIT=0

IMAGE="namzero-cross-arm64"
NAM_DIR="${NAM_DIR:-$PWD/.nam}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "== costruisco l'immagine $IMAGE (una volta sola)"
  docker build -f docker/cross-arm64.dockerfile -t "$IMAGE" docker
fi

echo "== cross build"
docker run --rm \
  -v "$PWD":/src \
  -v "$NAM_DIR":/nam:ro \
  -w /src \
  "$IMAGE" bash -lc '
    cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/src/docker/aarch64-toolchain.cmake \
      -DNAM_DIR=/nam >/tmp/cfg.log 2>&1 || { tail -20 /tmp/cfg.log; exit 1; }
    # Prima la build (exit propagato), poi solo gli avvisi: il tee con grep
    # mascherava i fallimenti. Il comando file manca nell immagine: innocuo.
    if ! cmake --build build-arm64 -j"$(nproc)" >/tmp/build.log 2>&1; then
      tail -40 /tmp/build.log
      exit 1
    fi
    grep -E "error:|warning:" /tmp/build.log | head -20 || true
    ls -l build-arm64/namzero-lite
    command -v file >/dev/null 2>&1 && file build-arm64/namzero-lite || echo "file: comando assente (innocuo)"
  '
echo
echo "pronto: build-arm64/namzero-lite (aarch64). Copialo sul Pi con ./scripts/deploy.sh"
