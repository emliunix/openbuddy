#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

missing=0

check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "MISSING: $1"
        missing=1
    else
        echo "  OK:    $1"
    fi
}

check_brew_pkg() {
    if brew list "$1" &>/dev/null; then
        echo "  OK:    $1 (brew)"
    else
        echo "MISSING: $1 (brew)"
        missing=1
    fi
}

echo "Checking buddy build dependencies..."

check_cmd cmake
check_cmd brew
check_brew_pkg sdl3
check_brew_pkg nlohmann-json
check_brew_pkg asio

if [ "$missing" -ne 0 ]; then
    echo ""
    echo "Install missing dependencies with:"
    echo "  brew install sdl3 nlohmann-json asio cmake"
    exit 1
fi

echo ""
echo "All dependencies present. Building..."
cmake -B buddy/build -S buddy
cmake --build buddy/build
echo "Done: buddy/build/buddy"
