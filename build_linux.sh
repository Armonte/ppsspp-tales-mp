#!/usr/bin/env bash
# Linux build of the SDL3 frontend (+ headless). Sources stay on /mnt/c; build trees live on ext4 for speed.
# SDL3 and SDL3_ttf are built once from their release tags into $SDL3_PREFIX (Ubuntu 24.04 ships no SDL3).
# Usage: ./build_linux.sh [configure]
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
BUILD="${PPSSPP_RB_BUILD:-$HOME/build/ppsspp-rollback-linux}"
SDL3_PREFIX="${SDL3_PREFIX:-$HOME/opt/sdl3}"
SDL3_TAG="release-3.4.16"
SDL3_TTF_TAG="release-3.2.2"

build_dep() { # name repo tag
	local name=$1 repo=$2 tag=$3 dir="$HOME/src/deps/$1"
	[[ -d "$dir" ]] || git clone -q --depth 1 --branch "$tag" "$repo" "$dir"
	cmake -S "$dir" -B "$dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$SDL3_PREFIX" -DCMAKE_PREFIX_PATH="$SDL3_PREFIX" \
		-DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDLTTF_SAMPLES=OFF -DSDLTTF_VENDORED=OFF >/dev/null
	cmake --build "$dir/build" --target install >/dev/null
}
if [[ ! -f "$SDL3_PREFIX/lib/cmake/SDL3/SDL3Config.cmake" && ! -f "$SDL3_PREFIX/lib/x86_64-linux-gnu/cmake/SDL3/SDL3Config.cmake" ]]; then
	build_dep SDL3 https://github.com/libsdl-org/SDL.git "$SDL3_TAG"
fi
if ! ls "$SDL3_PREFIX"/lib*/cmake/SDL3_ttf* "$SDL3_PREFIX"/lib/*/cmake/SDL3_ttf* >/dev/null 2>&1; then
	build_dep SDL3_ttf https://github.com/libsdl-org/SDL_ttf.git "$SDL3_TTF_TAG"
fi

if [[ "${1:-}" == "configure" || ! -f "$BUILD/build.ninja" ]]; then
	cmake -S "$SRC" -B "$BUILD" -G Ninja \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DCMAKE_PREFIX_PATH="$SDL3_PREFIX" \
		-DVCPKG_MANIFEST_MODE=OFF \
		-DUNITTEST=OFF -DHEADLESS=ON
fi
cmake --build "$BUILD" --target PPSSPPSDL PPSSPPHeadless
