#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cfg="${WOWEE_LOGIN_CFG:-$HOME/.wowee/login.cfg}"
probe="${WOWEE_AUTH_PROBE:-$repo_root/build/bin/auth_login_probe}"

if [[ ! -x "$probe" ]]; then
  echo "auth_login_probe is missing or not executable: $probe" >&2
  echo "Run: cmake --build build --target auth_login_probe" >&2
  exit 2
fi

if [[ ! -f "$cfg" ]]; then
  echo "Login config not found: $cfg" >&2
  exit 2
fi

active="${WOWEE_AUTH_SERVER:-$(awk -F= '$1 == "active" {print $2; found=1; exit} END {exit found ? 0 : 1}' "$cfg" || true)}"
if [[ -z "$active" ]]; then
  echo "No active auth server found in $cfg" >&2
  exit 2
fi

section="[server $active]"
username="${WOWEE_AUTH_USER:-$(awk -F= -v section="$section" '
  $0 == section {in_section=1; next}
  /^\[/ {in_section=0}
  in_section && $1 == "username" {print $2; found=1; exit}
  END {exit found ? 0 : 1}
' "$cfg" || true)}"

password="${WOWEE_AUTH_PASSWORD:-$(awk -F= -v section="$section" '
  $0 == section {in_section=1; next}
  /^\[/ {in_section=0}
  in_section && $1 == "password" {print substr($0, index($0, "=") + 1); found=1; exit}
  END {exit found ? 0 : 1}
' "$cfg" || true)}"

password_hash="${WOWEE_AUTH_HASH:-$(awk -F= -v section="$section" '
  $0 == section {in_section=1; next}
  /^\[/ {in_section=0}
  in_section && $1 == "password_hash" {print $2; found=1; exit}
  END {exit found ? 0 : 1}
' "$cfg" || true)}"

if [[ -z "$username" ]]; then
  echo "No username found for $active in $cfg" >&2
  exit 2
fi

if [[ -z "$password" && -z "$password_hash" ]]; then
  if [[ ! -t 0 ]]; then
    echo "No password or password_hash found for $active in $cfg; set WOWEE_AUTH_PASSWORD/WOWEE_AUTH_HASH or add one under $section" >&2
    exit 2
  fi
  read -rsp "Password for $username: " password
  echo
fi

host="$active"
port="3724"
if [[ "$active" == *:* ]]; then
  host="${active%:*}"
  port="${active##*:}"
fi

major="${WOWEE_AUTH_MAJOR:-1}"
minor="${WOWEE_AUTH_MINOR:-18}"
patch="${WOWEE_AUTH_PATCH:-1}"
build="${WOWEE_AUTH_BUILD:-7272}"
proto="${WOWEE_AUTH_PROTO:-3}"
locale="${WOWEE_AUTH_LOCALE:-zhCN}"
integrity_mode="${WOWEE_INTEGRITY_MODE:-file}"
integrity_dir="${WOWEE_INTEGRITY_DIR:-$HOME/Downloads/TurtleWoW}"
integrity_exe="${WOWEE_INTEGRITY_EXE:-WoW.exe}"

echo "Probing $host:$port as $username, client v$major.$minor.$patch build $build locale $locale"
auth_args=("$probe" "$host" "$port" "$username" \
  "$major" "$minor" "$patch" "$build" "$proto" "$locale")
if [[ -n "$password" ]]; then
  auth_args+=(--password-stdin)
else
  auth_args+=(--hash "$password_hash")
fi
auth_args+=(--proof legacy \
  --integrity "$integrity_mode" \
  --misc-dir "$integrity_dir" \
  --integrity-exe "$integrity_exe" \
  --realm-list)

if [[ -n "$password" ]]; then
  printf '%s\n' "$password" | "${auth_args[@]}"
else
  "${auth_args[@]}"
fi
