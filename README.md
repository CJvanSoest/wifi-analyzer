# WiFi Analyzer for Tanmatsu

A 2.4 GHz WiFi channel analyzer app for the **[Tanmatsu](https://tanmatsu.cloud) badge**.

> Scan your surroundings, visualize channel congestion, and monitor signal
> strength per network — directly on the badge display.

---

## Device

| | |
|---|---|
| **Hardware** | Tanmatsu badge (rev 5+) |
| **Application processor** | ESP32-P4 |
| **Radio co-processor** | ESP32-C6 (used for WiFi scanning) |
| **Display** | 4" MIPI DSI, 800×1280 px |
| **Framework** | ESP-IDF v5.5.1 |

The Tanmatsu is an open-source badge developed by
[Nicolai Electronics](https://tanmatsu.cloud).

---

## Views

### Channels
Bar chart showing the combined signal strength per 2.4 GHz channel (1–14).
Useful for quickly spotting congested channels.

### List
Sortable table of all detected networks with:
- SSID (or BSSID for hidden networks)
- Channel
- RSSI (signal strength in dBm)
- Security type (Open / WPA2 / WPA3 / etc.)

### Graph
Live signal history curves per SSID, with automatic label de-collision so
overlapping network names stay readable. Hidden SSIDs are hidden by default
(toggle with H).

---

## Controls

| Key | Action |
|---|---|
| Tab | Cycle views (Channels → List → Graph) |
| R | Rescan |
| W / S | Scroll up / down (List view) |
| H | Toggle hidden SSIDs |
| F1 / Red X | Exit to launcher |

---

## Screenshots

*(Coming soon)*

---

## Building

Requires the Tanmatsu ESP-IDF toolchain. Clone the
[Tanmatsu template](https://github.com/Nicolai-Electronics/tanmatsu-template-pax)
first to set up the local toolchain.

```sh
unset IDF_PATH && unset IDF_TOOLS_PATH && make build
```

---

## License

MIT — see [LICENSE](LICENSE).

Developed by **CJ van Soest** with **[Claude AI](https://claude.ai)** (Anthropic)
as AI co-author.

Badge BSP and template by [Nicolai Electronics](https://tanmatsu.cloud) (MIT/CC0).
