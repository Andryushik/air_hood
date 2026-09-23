# HomeKit "offline again" — pairing storage wipe (research + plan)

Status: **IN PROGRESS** — steps 1 and 3 done (patched library flashed). Step 4 (re-pair in Apple Home) is yours; then step 5 verification.

---

## 1. Research

### Symptom (2026-09-23)

Hood screen shows WiFi, router shows it connected, Apple Home shows "No Response"; reboots don't help.

### Live evidence (192.168.2.151)

| Probe                | Result                                                                                                                            |
| -------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| ping                 | 0% loss (avg 38 ms)                                                                                                               |
| HTTP `:8080/status`  | OK — `{"on":false,"temp":22.7,"hum":57.8,"manual":false,"rssi":-67}`                                                              |
| HAP `:5556` connect  | OK                                                                                                                                |
| mDNS `_hap._tcp` TXT | **`sf=1`** (UNPAIRED), **`id=FF:CB:68:D4:6E:FD`** — in July the ID was `5A:9C:5C:7E:B4:F6`                                        |
| telnet banner        | fw `2026-07-12.5`, **`reset=Exception`**, uptime 90 s → 265 s three minutes later (one-off, not a loop), heap ~33 KB, `clients=0` |

A **new accessory ID** means the HomeKit storage sector was reformatted, not merely unpaired. (Removing the last admin pairing only sets `paired=false` and keeps the ID — `arduino_homekit_server.cpp:~2630`.) Apple Home still holds the pairing for the old ID, so it can never reconnect; rebooting cannot help because the pairing data is gone from the device.

### Root cause — bug in the library, `Arduino-HomeKit-ESP8266` v1.2.0, `src/storage.c` `compact_data()`

`compact_data()` runs from `homekit_storage_add_pairing()` / `homekit_storage_update_pairing()` when `find_empty_block()` finds no all-`0xFF` slot. Removed and updated pairings are zeroed (`memset 0`, lines 342/363), **not** erased, so free slots only ever decrease. After enough Apple Home pairing operations (home members, permission updates, hub changes — weeks), the next one triggers compaction:

1. **Silent wipe — `storage.c:244`** `if (homekit_storage_reset()) { … return -1; }`
   `homekit_storage_reset()` returns `homekit_storage_init()`, which returns **1 after a successful reformat** (line 124). So success is treated as failure and the function returns **after erasing the sector but before writing the data back** → accessory ID, accessory key and all pairings are lost. The running device keeps working from RAM (existing controllers other than the one just added lose access immediately).
2. **Out-of-bounds write / crash — `storage.c:231`** `memcpy(&data[PAIRINGS_ADDR + …], …)`
   `PAIRINGS_ADDR` is the absolute flash address (`0x3FB000 + 128`), used as an index into a 4 KB `malloc` buffer → write far outside the buffer → Exception or heap corruption. Should be `PAIRINGS_OFFSET`. Triggers when a tombstone precedes a valid pairing.

**Next reboot** (power blip, crash, OTA): `homekit_server_init()` (`arduino_homekit_server.cpp:3354-3369`) → magic OK → `homekit_storage_load_accessory_id()` reads `0xFF` → fails → `homekit_storage_reset()` → **new random ID + key, zero pairings → `sf=1`**. This matches both the July incident and today's.

### Upstream confirmation

`maximkulkin/esp-homekit` master `src/storage.c` (the code this library ports) has both fixed:

- `memcpy(&data[PAIRINGS_OFFSET + sizeof(pairing_data_t)*next_pairing_idx], …)`
- `if (homekit_storage_reset() <= 0) { … }`

The Mixiaoxiao port kept the old broken version; the repo is unmaintained (last commit `8a8e1a0`, a translation merge).

### Blast radius

The library is shared in `~/codes/Arduino/libraries/Arduino-HomeKit-ESP8266` (clean git checkout of Mixiaoxiao master @ `8a8e1a0`). Used by: **air_hood, blinds_homekit, shades_homekit, shades_homekit_nema**. All carry the same latent bug.

### Still unclear

- Cause of the one-off `reset=Exception` ~5 min before the probe. It is **not** compaction (the store currently holds 0 pairings). The current firmware only logs `ESP.getResetReason()`, not the exception cause/address, so it can't be decoded after the fact.

---

## 2. Plan

1. **Patch the shared library** `libraries/Arduino-HomeKit-ESP8266/src/storage.c` (2 lines, matching upstream):
   - `:231` `PAIRINGS_ADDR` → `PAIRINGS_OFFSET`
   - `:244` `if (homekit_storage_reset())` → `if (homekit_storage_reset() <= 0)`
   - Save the diff as `air_hood/docs/patches/homekit-storage-compact.patch` so it's tracked in this repo and re-applicable (a library reinstall/`git pull` would revert the in-place patch).
     ✅ DONE — decision: in-place patch + tracked `docs/patches/homekit-storage-compact.patch` (no fork). Verified in the clean-built object (`compact_data$isra$0`): memcpy dest = `data + 128 + 80·n` (no flash address), and `bgei a2, 1` right after `homekit_storage_reset` (so `<= 0` = failure).
2. **(Optional) crash diagnostics** — add `ESP.getResetInfo()` (exccause / epc1 / excvaddr) to the telnet banner in `RemoteLog.cpp`, so the next Exception can be decoded with `xtensa-lx106-elf-addr2line` against the build ELF. Bump `FW_VERSION` → `2026-09-23.1`.
   ⏭ SKIPPED — not chosen (minimal fix). `FW_VERSION` was still bumped to `2026-09-23.1` so the banner confirms the OTA push.
3. **Build air_hood and OTA-flash** to 192.168.2.151. The device is unpaired right now, so the reboot costs nothing.
   ✅ DONE — OTA-flashed; banner `fw=2026-09-23.1`; accessory ID `FF:CB:68:D4:6E:FD` unchanged across the OTA reboot (`sf=1`, awaiting re-pair).
4. **You re-pair in Apple Home** (only you can do this): remove the stale "Air Hood" (old name, No Response) → Add Accessory → "Range Hood" → code `281-42-814`.
   ⚠️ Removing it from Home deletes automations/scenes/room assignment tied to it — note them first and recreate after.
5. **Verify persistence**: after re-pair, mDNS shows `sf=0` with the new ID; then one reboot (OTA no-op push or power-cycle) → still `sf=0`, **same ID**, telnet `clients ≥ 1`.
   Limitation: the compaction path itself can't easily be exercised on the device (it needs ~16 pairing operations). The fix is verified by matching upstream's fixed code plus review.
6. **Other projects** (blinds/shades): they pick up the fix on their next compile+flash. Not rebuilding them now unless you ask.
7. **Commits**: none without your OK. The library patch lives outside this repo; only the `.patch` file and this plan would be committed here.

### Out of scope

- Making the library's init path non-destructive (it still regenerates the ID on genuine corruption — same as upstream).
- Finding the one-off Exception's root cause (step 2 only adds instrumentation for next time).

### Risks

### Risks

- In-place library patch reverted by a library reinstall/update → mitigated by the tracked `.patch` file.
- Re-pairing loses Home automations/scenes for this accessory (step 4 warning).
