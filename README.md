# SubSpotter

SubSpotter turns Flipper Zero into a passive Sub-GHz explorer for weather sensors and other low-power ISM devices. It scans common bands, fingerprints bursts, and helps identify household sensors without transmitting.

The app is intentionally framed as a receive-only signal analysis tool for your own lab, test, and benign environmental devices. It does not include transmit, replay, emulate, or bypass features.

## MVP Features

- Band hopper across common receive frequencies: 433.92 MHz, 868.35 MHz, and 915.00 MHz.
- Passive receive only using alternating OOK and FSK-style listening presets.
- Burst fingerprinting based on packet length, pulse profile, repeat timing, and live RSSI.
- Local matcher for likely device families:
	- outdoor thermometers
	- weather stations
	- door/window sensors in a test setup
	- TPMS lab or demo traces
	- simple ISM beacons
- Three on-device screens:
	- Live Scan
	- Seen Devices
	- Saved Captures
- Capture saving with user-selectable labels.
- Session log append to a CSV file on-device.

## What The App Shows

For each likely burst or recurring device family, SubSpotter surfaces:

- frequency
- RSSI / signal strength
- modulation guess
- repeat interval
- packet length
- pulse profile summary
- confidence score

The current implementation focuses on useful fingerprinting rather than full protocol decoding.

## UI Flow

### Live Scan

Shows the active band/preset, current RSSI, the last classified burst, and a simple three-band activity view.

Controls:

- Left: Seen Devices
- Right: Saved Captures
- OK: save the latest capture
- Up/Down: cycle the label used for the next saved capture
- Back: exit

### Seen Devices

Shows the most recent unique fingerprints grouped by rough similarity.

Controls:

- Left: Live Scan
- Right: Saved Captures
- Up/Down: move selection
- OK: save the latest capture using the active label
- Back: exit

### Saved Captures

Shows the in-memory saved capture list for the current run. Every save is also appended to the CSV log on-device.

Controls:

- Left: Seen Devices
- Right: Live Scan
- Up/Down: move selection
- Back: exit

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

## Fingerprinting Approach

SubSpotter does not try to ship a full decoder stack onto the Flipper.

Instead it classifies bursts using:

- listening preset used during capture
- pulse count per burst
- short / medium / long pulse distribution
- repeat interval when the same rough fingerprint is seen again
- current peak RSSI

That keeps the app lightweight and understandable while still giving users a practical answer to “what am I seeing?”

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

## Current Limitations

- Matching is heuristic, not a full decoder.
- The saved capture list is in-memory for the current run, while the durable log is written to CSV.
- Labels are selected from a small on-device preset list rather than free-text entry.
- The current UI prioritizes fast scanning and review over deep waveform inspection.

## Roadmap Ideas

- richer pulse histogram and timing detail view
- beginner-friendly “what am I seeing?” helper copy per family
- JSON export alongside CSV
- desktop companion script for comparing captures against a larger signature database
- optional compatibility mapping to rtl_433-style family labels without copying its decoder stack
