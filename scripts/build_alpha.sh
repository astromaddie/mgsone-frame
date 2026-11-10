#!/usr/bin/env bash
# Private source build. Retail inputs and generated translations stay ignored.
set -euo pipefail
project_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
frame=OFF
default_jobs=4
if [[ ${1:-} == --steam-frame ]]; then
  frame=ON
  default_jobs=2
  shift
fi
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 [--steam-frame] /path/to/USA-v1.0-disc1.cue /path/to/USA-v1.0-disc2.cue" >&2
  exit 2
fi
if [[ $frame == ON && ( $(uname -s) != Linux || $(uname -m) != aarch64 ) ]]; then
  echo "The Steam Frame build runs in an ARM64 Linux environment." >&2
  exit 2
fi
mkdir -p .local data
alpha_build_profile="$project_root/data/alpha-build"
if [[ $frame == ON ]]; then
  export CC="${CC:-clang}" CXX="${CXX:-clang++}"
  data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
  [[ $data_home == /* ]] || data_home="$HOME/.local/share"
  alpha_build_profile="$data_home/mgs-recomp-frame"
fi
if [[ -f "$alpha_build_profile/installation.json" ]]; then
  echo "Reusing verified USA v1.0 build import."
  python3 scripts/mgs_alpha.py --user-dir "$alpha_build_profile" verify
else
  python3 scripts/mgs_alpha.py --user-dir "$alpha_build_profile" import "$1" "$2"
fi
alpha_media_root="$(python3 - "$alpha_build_profile" <<'PY'
import sys
sys.path.insert(0, 'scripts')
from mgs_alpha import installed, user_paths
print(installed(user_paths(override=sys.argv[1]))[0])
PY
)"
cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMGS_STEAM_FRAME="$frame" -DPS1RECOMP_USE_STUB=ON -DPS1RECOMP_DIAGNOSTIC_FAST_COMPILE=ON
cmake --build build --target ps1Analyzer ps1Recomp --parallel "${BUILD_JOBS:-$default_jobs}"
python3 scripts/prepare_native.py --campaign --exe "$alpha_media_root/SLUS_005.94" \
  --cue "$alpha_media_root/disc1.cue" --disc2-cue "$alpha_media_root/disc2.cue"
build/ps1Recomp/ps1Recomp data/mgs.usa.native.toml engine/ps1Runtime/src/recompiled_out.cpp
python3 scripts/install_decomp_bridges.py --executable "$alpha_media_root/SLUS_005.94"
cmake -S engine -B build -G Ninja -DPS1RECOMP_USE_STUB=OFF
cmake --build build --target ps1Runtime mgsFrontend mgsController_tests ps1Runtime_tests --parallel "${BUILD_JOBS:-$default_jobs}"
build/ps1Test/mgsController_tests --gtest_brief=1
build/ps1Test/ps1Runtime_tests --gtest_brief=1
python3 -m unittest discover -s scripts/tests
if [[ $frame == ON ]]; then
  echo "Steam Frame baseline build and unit tests complete. See docs/STEAM_FRAME.md for the first launch."
else
  echo "Native build and unit tests complete. Package with scripts/package_alpha.py OUTPUT_DIRECTORY."
fi
