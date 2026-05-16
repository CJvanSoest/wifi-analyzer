# WiFi Analyzer for Tanmatsu

A 2.4 GHz WiFi channel analyzer app for the [Tanmatsu](https://tanmatsu.cloud)
badge (ESP32-P4).

## Features

- **Channels view** — bar chart showing signal strength per 2.4 GHz channel (1–14)
- **List view** — table with SSID, channel, RSSI, and security type
- **Graph view** — live signal history curves per SSID with label de-collision
- Hidden SSID support (shows BSSID instead of name; toggle with H)
- Dark theme

## Controls

| Key | Action |
|---|---|
| Tab | Switch view (Channels → List → Graph) |
| R | Rescan |
| W / S | Scroll (List view) |
| H | Toggle hidden SSIDs |
| F1 / Red X | Exit to launcher |

## Building

Requires the Tanmatsu ESP-IDF toolchain (ESP-IDF v5.5.1 pinned locally).

```sh
unset IDF_PATH && unset IDF_TOOLS_PATH && make build
```

## License

MIT — see [LICENSE](LICENSE).

Developed by **CJ van Soest** with **Claude AI** (Anthropic) as AI co-author.

Badge BSP and template by [Nicolai Electronics](https://tanmatsu.cloud) (MIT/CC0).
