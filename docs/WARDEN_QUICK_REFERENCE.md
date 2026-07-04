# Warden Quick Reference

Warden is WoW's client integrity checking system. Wowee implements full Warden module execution
via Unicorn Engine CPU emulation — no Wine required.

---

## How It Works

Warden modules are encrypted native x86 Windows code blobs that the server delivers
at login. Classic/Turtle modules are not normal PE DLLs after decompression.

1. Server sends `SMSG_WARDEN_DATA` (0x2E6) with the encrypted module
2. Client decrypts: RC4 → RSA-2048 signature verify → zlib decompress
3. Parses the native Warden image header/copy stream, then applies image-internal relocations
4. Resolves export ordinal `1`, calls the factory, and dispatches packets through the returned object's vtable
5. Client responds with check results via module callbacks that send `CMSG_WARDEN_DATA` (0x2E7)

Common trap: do not treat the decompressed payload as `[size][data][relocs][imports]`
or as a PE file. Native WoW copies only the 0x28-byte header into the image, skips
the section descriptors, then alternates copy/zero spans until the image is filled.

---

## Server Compatibility

| Server type | Expected result |
|-------------|-----------------|
| Warden disabled | Works (no Warden packets) |
| AzerothCore (local) | Works |
| ChromieCraft | Should work |
| Warmane | Should work |
| Turtle WoW | Use `scripts/probe_turtle_chars.sh` with `WOWEE_WARDEN_WAIT_MS` to verify post-character-list disconnect behavior |

---

## Module Cache

Modules are cached after first download:

```
~/.local/share/wowee/warden_cache/<MD5>.wdn
```

First connection: ~120ms (download + decompress + emulate). Subsequent: ~1-5ms (load from cache).

---

## Dependency

Unicorn Engine is required for module execution:

```bash
sudo apt install libunicorn-dev   # Ubuntu/Debian
sudo dnf install unicorn-devel    # Fedora
sudo pacman -S unicorn            # Arch
```

The client builds without Unicorn (falls back to crypto-only responses), but will not pass
strict Warden enforcement in that mode.

---

## Key Files

```
include/game/warden_handler.hpp + src/game/warden_handler.cpp   - Packet handler
include/game/warden_module.hpp  + src/game/warden_module.cpp    - Module loader (8-step pipeline)
include/game/warden_emulator.hpp + src/game/warden_emulator.cpp - Unicorn Engine executor
include/game/warden_crypto.hpp  + src/game/warden_crypto.cpp    - RC4/MD5/SHA1/RSA crypto
include/game/warden_memory.hpp  + src/game/warden_memory.cpp    - PE image + memory patching
```

---

## Logs

```bash
grep -i warden logs/wowee.log
```

Key messages:
- `Warden: module loaded from cache` — cached path, fast startup
- `WardenModule: Warden object=` — factory returned a native-style module object
- `Warden: check response sent` — working correctly
- `packetsAfterGate=0` — server not responding after Warden exchange

---

## Check Types

| Opcode | Name | Response |
|--------|------|----------|
| 0x00 | Module info | `[0x00]` |
| 0x01 | Hash check | `[0x01][results]` |
| 0x02 | Lua check | `[0x02][0x00]` |
| 0x04 | Timing | `[0x04][timestamp]` |
| 0x05 | Memory scan | `[0x05][num][results]` |

---

**Last Updated**: 2026-07-04
