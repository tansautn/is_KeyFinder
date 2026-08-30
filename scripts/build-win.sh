#!/usr/bin/env bash
#*************************************************************************
# Build is_KeyFinder on Windows x64 via MSYS2 MINGW64.
#
# Run this from the *MINGW64* shell (title bar says "MINGW64", and
# `echo $MSYSTEM` prints MINGW64). It will:
#   1. install the required MSYS2 packages (Qt5, ffmpeg, taglib, fftw, ...)
#   2. build + install libkeyfinder into the MINGW64 prefix (not in the repos)
#   3. run qmake + mingw32-make to build the app (and, with --tests, the tests)
#
# Usage:
#   scripts/build-win.sh                 # full build (deps + libkeyfinder + app)
#   scripts/build-win.sh --tests         # also build & run the GoogleTest suite
#   scripts/build-win.sh --deploy        # copy DLLs so the exe runs outside MSYS2
#   scripts/build-win.sh --deps          # only install MSYS2 packages
#   scripts/build-win.sh --libkeyfinder  # only build/install libkeyfinder
#   scripts/build-win.sh --clean         # remove the build/ directory first
#   scripts/build-win.sh --no-deps       # skip pacman (assume deps present)
#
# Env overrides:
#   LIBKEYFINDER_REF=<tag|branch|sha>    # pin libkeyfinder (default: repo default branch)
#   JOBS=<n>                             # parallel build jobs (default: nproc)
#*************************************************************************
set -euo pipefail

# --- locate repo, regardless of where the script is invoked from ---------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build"
JOBS="${JOBS:-$(nproc)}"
LIBKEYFINDER_REF="${LIBKEYFINDER_REF:-}"
LIBKEYFINDER_URL="https://github.com/mixxxdj/libkeyfinder.git"

DO_DEPS=1
DO_LIBKF=1
DO_APP=1
DO_TESTS=0
DO_DEPLOY=0

for arg in "$@"; do
  case "$arg" in
    --deps)         DO_LIBKF=0; DO_APP=0 ;;
    --libkeyfinder) DO_DEPS=0; DO_APP=0 ;;
    --no-deps)      DO_DEPS=0 ;;
    --tests)        DO_TESTS=1 ;;
    --deploy)       DO_DEPLOY=1 ;;
    --clean)        echo ">> cleaning ${BUILD_DIR}"; rm -rf "${BUILD_DIR}" ;;
    -h|--help)      sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

# --- sanity: must be the MINGW64 shell -----------------------------------
if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
  echo "ERROR: run from the MINGW64 shell (MSYSTEM='${MSYSTEM:-unset}', need MINGW64)." >&2
  echo "       Launch 'MSYS2 MINGW64' from the Start menu, then re-run this script." >&2
  exit 1
fi

echo ">> repo:   ${REPO_ROOT}"
echo ">> build:  ${BUILD_DIR}"
echo ">> jobs:   ${JOBS}"
echo ">> prefix: ${MINGW_PREFIX}"
mkdir -p "${BUILD_DIR}"

# --- 1. dependencies ------------------------------------------------------
install_deps() {
  echo ">> installing MSYS2 packages"
  pacman -S --needed --noconfirm \
    git \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-make \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-qt5-base \
    mingw-w64-x86_64-qt5-tools \
    mingw-w64-x86_64-qt5-xmlpatterns \
    mingw-w64-x86_64-taglib \
    mingw-w64-x86_64-fftw \
    mingw-w64-x86_64-zlib \
    mingw-w64-x86_64-ffmpeg \
    mingw-w64-x86_64-gtest
}

# --- 2. libkeyfinder (built from source, installed into MINGW_PREFIX) -----
build_libkeyfinder() {
  if pkg-config --exists libkeyfinder 2>/dev/null; then
    echo ">> libkeyfinder already installed ($(pkg-config --modversion libkeyfinder)); skipping"
    return
  fi
  local src="${BUILD_DIR}/libkeyfinder"
  echo ">> fetching libkeyfinder (${LIBKEYFINDER_REF:-default branch})"
  rm -rf "${src}"
  if [[ -n "${LIBKEYFINDER_REF}" ]]; then
    git clone --depth 1 --branch "${LIBKEYFINDER_REF}" "${LIBKEYFINDER_URL}" "${src}"
  else
    git clone --depth 1 "${LIBKEYFINDER_URL}" "${src}"
  fi
  echo ">> building libkeyfinder"
  cmake -S "${src}" -B "${src}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX="${MINGW_PREFIX}"
  cmake --build "${src}/build" --parallel "${JOBS}"
  echo ">> installing libkeyfinder into ${MINGW_PREFIX}"
  cmake --install "${src}/build"
}

# --- 3. deploy: bundle DLLs so the exe runs outside MSYS2 -----------------
deploy_app() {
  local dir="$1"
  local exe="${dir}/KeyFinder.exe"
  [[ -f "${exe}" ]] || { echo "ERROR: ${exe} not found; build first." >&2; return 1; }

  echo ">> deploying Qt plugins + DLLs into ${dir}"
  windeployqt-qt5 --release --no-translations --no-opengl-sw --no-angle \
    --no-system-d3d-compiler "${exe}"

  ldd "${exe}" 2>/dev/null | grep -i mingw64 | awk '{print $3}' | sort -u | while read dll; do
    local base
    base=$(basename "${dll}")
    [[ -f "${dir}/${base}" ]] || cp "${dll}" "${dir}/"
  done

  # transitive pass: DLLs of DLLs
  for existing in "${dir}"/*.dll; do
    ldd "${existing}" 2>/dev/null | grep -i mingw64 | awk '{print $3}' | sort -u | while read dll; do
      local base
      base=$(basename "${dll}")
      [[ -f "${dir}/${base}" ]] || cp "${dll}" "${dir}/"
    done
  done

  echo ">> deploy complete ($(ls "${dir}"/*.dll 2>/dev/null | wc -l) DLLs)"
}

# --- 4. the app (and optionally the tests) --------------------------------
build_app() {
  local out="${BUILD_DIR}/app"
  mkdir -p "${out}"
  echo ">> qmake (app)"
  ( cd "${out}" && qmake-qt5 "CONFIG+=msys2" "${REPO_ROOT}/is_KeyFinder.pro" )
  echo ">> make (app)"
  ( cd "${out}" && mingw32-make -j"${JOBS}" )
  echo ">> built: ${out}/release/KeyFinder.exe (or ${out}/KeyFinder.exe)"

  if [[ "${DO_DEPLOY}" == 1 ]]; then
    deploy_app "${out}/release"
  fi
}

build_tests() {
  local out="${BUILD_DIR}/test"
  mkdir -p "${out}"
  echo ">> qmake (tests)"
  ( cd "${out}" && qmake-qt5 "CONFIG+=msys2 test" "${REPO_ROOT}/is_KeyFinder.pro" )
  echo ">> make (tests)"
  ( cd "${out}" && mingw32-make -j"${JOBS}" )
  echo ">> running tests"
  # test resources are referenced as ../is_KeyFinder/test-resources/... so run from a
  # sibling layout; fall back to running in-place if that path isn't present.
  ( cd "${out}" && ./KeyFinderTests.exe || ./release/KeyFinderTests.exe )
}

[[ "${DO_DEPS}"  == 1 ]] && install_deps
[[ "${DO_LIBKF}" == 1 ]] && build_libkeyfinder
[[ "${DO_APP}"   == 1 ]] && build_app
[[ "${DO_TESTS}" == 1 ]] && build_tests

echo ">> done"
