#!/bin/bash
cd "$(dirname "$0")"
DISPLAY=:0 LD_LIBRARY_PATH="./thirdparty/ngx/lib/linux_x86_64:${LD_LIBRARY_PATH}" \
  ./bin/godot.linuxbsd.editor.dev.x86_64 --path /storage/projects/code/godot/godot-demo-projects/3d/global_illumination "$@"
