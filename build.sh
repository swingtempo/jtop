#!/usr/bin/env bash
# build.sh — configure, build, and (optionally) test jtop.
#
# Usage:
#   ./build.sh              # configure + build jtop + jtop_render_test (incremental)
#   ./build.sh test         # also run the headless render test (writes widget_test.png)
#   ./build.sh clean        # wipe the build directory
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR=build
JOBS=$(nproc)

# --- check dependencies -------------------------------------------------
missing=()
command -v cmake      >/dev/null || missing+=(cmake)
command -v make       >/dev/null || missing+=(make)
command -v g++        >/dev/null || missing+=(g++)
command -v pkg-config >/dev/null || missing+=(pkg-config)
pkg-config --exists x11   2>/dev/null || missing+=("x11 dev headers (e.g. libx11-dev)")
pkg-config --exists cairo 2>/dev/null || missing+=("cairo dev headers (e.g. libcairo2-dev)")

if [[ ${#missing[@]} -gt 0 ]]; then
    echo "ERROR: missing dependencies: ${missing[*]}" >&2
    echo "Debian/Ubuntu:  sudo apt install cmake g++ pkg-config libx11-dev libcairo2-dev" >&2
    echo "Fedora:         sudo dnf install cmake g++ pkgconf-pkg-config libX11-devel cairo-devel" >&2
    exit 1
fi

# --- commands -----------------------------------------------------------
clean() {
    echo ">>> cleaning ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
}

configure() {
    echo ">>> configuring (build type: Release)"
    cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
}

build() {
    echo ">>> building with ${JOBS} jobs"
    cmake --build "${BUILD_DIR}" -j "${JOBS}"
    echo
    echo "Built:"
    ls -lh "${BUILD_DIR}/jtop" "${BUILD_DIR}/jtop_render_test"
}

test() {
    echo ">>> running render test (headless, no X server needed)"
    cd "${BUILD_DIR}"
    ./jtop_render_test
    cd ..
    echo
    echo "Render test output: ${BUILD_DIR}/widget_test.png"
}

case "${1:-}" in
    clean)  clean ;;
    test)   configure; build; test ;;
    *)
        if [[ -d "${BUILD_DIR}" ]]; then
            # keep the cache for incremental builds; configure is idempotent
            cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
        else
            configure
        fi
        build
        ;;
esac

echo
echo "Done. Run the widget with:  ./build/jtop"
echo "(or './build/jtop --dump' / '--table' for terminal output — see README)"
