#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
exec ./buddy/build/buddy --log-level=debug "$@" >> buddy.log 2>&1
