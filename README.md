# SubSpotter

SubSpotter turns Flipper Zero into a passive Sub-GHz explorer for weather sensors and other low-power ISM devices. It scans common bands, fingerprints bursts, and helps identify household sensors without transmitting.

It also integrates the Flipper SDK's built-in protocol decoder pipeline (SubGhzReceiver), so known protocols (Oregon, LaCrosse, Acurite, Nexus, TPMS, etc.) are decoded alongside raw burst fingerprinting.

The app is intentionally framed as a receive-only signal analysis tool for your own lab, test, and benign environmental devices. It does not include transmit, replay, emulate, or bypass features.

## Features

- **Focused scan** with manual band/preset switching via Up/Down on the LIVE screen.
- Six scan entries across three bands: 433.92 MHz, 868.35 MHz, 915.00 MHz, each with OOK and 2FSK presets.
- **Passive receive only** — never transmits.
- **Burst fingerprinting** based on packet length, pulse profile (short/medium/long distribution), repeat timing, and live RSSI.
- **SDK protocol decoding** via the Flipper's SubGhzReceiver — recognises known protocols in real time.
- Local matcher for likely device families:
  - outdoor thermometers
  - weather stations
  - door/window sensors
  - TPMS lab/demo traces
  - simple ISM beacons
- Compact tab bar for quick screen switching.
- Three on-device screens:
  - **LIVE** — frequency/preset in bold, wide RSSI bar with dBm, burst/SDK counters, decoded label
  - **SEEN** — scrollable fingerprint list with family, frequency, modulation, and detail panel
  - **SAVED** — capture list with labels, family names, and timestamp/frequency detail
- Capture saving with user-selectable labels.
- Session log append to a CSV file on-device.

## What The App Shows

For each detected burst or recurring device, SubSpotter surfaces:

- frequency and preset (OOK / 2FSK)
- RSSI / signal strength with visual meter
- modulation guess
- repeat interval
- packet length (pulse count)
- pulse profile summary (Short / Medium / Long)
- confidence score
- SDK-decoded protocol name (when recognised)
- total burst count and SDK decode hit count

## UI Flow

A compact 8px tab bar at the top shows LIVE / SEEN / SAVED. Left/Right arrows cycle between screens.

### LIVE

Row 1: frequency and preset in bold, scan position (e.g. 2/6).  
Row 2: wide RSSI bar with dBm value.  
Row 3: burst count and SDK decode hit count.  
Row 4: latest decoded protocol name (bold) or status message.

Controls:

- **Up/Down**: switch between scan entries (bands and presets)
- **Left**: go to SEEN
- **Right**: go to SAVED
- **OK**: save the latest capture
- **Back**: exit

### SEEN

Scrollable list (3 visible rows) showing family short name, frequency, modulation, and confidence. A separator line and detail row below show pulse profile, hit count, packet length, and repeat interval for the selected device.

Controls:

- **Up/Down**: move selection
- **Left**: go to LIVE
- **Right**: go to SAVED
- **OK**: save the latest capture using the active label
- **Back**: exit

### SAVED

Scrollable list (3 visible rows) showing label name, family short name, and confidence. Detail row shows timestamp, frequency, modulation, and pulse profile.

Controls:

- **Up/Down**: move selection
- **Left**: go to SEEN
- **Right**: go to LIVE
- **Back**: exit

## Capture Storage

Saved captures are appended to:

```text
/data/subspotter/captures.csv
```

Each row currently includes:

- timestamp
- frequency in Hz
- peak RSSI in dBm
- modulation guess
- guessed device family
- repeat interval in ms
- packet length
- pulse profile summary
- confidence score
- selected label

## Detection Approach

SubSpotter uses two complementary detection methods:

### SDK Protocol Decoding

Raw IQ-level pulse/duration pairs are fed into the Flipper SDK's `SubGhzReceiver` with the full `subghz_protocol_registry`. This decodes known protocols (Oregon Scientific, LaCrosse, Acurite, Nexus, TPMS, etc.) in real time alongside the raw burst engine.

### Burst Fingerprinting

For signals not matched by a known protocol, SubSpotter classifies bursts using:

- listening preset used during capture (OOK vs 2FSK)
- pulse count per burst
- short / medium / long pulse distribution
- repeat interval when the same rough fingerprint is seen again
- peak RSSI

This dual approach gives useful results for both known and unknown protocols.

## Safety Boundary

This project is intended for:

- your own weather sensors
- your own door/window sensor test rigs
- benign household ISM devices
- lab and demo traces

It is not intended for:

- covert tracking
- harvesting private telemetry
- targeting security-critical devices
- replay or intrusion workflows

## Build

Use the local uFBT install already present in this repo:

```bash
./.venv/bin/ufbt
```

Launch to a connected Flipper:

```bash
./.venv/bin/ufbt launch
```

## Project Files

- `application.fam` defines the external app metadata.
- `subspotter.c` contains the current MVP implementation.
- `README.md` documents the app behavior, boundaries, and usage.

## Scan Entries

| # | Frequency   | Preset | Dwell (ms) |
|---|-------------|--------|------------|
| 0 | 433.92 MHz  | OOK    | 5500       |
| 1 | 868.35 MHz  | OOK    | 4500       |
| 2 | 915.00 MHz  | OOK    | 4500       |
| 3 | 433.92 MHz  | 2FSK   | 1800       |
| 4 | 868.35 MHz  | 2FSK   | 1800       |
| 5 | 915.00 MHz  | 2FSK   | 1800       |

Use Up/Down on the LIVE screen to switch between entries.

## Current Limitations

- Burst fingerprint matching is heuristic — confidence scores indicate match quality.
- The saved capture list is in-memory for the current run; the durable log is appended to CSV.
- Labels are selected from a preset list (Field Note, Greenhouse, Window Rig, Beacon Demo, TPMS Lab, Custom Tag).
- Single-band focused scan — no automatic hopping between entries.

## Roadmap Ideas

- Automatic frequency hopping across scan entries
- Richer pulse histogram and timing detail view
- Beginner-friendly "what am I seeing?" helper text per family
- JSON export alongside CSV
- Desktop companion script for comparing captures against a larger signature database
