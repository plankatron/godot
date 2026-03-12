#!/bin/bash
# NVIDIA RTX Path Tracing Demo Launcher
# Runs the Godot GI demo with path tracing enabled on RTX 2070
#
# Usage:
#   ./run_rtx_demo.sh           # Launch game (play mode)
#   ./run_rtx_demo.sh --editor  # Launch editor
#
# Controls (in game):
#   0-9  : Visualization modes (0=Full PT, 1=Mirror, 2=Normals, 8=Albedo, etc.)
#   B    : Cycle max bounces (1-4)
#   S    : Cycle samples per pixel (1, 2, 4)
#   R    : Toggle raytracing on/off
#   P    : Print current RT status + GPU capabilities
#   F5   : Run automated feature test (cycles all modes, 2s each)
#   WASD : Move camera
#   Mouse: Look around
#   Esc  : Toggle mouse capture

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GODOT_BIN="${SCRIPT_DIR}/bin/godot.linuxbsd.editor.x86_64"
PROJECT_DIR="/storage/projects/code/godot/godot-demo-projects/3d/global_illumination"

export DISPLAY="${DISPLAY:-:0}"

# NGX DLSS: Ensure runtime .so files are findable
# The static lib (libnvsdk_ngx.a) talks to the driver's libnvidia-ngx.so.1,
# which loads the DLSS model .so files from the app data path or LD_LIBRARY_PATH
NGX_DLSS_LIBS="${SCRIPT_DIR}/thirdparty/ngx/lib/linux_x86_64"
if [ -d "$NGX_DLSS_LIBS" ]; then
    export LD_LIBRARY_PATH="${NGX_DLSS_LIBS}:${LD_LIBRARY_PATH}"
    echo "DLSS:    Runtime .so path: $NGX_DLSS_LIBS"
fi

if [ ! -f "$GODOT_BIN" ]; then
    echo "ERROR: Godot binary not found at: $GODOT_BIN"
    echo "Build with: scons -j\$(nproc) platform=linuxbsd target=editor use_streamline=no"
    exit 1
fi

echo "=== NVIDIA RTX Path Tracing Demo ==="
echo "Binary:  $GODOT_BIN"
echo "Project: $PROJECT_DIR"
echo "GPU:     $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null || echo 'unknown')"
echo "Driver:  $(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null || echo 'unknown')"
echo ""

if [ "$1" = "--editor" ]; then
    echo "Launching EDITOR mode..."
    exec "$GODOT_BIN" --path "$PROJECT_DIR" --editor 2>&1
else
    echo "Launching GAME mode (path tracing enabled)..."
    echo "Press F5 for automated feature test, P for status"
    echo ""
    exec "$GODOT_BIN" --path "$PROJECT_DIR" 2>&1
fi
