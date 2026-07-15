#!/bin/bash
set -euo pipefail

MACOS_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_DATA_ROOT="${HOME}/Library/Application Support/Wowee/Data"

# Asset extraction must live outside the signed app bundle. Prefer the user's
# writable extraction once it contains a manifest; otherwise use the small set
# of configuration files bundled with the application.
if [ -f "${USER_DATA_ROOT}/manifest.json" ] || \
   find "${USER_DATA_ROOT}/expansions" -mindepth 2 -maxdepth 2 \
       -name manifest.json -print -quit 2>/dev/null | grep -q .; then
    export WOW_DATA_PATH="${USER_DATA_ROOT}"
fi

cd "${MACOS_DIR}"
exec ./wowee_bin "$@"
