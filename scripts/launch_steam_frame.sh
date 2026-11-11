#!/usr/bin/env bash
set -euo pipefail
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
[[ $data_home == /* ]] || data_home="$HOME/.local/share"
if [[ ${1:-} == --vr ]]; then
  shift
  exec python3 "$app_root/scripts/mgs_alpha.py" --user-dir "$data_home/mgs-recomp-frame" run --vr "$@"
fi
exec python3 "$app_root/scripts/mgs_alpha.py" --user-dir "$data_home/mgs-recomp-frame" --big-picture "$@"
