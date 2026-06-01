/*
 * WiFi Analyzer for Tanmatsu
 *
 * A 2.4 GHz WiFi channel analyzer for the Tanmatsu badge (ESP32-P4).
 * Views: channel bar chart | network list with navigation + detail screen |
 *        frequency graph (Gaussian arch lines, bandwidth-scaled, per-AP legend).
 *
 * SPDX-FileCopyrightText: 2026 CJ van Soest
 * SPDX-License-Identifier: MIT
 *
 * Developed with Claude AI (Anthropic) as AI co-author.
 *
 * Feature inspiration and concept credit:
 *   Saarbastler (joerg at saarbastler dot de)
 *   tanmatsu-wifi-scanner — https://git.adminforge.de/jjp
 *   MIT License — network list navigation, per-AP detail screen,
 *   frequency graph with bandwidth-scaled channel visualization.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"
#include "wifi_connection.h"
#include "wifi_remote.h"

static char const TAG[] = "wifi-analyzer";

// Colors (dark theme)
#define COLOR_BG        0xFF0D1117
#define COLOR_HEADER    0xFF161B22
#define COLOR_ROW_ALT   0xFF1C2128
#define COLOR_ROW_SEL   0xFF1F3044
#define COLOR_TEXT      0xFFCDD9E5
#define COLOR_DIM       0xFF768390
#define COLOR_ACCENT    0xFF539BF5
#define COLOR_GOOD      0xFF57AB5A  // >= -60 dBm
#define COLOR_OK        0xFFCFBA06  // >= -70 dBm
#define COLOR_FAIR      0xFFDB6D28  // >= -80 dBm
#define COLOR_POOR      0xFFE5534B  // <  -80 dBm

#define VIEW_CHANNELS 0
#define VIEW_LIST     1
#define VIEW_GRAPH    2
#define VIEW_DETAIL   3

// 10 distinct colors for graph curves / list highlights
static const pax_col_t GRAPH_COLORS[] = {
    0xFF539BF5, 0xFF57AB5A, 0xFFDB6D28, 0xFFE5534B, 0xFFCFBA06,
    0xFFDCAEFA, 0xFF6CB6FF, 0xFFFFA657, 0xFF79C0FF, 0xFF56D364,
};

// MAC-based persistent color map (survives rescan)
#define COLOR_MAP_SIZE 24
typedef struct {
    uint8_t   mac[6];
    pax_col_t color;
    bool      used;
} color_map_entry_t;
static color_map_entry_t color_map[COLOR_MAP_SIZE];
static int               color_next_idx = 0;

// Display state
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

// App state
static wifi_ap_record_t *ap_list      = NULL;
static uint16_t          ap_count     = 0;
static int               view         = VIEW_CHANNELS;
static bool              show_hidden  = false;
static int               list_scroll   = 0;
static int               list_selected = 0;
static bool              wifi_ready   = false;

static void blit(void) {
    bsp_display_blit(0, 0, display_h_res, display_v_res, pax_buf_get_pixels(&fb));
}

static pax_col_t rssi_color(int8_t rssi) {
    if (rssi >= -60) return COLOR_GOOD;
    if (rssi >= -70) return COLOR_OK;
    if (rssi >= -80) return COLOR_FAIR;
    return COLOR_POOR;
}

static int compare_rssi(const void *a, const void *b) {
    return (int)((wifi_ap_record_t *)b)->rssi - (int)((wifi_ap_record_t *)a)->rssi;
}

// Returns persistent color for a MAC address, assigning a new one if unseen.
static pax_col_t get_ap_color(const uint8_t *mac) {
    for (int i = 0; i < COLOR_MAP_SIZE; i++) {
        if (color_map[i].used && memcmp(color_map[i].mac, mac, 6) == 0)
            return color_map[i].color;
    }
    // Find a free slot first, then evict the oldest by cycling
    for (int i = 0; i < COLOR_MAP_SIZE; i++) {
        if (!color_map[i].used) {
            memcpy(color_map[i].mac, mac, 6);
            color_map[i].color = GRAPH_COLORS[color_next_idx % 10];
            color_map[i].used  = true;
            color_next_idx++;
            return color_map[i].color;
        }
    }
    // All slots used — overwrite cyclically (oldest eviction)
    int slot = color_next_idx % COLOR_MAP_SIZE;
    memcpy(color_map[slot].mac, mac, 6);
    color_map[slot].color = GRAPH_COLORS[color_next_idx % 10];
    color_next_idx++;
    return color_map[slot].color;
}

static void show_message(const char *msg) {
    int w = pax_buf_get_width(&fb);
    int h = pax_buf_get_height(&fb);
    pax_background(&fb, COLOR_BG);
    pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 18, w / 2 - 100, h / 2 - 9, msg);
    blit();
}

static void do_scan(void) {
    show_message("Scanning WiFi networks...");

    if (ap_list) {
        free(ap_list);
        ap_list  = NULL;
        ap_count = 0;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode failed: %d", err);
        show_message("WiFi mode error - press R to retry");
        return;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "wifi_start failed: %d", err);
        show_message("WiFi start error - press R to retry");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %d", err);
        show_message("Scan failed - press R to retry");
        return;
    }

    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 0) {
        ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            qsort(ap_list, ap_count, sizeof(wifi_ap_record_t), compare_rssi);
        } else {
            ap_count = 0;
        }
    }

    list_scroll   = 0;
    list_selected = 0;
    ESP_LOGI(TAG, "Found %u networks", ap_count);
}

// Returns half-width in channel units for the ellipse (bandwidth-dependent).
static float bw_half_channels(uint8_t bw) {
    switch (bw) {
        case 1: return 2.0f;   // 40 MHz
        case 2: return 4.0f;   // 80 MHz
        case 3: return 8.0f;   // 160 MHz
        default: return 1.0f;  // 20 MHz
    }
}

static const char *bw_text(uint8_t bw) {
    switch (bw) {
        case 1: return "40 MHz";
        case 2: return "80 MHz";
        case 3: return "160 MHz";
        case 4: return "80+80 MHz";
        default: return "20 MHz";
    }
}

static const char *bw_short(uint8_t bw) {
    switch (bw) {
        case 1: return "40M";
        case 2: return "80M";
        case 3: return "160M";
        case 4: return "80+M";
        default: return "20M";
    }
}

static const char *auth_text(wifi_auth_mode_t mode) {
    static const char * const t[] = {
        "Open", "WEP", "WPA-PSK", "WPA2-PSK", "WPA/WPA2-PSK",
        "Enterprise", "WPA3-PSK", "WPA2/WPA3-PSK", "WAPI-PSK",
        "OWE", "WPA3-ENT-192",
    };
    if ((unsigned)mode < sizeof(t) / sizeof(t[0])) return t[(int)mode];
    return "?";
}

static const char *cipher_text(wifi_cipher_type_t c) {
    static const char * const t[] = {
        "None", "WEP40", "WEP104", "TKIP", "CCMP", "TKIP/CCMP",
        "AES-CMAC128", "SMS4", "GCMP", "GCMP256", "AES-GMAC128",
        "AES-GMAC256", "?",
    };
    if ((unsigned)c < sizeof(t) / sizeof(t[0])) return t[(int)c];
    return "?";
}

// ------- Render helpers -------

static void render_header(void) {
    int  w = pax_buf_get_width(&fb);
    char title[80];
    const char *view_name =
        view == VIEW_CHANNELS ? "Channels" :
        view == VIEW_LIST     ? "List" :
        view == VIEW_GRAPH    ? "Graph" : "Detail";
    snprintf(title, sizeof(title), "WiFi Analyzer  %u networks  [%s]", ap_count, view_name);
    pax_simple_rect(&fb, COLOR_HEADER, 0, 0, w, 28);
    pax_draw_text(&fb, COLOR_ACCENT, pax_font_sky_mono, 16, 8, 6, title);
}

static void render_footer(const char *hint) {
    int w = pax_buf_get_width(&fb);
    int h = pax_buf_get_height(&fb);
    pax_simple_rect(&fb, COLOR_HEADER, 0, h - 22, w, 22);
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, 8, h - 17, hint);
}

static void render_channels(void) {
    int w         = pax_buf_get_width(&fb);
    int h         = pax_buf_get_height(&fb);
    int top       = 28;
    int bottom    = h - 22;
    int label_h   = 18;
    int content_h = bottom - top - label_h;

    int8_t best[14];
    int    cnt[14];
    for (int i = 0; i < 14; i++) { best[i] = -110; cnt[i] = 0; }

    for (int i = 0; i < ap_count; i++) {
        uint8_t ch = ap_list[i].primary;
        if (ch >= 1 && ch <= 13) {
            cnt[ch]++;
            if (ap_list[i].rssi > best[ch]) best[ch] = ap_list[i].rssi;
        }
    }

    int slot_w = (w - 20) / 13;

    for (int ch = 1; ch <= 13; ch++) {
        int x     = 10 + (ch - 1) * slot_w;
        int bar_w = slot_w - 6;
        int lbl_y = bottom - label_h;

        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%d", ch);
        pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, x + slot_w / 2 - 5, lbl_y, lbl);

        if (cnt[ch] == 0) continue;

        int bar_h = (best[ch] + 95) * content_h / 65;
        if (bar_h < 6) bar_h = 6;
        if (bar_h > content_h) bar_h = content_h;

        int bar_x = x + 3;
        int bar_y = lbl_y - bar_h;

        pax_simple_rect(&fb, rssi_color(best[ch]), bar_x, bar_y, bar_w, bar_h);

        char rssi_str[6];
        snprintf(rssi_str, sizeof(rssi_str), "%d", best[ch]);
        pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 11, bar_x + 1, bar_y - 14, rssi_str);

        if (cnt[ch] > 1) {
            char cnt_str[12];
            snprintf(cnt_str, sizeof(cnt_str), "%d", cnt[ch]);
            pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 11, bar_x + 2, bar_y + 2, cnt_str);
        }
    }
}

static int list_max_vis(void) {
    int h = pax_buf_get_height(&fb);
    return (h - 28 - 22 - 24) / 24;
}

static bool ap_is_hidden(int i) {
    return strlen((char *)ap_list[i].ssid) == 0;
}

static bool ap_is_visible(int i) {
    return show_hidden || !ap_is_hidden(i);
}

// Snap list_selected onto a visible AP (search forward, then backward).
static void list_normalize_selection(void) {
    if (ap_count == 0) { list_selected = 0; return; }
    if (list_selected < 0) list_selected = 0;
    if (list_selected >= ap_count) list_selected = ap_count - 1;
    if (ap_is_visible(list_selected)) return;
    for (int i = list_selected + 1; i < ap_count; i++) {
        if (ap_is_visible(i)) { list_selected = i; return; }
    }
    for (int i = list_selected - 1; i >= 0; i--) {
        if (ap_is_visible(i)) { list_selected = i; return; }
    }
}

// Move selection to next/prev visible AP, skipping filtered ones.
static void list_step(int dir) {
    if (ap_count == 0) return;
    int i = list_selected + dir;
    while (i >= 0 && i < ap_count) {
        if (ap_is_visible(i)) { list_selected = i; return; }
        i += dir;
    }
}

static void render_list(void) {
    int w       = pax_buf_get_width(&fb);
    int top     = 28;
    int row_h   = 24;
    int max_vis = list_max_vis();

    // Count visible + hidden, and find sel_vis (position of list_selected in visible order).
    int vis_count = 0, hidden_count = 0, sel_vis = -1;
    for (int i = 0; i < ap_count; i++) {
        if (ap_is_hidden(i)) hidden_count++;
        if (ap_is_visible(i)) {
            if (i == list_selected) sel_vis = vis_count;
            vis_count++;
        }
    }

    // Selection was filtered out (e.g. just toggled H off while on a hidden row): snap.
    if (sel_vis < 0 && vis_count > 0) {
        list_normalize_selection();
        vis_count = 0; sel_vis = -1;
        for (int i = 0; i < ap_count; i++) {
            if (ap_is_visible(i)) {
                if (i == list_selected) sel_vis = vis_count;
                vis_count++;
            }
        }
    }

    // Keep the cursor on-screen (scroll is now in visible-index space).
    if (sel_vis >= 0) {
        if (sel_vis < list_scroll)            list_scroll = sel_vis;
        if (sel_vis >= list_scroll + max_vis) list_scroll = sel_vis - max_vis + 1;
    }
    if (list_scroll > vis_count - max_vis) list_scroll = vis_count - max_vis;
    if (list_scroll < 0)                   list_scroll = 0;

    // Column headers
    pax_simple_rect(&fb, COLOR_HEADER, 0, top, w, row_h);
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, 10,      top + 5, "SSID");
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 170, top + 5, "CH");
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 120, top + 5, "RSSI");
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 60,  top + 5, "SEC");
    top += row_h;

    // Counter top-right: "k/total" plus "(+N hidden, H)" hint when filtered.
    if (vis_count > 0) {
        char info[64];
        if (!show_hidden && hidden_count > 0) {
            snprintf(info, sizeof(info), "%d/%d (+%d hidden H)",
                     sel_vis + 1, vis_count, hidden_count);
            pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 210, 8, info);
        } else if (vis_count > max_vis) {
            snprintf(info, sizeof(info), "%d/%d", sel_vis + 1, vis_count);
            pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 70, 8, info);
        }
    }

    // Walk ap_list, but only render the v-th visible entry; row index in viewport is (v - list_scroll).
    int v = 0;
    for (int i = 0; i < ap_count; i++) {
        if (!ap_is_visible(i)) continue;
        if (v >= list_scroll && (v - list_scroll) < max_vis) {
            int y = top + (v - list_scroll) * row_h;

            if (v == sel_vis) {
                pax_simple_rect(&fb, COLOR_ROW_SEL, 0, y, w, row_h);
                pax_draw_text(&fb, COLOR_ACCENT, pax_font_sky_mono, 14, 2, y + 5, ">");
            } else if ((v % 2) == 0) {
                pax_simple_rect(&fb, COLOR_ROW_ALT, 0, y, w, row_h);
            }

            char ssid[33];
            strncpy(ssid, (char *)ap_list[i].ssid, 32);
            ssid[32] = '\0';
            if (strlen(ssid) == 0) {
                snprintf(ssid, sizeof(ssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                         ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                         ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
            }
            pax_col_t ap_color = get_ap_color(ap_list[i].bssid);
            pax_draw_text(&fb, ap_color, pax_font_sky_mono, 14, 14, y + 5, ssid);

            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%2u", ap_list[i].primary);
            pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 14, w - 170, y + 5, tmp);

            snprintf(tmp, sizeof(tmp), "%4d", ap_list[i].rssi);
            pax_draw_text(&fb, rssi_color(ap_list[i].rssi), pax_font_sky_mono, 14, w - 120, y + 5, tmp);

            const char *sec = (ap_list[i].authmode == WIFI_AUTH_OPEN) ? "open" : "lock";
            pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 14, w - 60, y + 5, sec);
        }
        v++;
    }
}

// Gaussian arch helpers — split so render_graph can stack all fills first and
// draw the 2px strokes in a second pass on top (no line gets hidden by another
// curve's translucent fill). sigma_ch is the standard deviation in channel units.
static void draw_arch_fill(int center_x, int bottom_y, float sigma_ch, float x_scale,
                           float peak_h, pax_col_t color) {
    if (peak_h < 2.0f) return;
    float sigma_px = sigma_ch * x_scale;
    pax_col_t fill = (color & 0x00FFFFFFu) | 0x20000000u;  // ~12% opacity tint
    int x_start = center_x - (int)(3.5f * sigma_px);
    int x_end   = center_x + (int)(3.5f * sigma_px);
    for (int x = x_start; x <= x_end; x++) {
        float dx = (float)(x - center_x);
        float h  = peak_h * expf(-0.5f * (dx / sigma_px) * (dx / sigma_px));
        int ih = (int)h;
        if (ih < 1) continue;
        pax_simple_rect(&fb, fill, x, bottom_y - ih, 1, ih);
    }
}

static void draw_arch_stroke(int center_x, int bottom_y, float sigma_ch, float x_scale,
                             float peak_h, pax_col_t color) {
    if (peak_h < 2.0f) return;
    float sigma_px = sigma_ch * x_scale;
    int x_start = center_x - (int)(3.5f * sigma_px);
    int x_end   = center_x + (int)(3.5f * sigma_px);
    int prev_top = -1;
    for (int x = x_start; x <= x_end; x++) {
        float dx = (float)(x - center_x);
        float h  = peak_h * expf(-0.5f * (dx / sigma_px) * (dx / sigma_px));
        int ih = (int)h;
        if (ih < 1) { prev_top = -1; continue; }
        int top = bottom_y - ih;
        if (prev_top < 0) prev_top = bottom_y;
        int y_hi = top < prev_top ? top     : prev_top;
        int y_lo = top > prev_top ? top + 1 : prev_top + 1;
        pax_simple_rect(&fb, color, x, y_hi, 2, y_lo - y_hi + 1);
        prev_top = top;
    }
}

static void render_graph(void) {
    int w      = pax_buf_get_width(&fb);
    int h      = pax_buf_get_height(&fb);
    int top    = 28;
    int bottom = h - 22 - 18;  // top of channel-number labels

    float x_margin = 30.0f;
    float x_scale  = (float)(w - 2 * x_margin) / 12.0f;

    // Build visible AP list first so we know how many legend rows we need.
    int draw_indices[20];
    int draw_count = 0;
    for (int i = 0; i < ap_count && draw_count < 20; i++) {
        if (!show_hidden && strlen((char *)ap_list[i].ssid) == 0) continue;
        draw_indices[draw_count++] = i;
    }

    // Reserve space for legend strip (4 entries per row, 13 px per row).
    int legend_rows    = draw_count == 0 ? 0 : (draw_count + 3) / 4;
    int legend_h       = legend_rows * 13 + (legend_rows > 0 ? 2 : 0);
    int ellipse_bottom = bottom - legend_h;
    int content_h      = ellipse_bottom - top;

    // Channel axis: baseline + tick marks + labels
    for (int ch = 1; ch <= 13; ch++) {
        int x = (int)(x_margin + (ch - 1) * x_scale);
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%d", ch);
        pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 12, x - 5, bottom, lbl);
        pax_simple_rect(&fb, COLOR_DIM, x, ellipse_bottom - 4, 1, 4);
    }
    pax_simple_rect(&fb, COLOR_DIM, (int)x_margin, ellipse_bottom, w - 2 * (int)x_margin, 1);

    float rssi_floor = -95.0f;
    float rssi_range = 70.0f;

    // Two-pass rendering: fills first (back-to-front so they stack), then 2px
    // strokes on top in the same order. This prevents a strong AP's translucent
    // fill from hiding the line of a weaker AP sitting in front of it.
    for (int pass = 0; pass < 2; pass++) {
        for (int di = draw_count - 1; di >= 0; di--) {
            int   i         = draw_indices[di];
            float mu        = (float)ap_list[i].primary;
            float norm_rssi = (ap_list[i].rssi - rssi_floor) / rssi_range;
            if (norm_rssi < 0.0f) norm_rssi = 0.0f;
            if (norm_rssi > 1.0f) norm_rssi = 1.0f;

            float     peak_h   = norm_rssi * (float)content_h;
            float     sigma_ch = bw_half_channels(ap_list[i].bandwidth);
            pax_col_t color    = get_ap_color(ap_list[i].bssid);
            int       center_x = (int)(x_margin + (mu - 1.0f) * x_scale);

            if (pass == 0) draw_arch_fill  (center_x, ellipse_bottom, sigma_ch, x_scale, peak_h, color);
            else           draw_arch_stroke(center_x, ellipse_bottom, sigma_ch, x_scale, peak_h, color);
        }
    }

    // SSID labels at dome peaks with de-collision (include bandwidth).
    int placed_x[20] = {0};
    int placed_y[20] = {0};
    int placed_count  = 0;

    for (int di = 0; di < draw_count; di++) {
        int     i  = draw_indices[di];
        uint8_t ch = ap_list[i].primary;
        if (ch < 1 || ch > 13) continue;

        float norm_rssi = (ap_list[i].rssi - rssi_floor) / rssi_range;
        if (norm_rssi < 0.0f) norm_rssi = 0.0f;
        if (norm_rssi > 1.0f) norm_rssi = 1.0f;

        int peak_px = (int)(x_margin + (ch - 1) * x_scale);
        int lbl_y   = ellipse_bottom - (int)(norm_rssi * content_h) - 16;

        bool moved = true;
        while (moved) {
            moved = false;
            for (int j = 0; j < placed_count; j++) {
                if (abs(placed_x[j] - peak_px) < 100 && abs(placed_y[j] - lbl_y) < 14) {
                    lbl_y = placed_y[j] + 14;
                    moved = true;
                }
            }
        }
        if (lbl_y > ellipse_bottom - 4) lbl_y = ellipse_bottom - 4;
        if (lbl_y < top + 2)            lbl_y = top + 2;

        placed_x[placed_count] = peak_px;
        placed_y[placed_count] = lbl_y;
        placed_count++;

        char lbl[24];
        if (strlen((char *)ap_list[i].ssid) == 0) {
            snprintf(lbl, sizeof(lbl), "%d:%02X%02X %s", di + 1,
                     ap_list[i].bssid[0], ap_list[i].bssid[1], bw_short(ap_list[i].bandwidth));
        } else {
            snprintf(lbl, sizeof(lbl), "%d:%.8s %s", di + 1,
                     (char *)ap_list[i].ssid, bw_short(ap_list[i].bandwidth));
        }

        pax_col_t color    = get_ap_color(ap_list[i].bssid);
        int       lbl_w    = (int)pax_text_size(pax_font_sky_mono, 12, lbl).x;
        pax_simple_rect(&fb, 0xCC0D1117, peak_px - 2, lbl_y - 1, lbl_w + 4, 14);
        pax_draw_text(&fb, color, pax_font_sky_mono, 12, peak_px, lbl_y, lbl);
    }

    // Legend strip: colored swatch + SSID + bandwidth, 4 per row.
    if (legend_rows > 0) {
        int entry_w = (w - 2 * (int)x_margin) / 4;
        for (int di = 0; di < draw_count; di++) {
            int i   = draw_indices[di];
            int col = di % 4;
            int row = di / 4;
            int lx  = (int)x_margin + col * entry_w;
            int ly  = ellipse_bottom + 2 + row * 13;
            pax_col_t color = get_ap_color(ap_list[i].bssid);
            pax_simple_rect(&fb, color, lx, ly + 1, 10, 10);
            char leg[20];
            if (strlen((char *)ap_list[i].ssid) == 0) {
                snprintf(leg, sizeof(leg), "%02X%02X:%s",
                         ap_list[i].bssid[0], ap_list[i].bssid[1], bw_short(ap_list[i].bandwidth));
            } else {
                snprintf(leg, sizeof(leg), "%.8s %s",
                         (char *)ap_list[i].ssid, bw_short(ap_list[i].bandwidth));
            }
            pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 11, lx + 13, ly, leg);
        }
    }
}

static void render_detail(void) {
    if (ap_count == 0 || list_selected >= ap_count) return;

    int w   = pax_buf_get_width(&fb);
    int h   = pax_buf_get_height(&fb);
    int top = 28;
    int row = 20;
    int lx  = 14;   // label x
    int vx  = 200;  // value x

    wifi_ap_record_t *ap = &ap_list[list_selected];

    // SSID title row (highlighted)
    char ssid[34];
    strncpy(ssid, (char *)ap->ssid, 32);
    ssid[32] = '\0';
    bool hidden = (strlen(ssid) == 0);
    if (hidden) {
        snprintf(ssid, sizeof(ssid), "(hidden)");
    }
    pax_col_t ap_color = get_ap_color(ap->bssid);
    pax_draw_text(&fb, ap_color, pax_font_sky_mono, 18, lx, top + 2, ssid);
    top += 24;

    // Draw a dim separator
    pax_simple_rect(&fb, COLOR_DIM, lx, top, w - lx * 2, 1);
    top += 4;

    // Helper macro for label/value rows
    #define ROW(label, fmt, ...) do { \
        pax_draw_text(&fb, COLOR_DIM,  pax_font_sky_mono, 13, lx, top + 2, label); \
        char _v[64]; snprintf(_v, sizeof(_v), fmt, ##__VA_ARGS__); \
        pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 13, vx, top + 2, _v); \
        top += row; \
    } while (0)

    // MAC address
    ROW("BSSID:",   "%02X:%02X:%02X:%02X:%02X:%02X",
        ap->bssid[0], ap->bssid[1], ap->bssid[2],
        ap->bssid[3], ap->bssid[4], ap->bssid[5]);

    // Channel
    ROW("Channel:", "%u", ap->primary);

    // RSSI with signal quality color
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, lx, top + 2, "RSSI:");
    {
        char v[16];
        snprintf(v, sizeof(v), "%d dBm", ap->rssi);
        pax_draw_text(&fb, rssi_color(ap->rssi), pax_font_sky_mono, 13, vx, top + 2, v);
    }
    top += row;

    // Auth mode
    ROW("Auth:",     "%s", auth_text(ap->authmode));

    // Cipher (only if not open)
    if (ap->authmode != WIFI_AUTH_OPEN) {
        ROW("Pair cipher:", "%s", cipher_text(ap->pairwise_cipher));
        ROW("Group cipher:", "%s", cipher_text(ap->group_cipher));
    }

    // Bandwidth
    ROW("Bandwidth:", "%s", bw_text(ap->bandwidth));

    // PHY standards
    {
        char phy[48] = "";
        if (ap->phy_11b)  strncat(phy, "11b ", sizeof(phy) - strlen(phy) - 1);
        if (ap->phy_11g)  strncat(phy, "11g ", sizeof(phy) - strlen(phy) - 1);
        if (ap->phy_11n)  strncat(phy, "11n ", sizeof(phy) - strlen(phy) - 1);
        if (ap->phy_11a)  strncat(phy, "11a ", sizeof(phy) - strlen(phy) - 1);
        if (ap->phy_11ac) strncat(phy, "11ac ", sizeof(phy) - strlen(phy) - 1);
        if (ap->phy_11ax) strncat(phy, "11ax ", sizeof(phy) - strlen(phy) - 1);
        if (ap->phy_lr)   strncat(phy, "LR ", sizeof(phy) - strlen(phy) - 1);
        if (strlen(phy) == 0) strcpy(phy, "?");
        ROW("PHY:", "%s", phy);
    }

    // WPS
    ROW("WPS:", "%s", ap->wps ? "yes" : "no");

    // FTM
    if (ap->ftm_responder || ap->ftm_initiator) {
        char ftm[16] = "";
        if (ap->ftm_responder) strncat(ftm, "R ", sizeof(ftm) - strlen(ftm) - 1);
        if (ap->ftm_initiator) strncat(ftm, "I",  sizeof(ftm) - strlen(ftm) - 1);
        ROW("FTM:", "%s", ftm);
    }

    // Country code
    if (ap->country.cc[0] != 0) {
        char cc[4] = {ap->country.cc[0], ap->country.cc[1], ap->country.cc[2], 0};
        ROW("Country:", "%s", cc);
    }

    // Hidden BSSID note
    if (hidden) {
        pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 12, lx, top + 2, "(hidden SSID — showing MAC above)");
    }

    #undef ROW
    (void)h;
}

static void render(void) {
    pax_background(&fb, COLOR_BG);
    render_header();
    if (view == VIEW_CHANNELS) {
        render_channels();
        render_footer("F1/ESC=Exit  Tab=View  R=Rescan  H=Hidden(" \
                      "on/off)");
    } else if (view == VIEW_LIST) {
        render_list();
        render_footer("F1/ESC=Exit  Tab=View  R=Rescan  W/S=Navigate  Enter=Detail  H=Hidden");
    } else if (view == VIEW_GRAPH) {
        render_graph();
        render_footer("F1/ESC=Exit  Tab=View  R=Rescan  H=Hidden");
    } else {
        render_detail();
        render_footer("any key=back to list  F1/ESC=Exit");
    }
    blit();
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) return;

    const bsp_configuration_t bsp_cfg = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
            .num_fbs                = 1,
        },
    };
    if (bsp_device_initialize(&bsp_cfg) != ESP_OK) return;

    res = bsp_display_get_parameters(&display_h_res, &display_v_res, &display_color_format, &display_data_endian);
    if (res == ESP_OK) {
        pax_buf_type_t fmt = PAX_BUF_24_888RGB;
        switch (display_color_format) {
            case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:   fmt = PAX_BUF_16_565RGB;   break;
            case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB: fmt = PAX_BUF_32_8888ARGB; break;
            default: break;
        }
        bsp_display_rotation_t rot = bsp_display_get_default_rotation();
        pax_orientation_t ori = PAX_O_UPRIGHT;
        switch (rot) {
            case BSP_DISPLAY_ROTATION_90:  ori = PAX_O_ROT_CCW;  break;
            case BSP_DISPLAY_ROTATION_180: ori = PAX_O_ROT_HALF; break;
            case BSP_DISPLAY_ROTATION_270: ori = PAX_O_ROT_CW;   break;
            default: break;
        }
        pax_buf_init(&fb, NULL, display_h_res, display_v_res, fmt);
        pax_buf_reversed(&fb, display_data_endian == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&fb, ori);
    }

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    show_message("Starting WiFi radio...");
    if (wifi_remote_initialize() == ESP_OK) {
        show_message("Starting WiFi stack...");
        wifi_connection_init_stack();
        wifi_ready = true;
    } else {
        bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
        ESP_LOGE(TAG, "WiFi radio unavailable");
        show_message("WiFi radio unavailable - cannot scan");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (wifi_ready) do_scan();
    render();

    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) != pdTRUE) continue;

        if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state) {
            switch (event.args_navigation.key) {
                case BSP_INPUT_NAVIGATION_KEY_F1:
                    bsp_device_restart_to_launcher();
                    break;
                case BSP_INPUT_NAVIGATION_KEY_ESC:
                    if (view == VIEW_DETAIL) { view = VIEW_LIST; render(); }
                    else bsp_device_restart_to_launcher();
                    break;
                case BSP_INPUT_NAVIGATION_KEY_RETURN:
                    if (view == VIEW_LIST && ap_count > 0) { view = VIEW_DETAIL; render(); }
                    else if (view == VIEW_DETAIL)           { view = VIEW_LIST;   render(); }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_UP:
                    if (view == VIEW_LIST) { list_step(-1); render(); }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_DOWN:
                    if (view == VIEW_LIST) { list_step(+1); render(); }
                    break;
                case BSP_INPUT_NAVIGATION_KEY_TAB:
                    if (view != VIEW_DETAIL) { view = (view + 1) % 3; list_scroll = 0; render(); }
                    break;
                default:
                    break;
            }
        }

        if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            char c = event.args_keyboard.ascii;

            if (c == 0x1B) {  // ESC (ASCII fallback)
                if (view == VIEW_DETAIL) { view = VIEW_LIST; render(); }
                else bsp_device_restart_to_launcher();

            } else if (view == VIEW_DETAIL) {
                // Any printable key goes back to list
                view = VIEW_LIST;
                render();

            } else if (c == 'r' || c == 'R') {
                if (wifi_ready) { do_scan(); render(); }

            } else if (c == 'h' || c == 'H') {
                show_hidden = !show_hidden;
                list_normalize_selection();
                render();

            } else if ((c == 'w' || c == 'W') && view == VIEW_LIST) {
                list_step(-1);
                render();

            } else if ((c == 's' || c == 'S') && view == VIEW_LIST) {
                list_step(+1);
                render();

            } else if ((c == '\r' || c == '\n') && view == VIEW_LIST) {
                if (ap_count > 0) { view = VIEW_DETAIL; render(); }
            }
        }
    }
}
