#!/usr/bin/env bash
# Test end-to-end del pacchetto .deb arm64 in un container Debian bookworm
# arm64 con systemd come PID 1.
#
#   ./scripts/test-container.sh            # build .deb + immagine + test
#   SKIP_BUILD=1 ./scripts/test-container.sh   # usa i .deb gia' in dist/
#   KEEP_CONTAINER=1 ./scripts/test-container.sh
#
# Il container viene avviato con i flag richiesti da systemd in Docker:
#   --privileged --cgroupns=host \
#   -v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

# Docker Desktop vuole scrivere in ~/.docker (fuori dal workspace): si tiene la
# config nel repo, come scripts/cross-build.sh. Il builder classico evita
# buildx e i suoi file di stato fuori dalla workspace.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$REPO/.docker}"
export DOCKER_BUILDKIT=0

PLATFORM=linux/arm64
IMAGE=namzero-lite-test
CONTAINER="namzero-lite-test-$$"
BASE_VERSION="${BASE_VERSION:-$(tr -d '[:space:]' <VERSION)}"
UPGRADE_VERSION="${UPGRADE_VERSION:-${BASE_VERSION}+test1}"

BASE_DEB="namzero-lite_${BASE_VERSION}_arm64.deb"
UPGRADE_DEB="namzero-lite_${UPGRADE_VERSION}_arm64.deb"

cleanup() {
  if [ -n "${CONTAINER:-}" ]; then
    if [ "${KEEP_CONTAINER:-0}" = 1 ]; then
      echo "container lasciato attivo: $CONTAINER"
      echo "  docker exec -it $CONTAINER bash"
    else
      docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    fi
  fi
}
trap cleanup EXIT

command -v docker >/dev/null 2>&1 || {
  echo "errore: docker non trovato" >&2
  exit 1
}
if ! docker info >/dev/null 2>&1; then
  echo "errore: demone Docker non raggiungibile" >&2
  exit 1
fi

if [ "${SKIP_BUILD:-0}" != 1 ]; then
  echo "== build del .deb base ($BASE_VERSION)"
  ./scripts/build-deb.sh "$BASE_VERSION"
  echo "== build del .deb di upgrade ($UPGRADE_VERSION)"
  ./scripts/build-deb.sh "$UPGRADE_VERSION"
fi

for deb in "dist/$BASE_DEB" "dist/$UPGRADE_DEB"; do
  [ -f "$deb" ] || {
    echo "errore: manca $deb" >&2
    exit 1
  }
done

echo
echo "== build immagine di test ($PLATFORM)"
docker build --platform "$PLATFORM" \
  -f docker/Dockerfile.test \
  -t "$IMAGE" docker

echo
echo "== avvio container systemd"
docker run -d --name "$CONTAINER" \
  --platform "$PLATFORM" \
  --privileged --cgroupns=host \
  -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
  --tmpfs /run --tmpfs /run/lock \
  -v "$REPO/dist":/pkg:ro \
  "$IMAGE" >/dev/null

echo -n "   attendo systemd"
SYSTEMD_STATE=""
for _ in $(seq 1 240); do
  if ! docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null | grep -q true; then
    echo
    echo "errore: il container e' terminato durante il boot" >&2
    docker logs "$CONTAINER" 2>&1 | tail -80 || true
    exit 1
  fi
  SYSTEMD_STATE="$(docker exec "$CONTAINER" systemctl is-system-running 2>/dev/null || true)"
  case "$SYSTEMD_STATE" in
  running | degraded) break ;;
  esac
  echo -n "."
  sleep 1
done
echo " ($SYSTEMD_STATE)"
case "$SYSTEMD_STATE" in
running | degraded) ;;
*)
  echo "errore: systemd non pronto (stato: $SYSTEMD_STATE)" >&2
  docker exec "$CONTAINER" systemctl status --no-pager -l 2>&1 | head -80 || true
  exit 1
  ;;
esac

echo
echo "== check nel container"
set +e
docker exec -i "$CONTAINER" bash -s -- "$BASE_DEB" "$UPGRADE_DEB" \
  <scripts/test-deb-inside.sh
RC=$?
set -e

if [ "$RC" -ne 0 ]; then
  echo
  echo "== test FALLITI (exit $RC): log del container"
  docker exec "$CONTAINER" journalctl --no-pager -n 200 2>&1 | tail -120 || true
  exit "$RC"
fi

echo
echo "test container: OK"
