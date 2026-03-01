#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --stl-dir <dir_with_stl_files> --results-dir <parent_results_dir> [--ext stl]"
  echo
  echo "Runs from the FluidX3D repo root and invokes ./make.sh for each STL:"
  echo "  ./make.sh \"<abs_stl_path>\" \"<abs_output_dir>\""
  exit 1
}

STL_DIR=""
RESULTS_PARENT=""
EXT="stl"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stl-dir) STL_DIR="${2:-}"; shift 2 ;;
    --results-dir) RESULTS_PARENT="${2:-}"; shift 2 ;;
    --ext) EXT="${2:-stl}"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "Unknown arg: $1"; usage ;;
  esac
done

[[ -n "$STL_DIR" && -n "$RESULTS_PARENT" ]] || usage

# Must be run from FluidX3D repo root (expects make.sh here)
if [[ ! -f "./make.sh" ]]; then
  echo "[ERROR] ./make.sh not found. Run this script from the FluidX3D repo root."
  exit 2
fi

# Resolve absolute paths
if command -v realpath >/dev/null 2>&1; then
  STL_DIR_ABS="$(realpath "$STL_DIR")"
  RESULTS_PARENT_ABS="$(realpath -m "$RESULTS_PARENT")"
else
  # Fallback (less robust) if realpath isn't available
  STL_DIR_ABS="$(cd "$STL_DIR" && pwd)"
  RESULTS_PARENT_ABS="$(mkdir -p "$RESULTS_PARENT"; cd "$RESULTS_PARENT" && pwd)"
fi

mkdir -p "$RESULTS_PARENT_ABS"

shopt -s nullglob

# Collect STL files (non-recursive). Change to **/*."$EXT" for recursive.
stls=( "$STL_DIR_ABS"/*."$EXT" )

if [[ ${#stls[@]} -eq 0 ]]; then
  echo "[ERROR] No *.${EXT} files found in: $STL_DIR_ABS"
  exit 3
fi

echo "[INFO] Found ${#stls[@]} *.${EXT} files in: $STL_DIR_ABS"
echo "[INFO] Writing results under: $RESULTS_PARENT_ABS"
echo

for stl in "${stls[@]}"; do
  base="$(basename "$stl")"
  stem="${base%.*}"

  # Per-STL output dir (child of results parent)
  out_dir="$RESULTS_PARENT_ABS/$stem/"
  mkdir -p "$out_dir"

  echo "[RUN] $base"
  echo "      STL : $stl"
  echo "      OUT : $out_dir"

  # Run FluidX3D via make.sh with required args:
  #   [absolute stl path] [absolute output folder]
  ./make.sh "$stl" "$out_dir"

  echo "[DONE] $base"
  echo
done
