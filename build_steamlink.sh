#!/bin/bash
set -e

TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [ -z "${MARVELL_SDK_PATH:-}" ]; then
    MARVELL_SDK_PATH="$(cd "${TOP}/../.." && pwd)"
fi
export MARVELL_SDK_PATH

source "${MARVELL_SDK_PATH}/setenv.sh"

if [ "$#" -gt 1 ]; then
    echo "Usage: $0 [RPG Maker MZ game directory]" >&2
    exit 2
fi

BUILD_DIR="${BUILD_DIR:-${TOP}/build-steamlink}"
GAME_DIR="${1:-${RMMZ_GAME_DIR:-}}"
CMAKE_ARGS=(
    -DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE}"
    -DCMAKE_C_COMPILER="${MARVELL_SDK_PATH}/toolchain/bin/${CROSS}gcc"
    -DCMAKE_CXX_COMPILER="${MARVELL_SDK_PATH}/toolchain/bin/${CROSS}g++"
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=/
    -DRMMZ_STEAMLINK=ON
    -DRMMZ_RENDER_BACKEND=gles2
    -DRMMZ_BUILD_TESTS=OFF
    -DRMMZ_RUNTIME_SHADERS=OFF
    -DRMMZ_GAME_DIR="${GAME_DIR}"
)

cmake -S "${TOP}" -B "${BUILD_DIR}" "${CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --target outsider --parallel "${JOBS:-2}"

APP_DIR="${TOP}/steamlink/apps/outsider"
mkdir -p "${APP_DIR}"
DESTDIR="${APP_DIR}" cmake --install "${BUILD_DIR}" --component runtime --prefix /

if [ -n "${OUTSIDER_ICON:-}" ]; then
    cp "${OUTSIDER_ICON}" "${APP_DIR}/icon.png"
    printf 'name=Outsider\nicon=icon.png\nrun=outsider\n' >"${APP_DIR}/toc.txt"
else
    printf 'name=Outsider\nrun=outsider\n' >"${APP_DIR}/toc.txt"
fi

mkdir -p "${TOP}/steamlink/apps"
tar -czf "${TOP}/steamlink/apps/outsider.tgz" -C "${APP_DIR}" .
echo "Steam Link package created: ${TOP}/steamlink/apps/outsider.tgz"
