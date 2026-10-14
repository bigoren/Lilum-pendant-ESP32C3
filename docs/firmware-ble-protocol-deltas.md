# Lilum Mandalum Pendant — BLE Protocol Deltas & Current Contract

This document captures the actual, as-implemented BLE surface of the
ESP32-C3 Mandalum pendant firmware as of this commit, and the deltas
from `LilumV2/docs/firmware-ble-protocol.md` that the Android app team
needs to absorb.

The spec doc remains the *intended* protocol. This file is the
*shipped* protocol until the spec is updated.

---

## Part A — Deltas from the spec

### A1. Advertising payload — the 38-byte manufacturer-data block is no longer in the advert. It moved to a GATT characteristic.

**Why:** spec §3.1's 38 bytes of mfg-data + 2-byte company id + 1-byte
kind tag = 41 bytes, which does not fit in a 31-byte legacy `ADV_IND`.
The previous build worked around this by using BLE 5 extended
(non-legacy) advertising, which made the pendant invisible to iOS
CoreBluetooth and to nRF Connect with default scanner settings.

**What the wire actually carries now (legacy `ADV_IND`, 31 bytes total):**

| AD field                       | Bytes | Value                                        |
| ------------------------------ | ----- | -------------------------------------------- |
| Flags                          | 3     | `0x06` — LE General Discoverable + BR/EDR Not Supported |
| Complete list of 128-bit UUIDs | 18    | Lilum primary service `0000FF00-…`           |
| Shortened local name           | 10    | `"Mandalum"` (8 ASCII chars)                 |

- **PDU type:** `ADV_IND` (connectable, scannable, undirected). 1M PHY,
  primary channels 37/38/39.
- **Interval:** 100 – 110 ms (range `LILUM_ADV_ITVL_MIN/MAX_0_625` =
  160 / 176 in 0.625 ms units).
- **No manufacturer-specific data is present in the advert or scan
  response.** The app must *not* rely on `LilumAdvertParser` for
  `pairId`, `groupId`, `firmwareVersion`, `configWindowRemainingSec`,
  or `elementTypeIdHash` anymore — read them from GATT after connecting.
- **GAP device name (full):** `"Lilum Mandalum"` (readable via the
  standard `0x2A00` GAP characteristic and reflected in the connection
  banner of most scanners).

The advertising lifecycle described in spec §4 (advertise only during
the 120 s configuration window) is unchanged.

### A2. New Identity characteristic `FF15` carries the old advert payload.

| Char                  | UUID suffix | Props | Payload  | Notes                                                  |
| --------------------- | ----------- | ----- | -------- | ------------------------------------------------------ |
| **Config window info** | `FF15`      | R     | 41 bytes | Same byte layout the spec used for mfg-data, **prefixed with 2-byte company id + 1-byte kind tag**. |

Byte layout of the 41-byte read result:

```
offset  size  field                       notes
------  ----  --------------------------  --------------------------------------
   0      2   companyId                   u16 LE, 0xFFFF
   2      1   kind                        u8, 0x01 = config-window payload
   3      2   elementTypeIdHash           u16 LE (same algorithm as spec §3.2)
   5     16   pairId                      16-byte UUID, big-endian RFC 4122
  21     16   groupId                     16-byte UUID, big-endian RFC 4122
  37      1   firmwareVersionMajor        u8
  38      1   firmwareVersionMinor        u8
  39      1   firmwareVersionPatch        u8
  40      1   configWindowRemainingSec    u8, 0..120 — recomputed on every read
```

Bytes 3..40 are the original spec §3.1 body, **unchanged**, so the
existing decoder can be reused if the app strips the 3-byte prefix.

Recommended app flow:
- Read `FF15` once on connect for `pairId` / `groupId` / firmware version.
- Re-read once per second while the user is on the configuration screen
  to drive the 120-second countdown (or omit if the countdown UI is
  dropped).

### A3. Element type identifier mismatch with spec §7.

Firmware reports via `FF11`:

- string id: **`LILUM_MANDALUM_PENDANT_ESP32C3_V1`** (not `LILUM_PENDANT_V1`)
- 16-bit hash: **`0x8C26`** (not `0xB836`)

Either the app's `ElementTypeRegistry` adds a row for this id, or the
firmware constant is renamed. The hash algorithm itself (spec §3.2) is
unchanged and matches Java's `String.hashCode() & 0xFFFF` on the
current string.

### A4. `0x180F` Battery Service is not exposed yet.

Only the four custom services below are registered. The app's battery
indicator will show "no battery characteristic" until this is added.

### A5. `applyConfigAndExitWindow` payload tolerance.

Spec §5.4 says 0 B with `W` (with response). Firmware currently
accepts any payload length (it logs and proceeds). App should send an
empty write to stay forward-compatible if the firmware later tightens
validation.

### A6. `FF32` (shared/pair config) accepts writes but the pair-mirror behavior is not yet implemented.

Same wire format as `FF31`. Writing to it persists and applies the
config locally, but the master does not yet forward anything to a
paired follower over BLE.

---

## Part B — Full GAP + GATT contract as currently implemented

### B1. GAP (advertising) — config-window only

| Parameter         | Value                                                                                  |
| ----------------- | -------------------------------------------------------------------------------------- |
| Lifetime          | First 120 s after boot (`LILUM_CONFIG_WINDOW_SEC`). Paused while a phone is connected; resumes on disconnect if window not yet expired. |
| PDU               | Legacy `ADV_IND` (connectable + scannable + undirected). Built via NimBLE EXT_ADV API with `legacy_pdu=1`. |
| PHY               | 1M, channels 37/38/39                                                                  |
| Interval          | 100 – 110 ms                                                                            |
| Own address type  | Public/random as inferred by `ble_hs_id_infer_auto` (currently public)                 |
| AD bytes          | Flags `0x06`; complete 128-bit UUID list = `0000FF00-…`; shortened name `"Mandalum"`   |
| GAP device name   | `"Lilum Mandalum"` (set via `ble_svc_gap_device_name_set`)                              |
| Scan response     | Empty                                                                                  |

After the 120 s window expires, the radio either becomes a
non-connectable runtime master (extended adv with mfg-data, see B6), a
passive scanner (runtime follower), or silent (runtime idle).

### B2. GAP (connection)

The firmware requests, immediately on connect:

| Parameter            | Value                                          |
| -------------------- | ---------------------------------------------- |
| Connection interval  | 15 ms (12 × 1.25 ms, min = max)                |
| Slave latency        | 0                                              |
| Supervision timeout  | 4 s (400 × 10 ms)                              |
| MTU exchange         | initiated by firmware via `ble_gattc_exchange_mtu` |

Only one concurrent connection; additional connect attempts are
rejected by NimBLE.

### B3. GATT service tree

```
0000FF01-0000-1000-8000-00805F9B34FB   Identity service
  FF11  Element type id            R     UTF-8, "LILUM_MANDALUM_PENDANT_ESP32C3_V1"
  FF12  Firmware version           R     3 B: major, minor, patch  (currently 1.0.0)
  FF13  Hardware revision          R     UTF-8, "rev-A"
  FF14  Serial number              R     UTF-8, "MANDA-0001"
  FF15  Config window info         R     41 B, see A2

0000FF02-0000-1000-8000-00805F9B34FB   Binding service
  FF21  pairId                     R/W   16 B UUID; all-zero clears + forces pairRole=NONE
  FF22  pairRole                   R/W   1 B  (0=NONE, 1=MASTER, 2=FOLLOWER); rejects >2
  FF23  groupId                    R/W   16 B UUID
  FF24  groupRole                  R/W   1 B  (0=FOLLOWER, 1=MASTER); rejects >1
  FF25  groupSlotIndex             R/W   1 B  (0..5); rejects >5
  (all writes persisted to NVS before ACK)

0000FF03-0000-1000-8000-00805F9B34FB   Configuration service
  FF31  standalone config blob     W     14..256 B, see B4
  FF32  shared config blob (pair)  W     same format (mirroring not yet implemented)

0000FF04-0000-1000-8000-00805F9B34FB   Live Command service
  FF41  setLiveBrightness          WNR   1 B  brightness 0..100 (>100 clamped)
  FF42  previewAnimation           WNR   8 B  animation tuple (B5)
  FF43  testFlashlight             WNR   5 B  ARGB u32 LE + brightness u8
  FF44  applyAndExit               W     0 B  (any length accepted, see A5)
```

No characteristic exposes `INDICATE` or `NOTIFY`. No CCCDs. No
standard Battery Service.

### B4. `ElementConfig` blob (FF31 / FF32)

Same as spec §6.1: 14-byte header + `1 + 8·N1` auto playlist +
`1 + 8·N2` manual playlist. Max length `LILUM_CONFIG_BLOB_MAX_LEN`
= 256 bytes. The firmware decodes via `lilum_config_decode`; on any
structural failure the write is NACK'd with
`BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN` and the previous config remains
active. On success the blob is persisted to NVS before ACK.

### B5. Animation tuple

Same as spec §6.2: 8 bytes — `base, symmetry, p1, p2, p3, p4, p5, p6`.

### B6. Runtime-mode advertising / scanning (outside the 120 s window)

Not relevant to the phone app but listed for completeness:

- **Runtime master:** non-connectable BLE 5 *extended* advertisement
  on the same `0000FF00-…` service, with a 26-byte manufacturer-data
  field (`companyId | kind=0x02 | LilumRuntimeAdvPayload` = 23 B).
  Continuous, 100 – 110 ms interval. The phone should *not* try to
  connect to this; it is peer-to-peer between pendants.
- **Runtime follower:** continuous passive `ble_gap_disc` on 1M PHY.
- **Runtime idle:** radio silent.

### B7. Security

None. No pairing, no bonding, no encryption. Authentication is "the
user is holding the device and just power-cycled it". Matches spec §9.

---

## Part C — Source files where this contract is enforced

- [components/orchestra_ble/include/lilum_ble_protocol.h](../components/orchestra_ble/include/lilum_ble_protocol.h) — UUIDs, enums, sizes.
- [components/orchestra_ble/include/orchestra_payload.h](../components/orchestra_ble/include/orchestra_payload.h) — `LilumConfigAdvPayload` (used by `FF15` read) and `LilumRuntimeAdvPayload` (runtime master adv).
- [components/orchestra_ble/simple_BLE.cpp](../components/orchestra_ble/simple_BLE.cpp) — advertising lifecycle, GATT service table, all access callbacks.
- [components/orchestra_ble/lilum_config.cpp](../components/orchestra_ble/lilum_config.cpp) — config blob codec.

---

## TL;DR for the app team

1. **Discovery:** scan for service UUID
   `0000FF00-0000-1000-8000-00805F9B34FB`, advert/local-name
   `"Mandalum"`. iOS-friendly. No manufacturer data.
2. **Identity / countdown:** after connect, read `FF15` to get the
   41-byte blob containing `pairId`, `groupId`, firmware version, and
   remaining window seconds. Re-read while on the config screen if a
   live countdown is desired.
3. **Element-type registry:** add an entry for id
   `LILUM_MANDALUM_PENDANT_ESP32C3_V1`, hash `0x8C26`, or ask firmware
   to rename to `LILUM_PENDANT_V1`.
4. **Everything else** (Binding / Configuration / Live Command
   services and their byte layouts) is unchanged from the spec.
5. **Not yet implemented:** Battery Service `0x180F`, pair-mirror
   behavior of `FF32`, notifications / indications.
