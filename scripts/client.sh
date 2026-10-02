#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

docker run --rm -it \
  --network chatnet \
  --add-host=host.docker.internal:host-gateway \
  -v "$PROJECT_DIR:/work" \
  -w /work \
  chatlab:dev ./build/chat_client "${1:-host.docker.internal}" "${2:-9000}"