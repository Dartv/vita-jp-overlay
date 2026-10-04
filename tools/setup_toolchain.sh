#!/bin/bash
# One-time macOS (or Linux) setup for the Vita build:
#   1. vitasdk          -> $VITASDK (default ~/vitasdk)
#   2. taihen headers   (vdpm)
#   3. ScePaf C++ headers + stubs (Princess-of-Sleeping/vitasdk-paf-component,
#      patched: macOS has no `nproc`, and its emd2yml CMake passes -Wl,-q to
#      the host linker)
#   4. psp2cxml-tool    (RCO compiler)
# The cloned sources are pinned to known commits.
# Host tools: cmake, ninja, git, python3 (Pillow for tools/make_test_screens.py).
set -euo pipefail
export VITASDK=${VITASDK:-$HOME/vitasdk}
export PATH=$VITASDK/bin:$PATH
WORK=${WORK:-$(cd "$(dirname "$0")/.." && pwd)/.refs/toolchain}
mkdir -p "$WORK"
ncpu=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)

if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
  curl -fsSL https://raw.githubusercontent.com/vitasdk/vdpm/master/bootstrap-vitasdk.sh -o "$WORK/bootstrap-vitasdk.sh"
  yes | bash "$WORK/bootstrap-vitasdk.sh" --install-dir "$VITASDK"
fi
[ -f "$VITASDK/arm-vita-eabi/include/taihen.h" ] || yes | vdpm install taihen

if [ ! -d "$VITASDK/arm-vita-eabi/include/paf/widget" ]; then
  cd "$WORK"
  [ -d vitasdk-paf-component ] || git clone -q https://github.com/Princess-of-Sleeping/vitasdk-paf-component
  cd vitasdk-paf-component
  git checkout -q 9a68b07aa2d7247a756765bf154f292cda20aed3
  rm -rf build final
  mkdir -p build && cd build
  git clone -q https://github.com/GrapheneCt/ScePaf-RE
  (cd ScePaf-RE && git checkout -q b76d6ae00516b107d37e7e8a1a16211e3b729a07 && patch -p2 < ../../vitasdk_2.patch)
  mkdir -p build_emd2yml build_libpaf
  cc -O2 -c ../libpaf/emd2yml/src/sha1.c -o build_emd2yml/sha1.o
  c++ -std=c++11 -O2 -fshort-wchar ../libpaf/emd2yml/src/main.cpp build_emd2yml/sha1.o -o build_emd2yml/emd2yml
  (cd build_libpaf && ../build_emd2yml/emd2yml > ScePaf.yml && vita-libs-gen-2 -yml=./ScePaf.yml -output=./ -cmake=true \
     && cmake . >/dev/null && make -j"$ncpu" >/dev/null)
  cd ..
  # install.sh copies headers/stubs once build/ exists; it skips the rebuild.
  sed -i.bak 's/\$(nproc)/'"$ncpu"'/' install.sh
  bash install.sh
  cp build/build_libpaf/*.a "$VITASDK/arm-vita-eabi/lib/"
fi

if ! command -v psp2cxml-tool >/dev/null; then
  cd "$WORK"
  [ -d psp2cxml-tool ] || git clone -q https://github.com/Princess-of-Sleeping/psp2cxml-tool
  git -C psp2cxml-tool checkout -q d971a9a9892c122b4edef15d335326b2a551fec4
  cmake -S psp2cxml-tool -B psp2cxml-tool/mbuild -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null && cmake --build psp2cxml-tool/mbuild -j"$ncpu" >/dev/null
  echo "psp2cxml-tool: $WORK/psp2cxml-tool/mbuild/psp2cxml-tool (add to PATH or keep in .refs)"
fi
echo "toolchain ready: VITASDK=$VITASDK"
