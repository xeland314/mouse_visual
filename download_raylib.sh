#!/bin/sh
set -eu

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
RAYLIB_VERSION="4.5.0"
RAYLIB_TARBALL="raylib-${RAYLIB_VERSION}.tar.gz"
RAYLIB_URL="https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz"
SRC_DIR="$ROOT_DIR/raylib-${RAYLIB_VERSION}"
BUILD_DIR="$ROOT_DIR/.raylib_build"
LINUX_BUILD="$BUILD_DIR/build-linux"
WIN_BUILD="$BUILD_DIR/build-win"
REPO_LIB="$ROOT_DIR/lib"
REPO_LIB_MINGW="$ROOT_DIR/lib_mingw-w64"
REPO_INCLUDE="$ROOT_DIR/include"

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

download_file() {
  url="$1"
  dest="$2"
  if command_exists curl; then
    curl -L -o "$dest" "$url"
  elif command_exists wget; then
    wget -O "$dest" "$url"
  else
    echo "ERROR: curl o wget requerido para descargar raylib" >&2
    exit 1
  fi
}

ensure_directory() {
  if [ ! -d "$1" ]; then
    mkdir -p "$1"
  fi
}

echo "Descargando raylib ${RAYLIB_VERSION}..."
ensure_directory "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -f "$BUILD_DIR/$RAYLIB_TARBALL" ]; then
  download_file "$RAYLIB_URL" "$BUILD_DIR/$RAYLIB_TARBALL"
fi

if [ ! -d "$SRC_DIR" ]; then
  tar -xzf "$BUILD_DIR/$RAYLIB_TARBALL" -C "$BUILD_DIR"
fi

ensure_directory "$REPO_LIB"
ensure_directory "$REPO_LIB_MINGW"
ensure_directory "$REPO_INCLUDE"

for header in raylib.h rlgl.h raymath.h; do
  if [ ! -f "$REPO_INCLUDE/$header" ]; then
    cp "$SRC_DIR/$header" "$REPO_INCLUDE/"
    echo "Copiado $header a include/"
  fi
done

build_raylib() {
  build_dir="$1"
  shift
  echo "Construyendo raylib en $build_dir..."
  ensure_directory "$build_dir"
  cmake -S "$SRC_DIR" -B "$build_dir" "$@"
  cmake --build "$build_dir" --target raylib
}

if ! command_exists cmake; then
  echo "ERROR: cmake no está instalado" >&2
  exit 1
fi

# Build Linux static library
build_raylib "$LINUX_BUILD" \
  -DBUILD_SHARED_LIBS=OFF \
  -DOPENGL_VERSION=33 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_GAMES=OFF
cp "$LINUX_BUILD/src/libraylib.a" "$REPO_LIB/"

echo "Biblioteca Linux instalada en $REPO_LIB/libraylib.a"

# Build Windows static library if toolchain is available
if command_exists x86_64-w64-mingw32-gcc && command_exists x86_64-w64-mingw32-windres; then
  build_raylib "$WIN_BUILD" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DBUILD_SHARED_LIBS=OFF \
    -DOPENGL_VERSION=33 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_GAMES=OFF \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
  cp "$WIN_BUILD/src/libraylib.a" "$REPO_LIB_MINGW/"
  echo "Biblioteca Windows instalada en $REPO_LIB_MINGW/libraylib.a"
else
  echo "Herramienta MinGW no encontrada; omitiendo compilación Windows." >&2
  echo "Instala x86_64-w64-mingw32-gcc y x86_64-w64-mingw32-windres para compilar Windows." >&2
fi

echo "Proceso completado. Usa make o make windows según corresponda."
