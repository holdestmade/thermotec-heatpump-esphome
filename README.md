# Thermotec / PHNIX Pool Heat Pump — ESPHome ↔ Home Assistant Bridge

Control and monitor a **Thermotec** (PHNIX-manufactured) pool heat pump from
**Home Assistant** over **Bluetooth Low Energy (BLE)**, with no cloud account
and no dependency on the manufacturer's *AquaTemp* app.

An ESP32 board running [ESPHome](https://esphome.io/) connects to the heat
pump's BLE module, speaks its Modbus-over-BLE protocol, and exposes the pump
as native Home Assistant entities — temperatures, set-points, power, mode,
output relays, faults and firmware info.

> The full reverse-engineered protocol is documented separately in
> **[Protocol.md](Protocol.md)** — read that if you want to understand or
> extend the register map.

---

## Features

- **Power** on/off
- **Target temperature** (mode-aware — writes to the Heat / Cool / Auto
  register that matches the current mode)
- **Mode** select: Heat / Cool / Auto
- **Climate entity** (`Pool Heat Pump`) tying the above together
- **Live temperatures**: inlet water, outlet water, coil, ambient, exhaust
- **Diagnostics**: heating/cooling/auto set-points, min/max limits,
  defrost type, pump mode, mode type
- **Output relays**: compressor, circulating pump, high fan, 4-way valve
- **Fault reporting**: per-sensor P-code faults (P01/P02/P04/P05/P81),
  bitmasked E-codes (E01/E02/E03), a rolled-up **Has Error** flag and a
  human-readable **Error Description**
- **Firmware identity**: master program version + main control software code
- **BLE connection** binary sensor and **Last Connected** timestamp

---

## How it works

```
┌──────────────┐   BLE (Modbus RTU)   ┌─────────────┐   Wi-Fi / API   ┌────────────────┐
│  Heat pump   │◄────────────────────►│   ESP32     │◄───────────────►│ Home Assistant │
│ (BlueNRG BLE)│   GATT 0xFF01/02/03  │  (ESPHome)  │   native API    │                │
└──────────────┘                      └─────────────┘                 └────────────────┘
```

- Every **30 s** (only while connected) the ESP32 sends the "read all status"
  poll command. The pump replies with four notification blocks.
- A lambda on the notify characteristic parses each block, decodes the
  registers, and publishes the values to the relevant ESPHome entities.
- Writes (power, mode, target temperature) are built as Modbus
  `0x10` *Write Multiple Registers* frames with a computed CRC-16, then sent
  to the write characteristic.
- **Confirmed writes / auto-retry.** BLE writes are occasionally dropped, so
  each command runs a *send → poll → verify → retry* loop (up to 4 attempts).
  After sending, it polls and compares the value the pump reports back against
  what was requested; if they don't match it re-sends. A confirmation only
  counts once a fresh poll response has actually been parsed — a lost response
  can't be mistaken for success just because the HA entity already shows the
  optimistic value. Concurrent commands (e.g. mode + power from the climate
  entity) are serialized so their frames and polls never interleave. The loop
  stops the moment the pump confirms the change, and logs a `WARN` if it gives
  up unconfirmed. This is why a command that previously needed 2–3 manual
  tries now "takes" on its own.
- **Re-assert on reconnect.** If a command is issued (or dropped) while BLE is
  down, it's replayed automatically once the link comes back. On reconnect the
  ESP32 polls for the real state, then re-sends only the commands whose
  confirmation is still outstanding. A command that *did* confirm is never
  replayed, so a change made at the physical panel during the outage is
  respected rather than overridden.

⚠️ **Only one BLE client can be connected at a time.** While the ESP32 is
connected, the AquaTemp phone app cannot connect, and vice-versa.

---

## Repository contents

| Path | Purpose |
|---|---|
| `esphome_thermotec_heatpump.yaml` | The ESPHome device configuration. |
| `components/pool_heatpump_climate/` | Custom climate component that ties the switch/select/number/sensors into one HA climate entity. |
| `Protocol.md` | Full BLE/Modbus protocol reference (register map, frames, error codes). |
| `README.md` | This file. |

---

## Prerequisites

### Hardware
- An **ESP32** board with BLE. The config targets an **ESP32-C3**
  (`esp32-c3-devkitm-1`) using the **ESP-IDF** framework — change the `board:`
  key if you use a different module.
- The board must be within BLE range of the heat pump.

### Software
- [ESPHome](https://esphome.io/) (CLI, or the Home Assistant add-on).
- [Home Assistant](https://www.home-assistant.io/) with the ESPHome / native
  API integration.

### How the pieces fit together

Part of the config live outside the main YAML and is now included in
this repo:

   **Custom climate component** (`components/pool_heatpump_climate/`) — loaded
   as a local external component:
   ```yaml
   external_components:
     - source:
         type: local
         path: components
       components: [pool_heatpump_climate]
   ```
   It aggregates the `Power` switch, `Mode` select, `Target Temperature` number
   and the temperature/compressor/fan sensors into the single `Pool Heat Pump`
   climate entity. If you ever want to drop it, remove the
   `external_components:` block and the `climate:` section — every other entity
   keeps working without it.

---

## Setup

1. **Find the heat pump's BLE MAC address.** The module advertises as
   `BLUENRG-XXXXXX` (the suffix is the last 3 bytes of the MAC). Scan with
   ESPHome's `esp32_ble_tracker`, the *nRF Connect* app, or `bluetoothctl`.

2. **Edit `esphome_thermotec_heatpump.yaml`:**
   - Set your pump's MAC under `ble_client: → mac_address`.
   - Adjust `board:` if you're not on an ESP32-C3.
   - The service/characteristic UUIDs (`0xFF01/0xFF02/0xFF03`) are standard
     for these PHNIX modules and rarely need changing.

3. **Flash the ESP32:**
   ```bash
   esphome run esphome_thermotec_heatpump.yaml
   ```

4. **Adopt in Home Assistant.** The device appears via the ESPHome
   integration; add it and the entities below show up automatically.

---

## Entities exposed

### Controls
| Entity | Type | Notes |
|---|---|---|
| `Power` | switch | On/off |
| `Mode` | select | Heat / Cool / Auto |
| `Target Temperature` | number | 10–40 °C slider; clamped to the pump's per-mode limits on write |
| `Pool Heat Pump` | climate | Combines the above (requires the `pool_heatpump_climate` component) |
| `ESP BLE Link` | switch | Turn **OFF** to release BLE so the AquaTemp phone app can connect (the pump accepts only one connection at a time); turn back **ON** to resume ESP control. Auto re-enables after 15 min. |

### Temperatures
`Inlet Water`, `Outlet Water`, `Coil`, `Ambient`, `Exhaust` — published as
`NaN` (Home Assistant "unavailable") when the matching sensor faults.

### Diagnostics
Heating / Cooling / Auto set-points · Min/Max heat & cool limits · `Defrost
Type` · `Circulating Pump Mode` · `Heat Pump Mode Type` · `Master Program
Version` · `Main Control Software Code` · `Last Connected`.

### Status & faults
| Entity | Meaning |
|---|---|
| `Heat Pump BLE Connection` | Link up/down |
| `Compressor`, `Circulate Pump`, `High Fan`, `4-Way Valve` | Output relay states |
| `Unit Running` | Pump actually operating (distinct from the power command) |
| `Inlet/Outlet/Ambient/Coil/Exhaust Temp Sensor Fault` | Per-sensor P-codes |
| `Has Error` | Rolled-up problem flag (any E-code or P-code active) |
| `Error Code` / `Error Description` | Raw bitmask + human-readable text |

---

## Compatibility

This was built and verified against a **Thermotec heat and cool single-system**
pool heat pump, but the same PHNIX hardware/protocol is sold under many brands
that use the **AquaTemp** app (PHNIX, AquaTemp, and various regional
re-sellers). See [Protocol.md → *Scope and applicability*](Protocol.md) for the
details and caveats (multi-system units are only partially covered).

If you confirm it works on another brand, please open a PR to add it.

---

## Troubleshooting

- **Won't connect / keeps disconnecting** — make sure the AquaTemp app (or any
  other BLE client) is fully closed; only one connection is allowed at a time.
  Confirm the MAC address and that the ESP32 is in range.
- **A `status=2` read-not-permitted error on `0xFF02`** — benign; the notify
  characteristic is notify-only and some BLE stacks probe it on discovery.
- **Ambient temp falsely shows a fault near −1 °C** — the ambient sensor's
  open-circuit value (`0xFFF6`) coincides with a real −1.0 °C reading. See
  [Protocol.md §6.2.2](Protocol.md).
- **Duplicate readings** — the module sometimes repeats notifications; the
  config already de-dupes via `delta` filters and change checks.

---

## Credits

- Original integration work by **a-teece**:
  <https://github.com/a-teece/Thermotec-Mini-Heat-Pump-Home-Assistant-Integration>
- Protocol reverse-engineered from BTSnoop captures of the AquaTemp app plus a
  leaked PHNIX Modbus document — see [Protocol.md](Protocol.md).

## Disclaimer

This is an unofficial, community project and is not affiliated with or endorsed
by Thermotec, PHNIX, or AquaTemp. Use at your own risk. Writing to undocumented
registers can put the unit into unexpected states — the high-pressure fault
(E01) in particular must **never** be triggered deliberately.
