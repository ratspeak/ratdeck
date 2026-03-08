# RatDeck

**v1.5.2** | [Ratspeak.org](https://ratspeak.org)

Standalone [Reticulum](https://reticulum.network/) mesh node + [LXMF](https://github.com/markqvist/LXMF) encrypted messenger for the [LilyGo T-Deck Plus](https://www.lilygo.cc/products/t-deck-plus).

Not an RNode. Not a gateway. A fully self-contained LoRa mesh communicator with a keyboard, trackball, and LVGL UI — no host computer required.

```
+----------------------------------------------+
| [|||]          Ratspeak           [87%]       |
+----------------------------------------------+
|                                              |
|              CONTENT AREA                    |
|           320x240, LVGL v8.4                 |
+----------------------------------------------+
| Home  Friends  Msgs  Peers  Setup            |
+----------------------------------------------+
```

## Features

- **Encrypted LoRa messaging** — LXMF protocol, Ed25519 signatures, per-conversation storage
- **Mesh networking** — Reticulum endpoint or transport node, automatic path discovery
- **Node discovery** — see who's online, save contacts, manage friends
- **WiFi bridging** — TCP client to remote Reticulum nodes, or AP mode to bridge your desktop to LoRa
- **BLE transport** — NimBLE Sideband interface
- **On-device config** — 7-category settings, radio presets, multi-slot identity management
- **Dual storage** — LittleFS flash + SD card with atomic writes and automatic backup
- **OTA-ready** — check for firmware updates directly from the device

Built on [microReticulum](https://github.com/attermann/microReticulum) with a register-level SX1262 driver and LVGL v8.4.

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| **Board** | LilyGo T-Deck Plus | ESP32-S3, 16MB flash, 8MB PSRAM, 320x240 IPS, QWERTY keyboard, trackball |
| **Radio** | Integrated SX1262 | 915 MHz ISM, TCXO 1.8V, DIO2 RF switch |
| **Storage** | microSD card | Optional but recommended. FAT32, any size |

## Quick Start

```bash
git clone https://github.com/ratspeak/ratdeck.git
cd Ratdeck
python3 -m platformio run               # build
python3 -m platformio run --target upload # flash
```

First build pulls the ESP32-S3 toolchain and all dependencies automatically.

### Web Flash

No build tools? Visit **[ratspeak.org/download](https://ratspeak.org/download.html)** to flash from your browser.

## First Boot

1. Boot animation with progress bar
2. SX1262 radio initializes (915 MHz, Balanced preset)
3. SD card checked, `/ratputer/` directories auto-created
4. Reticulum identity generated (Ed25519 keypair, triple-redundant backup)
5. Name input screen (optional — auto-generates if skipped)
6. Home screen — you're on the mesh

## Usage

### Navigation

| Input | Action |
|-------|--------|
| Trackball up/down | Scroll lists |
| Trackball left/right | Cycle tabs |
| Trackball click | Select / confirm |
| Long-press (1.2s) | Context menu |
| `,` / `/` | Previous / next tab |
| Enter | Select / send |
| Esc / Backspace | Back / cancel |
| Ctrl+H | Hotkey help overlay |

### Tabs

| Tab | What It Shows |
|-----|---------------|
| **Home** | Name, LXMF address, connection status, online nodes |
| **Friends** | Saved contacts with display names |
| **Msgs** | Conversations — sorted by most recent, preview, unread dots |
| **Peers** | All discovered nodes with RSSI/SNR |
| **Setup** | Device, Display, Radio, Network, Audio, Info, System |

### Sending a Message

1. Another node appears in **Peers** (or connect via WiFi)
2. Select the node, press Enter to open chat
3. Type your message, press Enter to send
4. Status: yellow (sending) → green (delivered) → red (failed)

## LoRa Radio

Three presets in Settings → Radio:

| Preset | SF | BW | CR | TX | Use Case |
|--------|----|----|----|----|----------|
| **Balanced** | 9 | 250 kHz | 4/5 | 14 dBm | General use |
| Long Range | 12 | 125 kHz | 4/8 | 17 dBm | Maximum range |
| Fast | 7 | 500 kHz | 4/5 | 10 dBm | Short range, fast |

All parameters individually configurable. Changes apply immediately.

## WiFi & Networking

| Mode | Description |
|------|-------------|
| **STA** (default) | Connect to your WiFi, TCP client to remote Reticulum nodes |
| **AP** | Creates `ratdeck-XXXX` hotspot, TCP server on port 4242 |
| **OFF** | LoRa only, saves power |

**Connect to the mesh over WiFi:** Settings → Network → enter WiFi creds → add TCP endpoint (e.g., `rns.beleth.net:4242`).

**Bridge your desktop to LoRa:** Set AP mode → connect laptop to `ratdeck-XXXX` (password: `ratspeak`) → add `TCPClientInterface` to `~/.reticulum/config` pointing at `192.168.4.1:4242`.

## Transport Mode

Default: **endpoint** (handles own traffic). Enable **Transport Node** in Settings → Network to relay packets and maintain routing tables for the mesh.

## Status

| Subsystem | Status |
|-----------|--------|
| LoRa (SX1262) | Working — async TX/RX, configurable presets |
| LXMF messaging | Working — send/receive/store, Ed25519, delivery tracking |
| LVGL UI | Working — 5 tabs, chat bubbles, settings, contacts |
| WiFi STA + TCP | Working — auto-reconnect, live server switching |
| WiFi AP | Working — TCP server, HDLC framing, desktop bridge |
| BLE Sideband | Working — NimBLE transport |
| Node discovery | Working — announces, friend management |
| SD + Flash storage | Working — dual-backend, atomic writes |
| Identity management | Working — multi-slot, triple-redundant |
| Transport mode | Working — endpoint or relay |
| Touchscreen | Disabled — GT911 needs calibration |
| GPS | Pins defined — not yet implemented |

## Documentation

| Doc | Contents |
|-----|----------|
| **[Quick Start](docs/QUICKSTART.md)** | Build, flash, first boot, first message |
| [Building](docs/BUILDING.md) | Build flags, flashing, CI/CD, partition table |
| [Architecture](docs/ARCHITECTURE.md) | Layer diagram, data flow, design decisions |
| [Development](docs/DEVELOPMENT.md) | Adding screens, settings, transports, debugging |
| [Pin Map](docs/PINMAP.md) | Full T-Deck Plus GPIO assignments |
| [Hotkeys](docs/HOTKEYS.md) | Keyboard shortcuts and navigation |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Radio, build, boot, storage issues |

## License

GPL-3.0
