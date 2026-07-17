#!/bin/bash
set -euo pipefail

APP_PATH="${1:?usage: sign_app.sh <app> [identity]}"
IDENTITY="${2:--}"

if [ "${IDENTITY}" = "-" ]; then
    codesign --force --deep --sign - "${APP_PATH}"
    exit 0
fi

# Sign every Mach-O component before the containing app bundle. Hardened
# runtime and a trusted timestamp are required for Apple notarization.
while IFS= read -r -d '' component; do
    if file -b "${component}" | grep -q 'Mach-O'; then
        codesign --force --sign "${IDENTITY}" \
            --options runtime --timestamp "${component}"
    fi
done < <(find "${APP_PATH}/Contents" -type f -print0)

codesign --force --sign "${IDENTITY}" \
    --options runtime --timestamp "${APP_PATH}"
