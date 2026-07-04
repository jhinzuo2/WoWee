# Warden Implementation

**Status**: Partial. Module download, crypto, native-format image parsing, relocation,
import binding scaffolding, callback stubs, and Unicorn dispatch exist. Full module
execution is still not guaranteed for strict servers.
**WoW Versions**: Classic/Turtle flow is the active target. Older 3.3.5a notes remain
useful for crypto and opcode background.

---

## Overview

Warden is WoW's client integrity checking system. The server sends encrypted modules
containing native x86 code; the client is expected to load and execute them, then
return check results.

Wowee handles this via Unicorn Engine CPU emulation — the x86 module is executed
directly in an emulated environment with Windows API hooks, without Wine or a Windows OS.

Native-client behavior confirmed in IDA:

- `WoW.exe` registers a world handler for `SMSG_WARDEN_DATA` (`0x2E6`).
- Once a Warden object is active, the main client does not compute Warden hash/check
  responses itself; it passes the decrypted payload to the module object's packet
  handler.
- Outbound Warden data is emitted by a callback that builds `CMSG_WARDEN_DATA`
  (`0x2E7`).
- The callable entry is not the image base. Native WoW resolves export ordinal `1`,
  calls that factory with the callback table, receives an object, then calls methods
  from the object's vtable.

---

## Loading Pipeline (8 steps)

```
1. MD5       - Verify encrypted module checksum matches server challenge
2. RC4       - Decrypt module payload with the per-module key from `MODULE_USE`
3. RSA-2048  - Verify module signature when possible
4. zlib      - Decompress the signed payload
5. Parse     - Decode the native Warden image format, not a normal PE header
6. Relocate  - Apply the image-internal relocation table
7. Bind      - Resolve image-internal import descriptors to Unicorn stubs
8. Init      - Resolve export ordinal `1`, call the factory, and initialize the returned object
```

---

## Native Image Format Pitfalls

This format is the biggest compatibility trap. It is not a PE file after decompression.

- The first 0x28 bytes are a Warden image header.
- Header fields include image size, relocation table offset/count, export table
  offset/count/base ordinal, import table offset/count, and section count.
- Section descriptors live after the 0x28-byte header in the compressed stream, but
  native WoW does not copy those descriptors into the mapped image.
- The copy stream starts after the descriptors. It uses 16-bit little-endian lengths
  that alternate between copied bytes and zero-filled gaps.
- Relocations live inside the mapped image, not after the copy stream. Two-byte
  relocation entries are big-endian deltas; entries with the high bit set are
  four-byte absolute big-endian offsets.
- Imports live inside the mapped image as `[dllNameRva, thunkRva]` descriptors.
  Thunks contain either a function-name RVA or an ordinal with the high bit set.

Treating this as `[size][copy/data/skip][relocs][imports]` produces plausible-looking
buffers but leaves function pointers unrelocated and causes jumps into unmapped
addresses during module factory execution.

---

## Unicorn Engine Execution

The Warden object factory and vtable methods are called inside an Unicorn x86 emulator with:

- Executable memory mapped at the module's load address
- A simulated stack
- Windows API interception for calls the module makes

Intercepted APIs include `VirtualAlloc`, `GetTickCount`, `Sleep`, `ReadProcessMemory`,
and other common Warden targets. Each hook returns a plausible value without
accessing real process memory.

---

## Module Cache

After the first load, modules are written to disk:

```
~/.local/share/wowee/warden_cache/<MD5>.wdn
```

The key for lookup is the MD5 of the encrypted module. The cache currently stores
the encrypted module bytes; the module key still comes from the server's `MODULE_USE`
packet for that session.

---

## Crypto Layer

| Algorithm | Purpose |
|-----------|---------|
| RC4 | Encrypt/decrypt Warden traffic (separate in/out ciphers) |
| MD5 | Module identity hash |
| SHA1 | HMAC and check hashes |
| RSA-2048 | Module signature verification |

Some private-server/Turtle modules do not verify against the stock Blizzard modulus.
Signature failure is logged but is not fatal so the module can still be parsed and tested.

---

## Opcodes

- `SMSG_WARDEN_DATA` = 0x2E6 — server sends module + checks
- `CMSG_WARDEN_DATA` = 0x2E7 — client sends results

---

## Turtle Notes

Turtle's auth/world path can accept character enumeration while Warden is still
negotiating. Do not assume silence after `HASH_REQUEST` is always safe or always
fatal; test against the live realm with `world_char_probe`:

```bash
WOWEE_WARDEN_WAIT_MS=8000 WOWEE_PROBE_TIMEOUT_MS=25000 scripts/probe_turtle_chars.sh "Eversong Wilds"
```

The wait period matters because the failure mode seen on Turtle was a disconnect a
few seconds after the character list, not an immediate auth failure.

---

## Check Responses

| Check type | Opcode | Notes |
|------------|--------|-------|
| Module info | 0x00 | Returns module status |
| Hash check | 0x01 | File/memory hash validation |
| Lua check | 0x02 | Anti-addon detection |
| Timing check | 0x04 | Speedhack detection |
| Memory scan | 0x05 | Memory scan results |

---

## Key Files

```
include/game/warden_handler.hpp      - Packet handler interface
src/game/warden_handler.cpp          - handleWardenData + module manager init
include/game/warden_module.hpp       - Module loader interface
src/game/warden_module.cpp           - 8-step pipeline
include/game/warden_emulator.hpp     - Emulator interface
src/game/warden_emulator.cpp         - Unicorn Engine executor + API hooks
include/game/warden_crypto.hpp       - Crypto interface
src/game/warden_crypto.cpp           - RC4 / key derivation
include/game/warden_memory.hpp       - PE image + memory patch interface
src/game/warden_memory.cpp           - PE loader, runtime globals patching
```

---

## Performance

- First check (cold, no cache): ~120ms
- Subsequent checks (cache hit): ~1-5ms

---

## Dependencies

Requires `libunicorn-dev` (Unicorn Engine). The client compiles without it but
falls back to crypto-only mode (check responses are fabricated, not executed).

---

## References

- [WoWDev Wiki - Warden](https://wowdev.wiki/Warden)
- [WoWDev Wiki - SMSG_WARDEN_DATA](https://wowdev.wiki/SMSG_WARDEN_DATA)
- [TrinityCore Warden](https://github.com/TrinityCore/TrinityCore/tree/3.3.5/src/server/game/Warden)

---

**Last Updated**: 2026-07-04
