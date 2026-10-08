#!/usr/bin/env bash
set -e

# 项目根目录
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# 默认 host 和 port
HOST="${1:-127.0.0.1}"
PORT="${2:-9000}"

exec "$PROJECT_DIR/build/chat_client" "$HOST" "$PORT"