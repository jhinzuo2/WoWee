#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cfg="${WOWEE_LOGIN_CFG:-$HOME/.wowee/login.cfg}"
probe="${WOWEE_WORLD_CHAR_PROBE:-$repo_root/build/bin/world_char_probe}"

read_cfg_value() {
  local key="$1"
  local section="${2:-}"
  if [[ -n "$section" ]]; then
    awk -F= -v section="$section" -v key="$key" '
      $0 == section {in_section=1; next}
      /^\[/ {in_section=0}
      in_section && $1 == key {print substr($0, index($0, "=") + 1); found=1; exit}
      END {exit found ? 0 : 1}
    ' "$cfg" || true
  else
    awk -F= -v key="$key" '
      $1 == key {print substr($0, index($0, "=") + 1); found=1; exit}
      END {exit found ? 0 : 1}
    ' "$cfg" || true
  fi
}

if [[ ! -x "$probe" ]]; then
  echo "world_char_probe is missing or not executable: $probe" >&2
  echo "Run: cmake --build build --target world_char_probe" >&2
  exit 2
fi

if [[ ! -f "$cfg" ]]; then
  echo "Login config not found: $cfg" >&2
  exit 2
fi

active="${WOWEE_AUTH_SERVER:-$(read_cfg_value active)}"
if [[ -z "$active" ]]; then
  echo "No active auth server found in $cfg" >&2
  exit 2
fi

section="[server $active]"
username="${WOWEE_AUTH_USER:-$(read_cfg_value username "$section")}"
password="${WOWEE_AUTH_PASSWORD:-$(read_cfg_value password "$section")}"
password_hash="${WOWEE_AUTH_HASH:-$(read_cfg_value password_hash "$section")}"

if [[ -z "$username" ]]; then
  echo "No username found for $active in $cfg" >&2
  exit 2
fi

if [[ -z "$password" && -z "$password_hash" ]]; then
  if [[ ! -t 0 ]]; then
    echo "No password or password_hash found for $active in $cfg" >&2
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

realm="${WOWEE_REALM_NAME:-${1:-Eversong Wilds}}"
integrity_dir="${WOWEE_INTEGRITY_DIR:-$HOME/Downloads/TurtleWoW}"
integrity_exe="${WOWEE_INTEGRITY_EXE:-WoW.exe}"
timeout_ms="${WOWEE_PROBE_TIMEOUT_MS:-15000}"
warden_wait_ms="${WOWEE_WARDEN_WAIT_MS:-0}"

args=("$probe" "$host" "$port" "$username"
  --version "${WOWEE_AUTH_VERSION:-1.18.1}"
  --build "${WOWEE_AUTH_BUILD:-7272}"
  --world-build "${WOWEE_WORLD_BUILD:-5875}"
  --logon-proto "${WOWEE_LOGON_PROTO:-8}"
  --proto "${WOWEE_AUTH_PROTO:-3}"
  --locale "${WOWEE_AUTH_LOCALE:-zhCN}"
  --integrity "${WOWEE_INTEGRITY_MODE:-file}"
  --misc-dir "$integrity_dir"
  --integrity-exe "$integrity_exe"
  --realm "$realm"
  --opcodes "${WOWEE_OPCODE_JSON:-$repo_root/Data/expansions/turtle/opcodes.json}"
  --timeout-ms "$timeout_ms"
  --warden-wait-ms "$warden_wait_ms")

echo "Probing characters on realm '$realm' via $host:$port as $username"
if [[ -n "$password" ]]; then
  args+=(--password-stdin)
  printf '%s\n' "$password" | "${args[@]}"
else
  args+=(--hash "$password_hash")
  "${args[@]}"
fi
