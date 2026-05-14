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
#define COLOR_BG      0xFF0D1117
#define COLOR_HEADER  0xFF161B22
#define COLOR_ROW_ALT 0xFF1C2128
#define COLOR_TEXT    0xFFCDD9E5
#define COLOR_DIM     0xFF768390
#define COLOR_ACCENT  0xFF539BF5
#define COLOR_GOOD    0xFF57AB5A  // >= -60 dBm
#define COLOR_OK      0xFFCFBA06  // >= -70 dBm
#define COLOR_FAIR    0xFFDB6D28  // >= -80 dBm
#define COLOR_POOR    0xFFE5534B  // <  -80 dBm

#define VIEW_CHANNELS 0
#define VIEW_LIST     1
#define VIEW_GRAPH    2

// 10 distinct colors for SSID curves in graph view
static const pax_col_t GRAPH_COLORS[] = {
    0xFF539BF5, 0xFF57AB5A, 0xFFDB6D28, 0xFFE5534B, 0xFFCFBA06,
    0xFFDCAEFA, 0xFF6CB6FF, 0xFFFFA657, 0xFF79C0FF, 0xFF56D364,
};

// Display state
static size_t                     display_h_res        = 0;
static size_t                     display_v_res        = 0;
static bsp_display_color_format_t display_color_format = 0;
static bsp_display_endianness_t   display_data_endian  = 0;
static pax_buf_t                  fb                   = {0};
static QueueHandle_t              input_event_queue    = NULL;

// App state
static wifi_ap_record_t *ap_list  = NULL;
static uint16_t          ap_count = 0;
static int               view     = VIEW_CHANNELS;
static int               list_scroll = 0;
static bool              wifi_ready  = false;

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

    // wifi_connection_init_stack() leaves WiFi stopped.
    // Must set STA mode and start before scanning (same pattern as wifi_connection_connect).
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
    vTaskDelay(pdMS_TO_TICKS(200)); // brief settle time

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

    list_scroll = 0;
    ESP_LOGI(TAG, "Found %u networks", ap_count);
}

static void render_header(void) {
    int  w = pax_buf_get_width(&fb);
    char title[80];
    const char *view_name = view == VIEW_CHANNELS ? "Channels" : (view == VIEW_LIST ? "List" : "Graph");
    snprintf(title, sizeof(title), "WiFi Analyzer  %u networks  [%s]", ap_count, view_name);
    pax_simple_rect(&fb, COLOR_HEADER, 0, 0, w, 28);
    pax_draw_text(&fb, COLOR_ACCENT, pax_font_sky_mono, 16, 8, 6, title);
}

static void render_footer(void) {
    int w = pax_buf_get_width(&fb);
    int h = pax_buf_get_height(&fb);
    pax_simple_rect(&fb, COLOR_HEADER, 0, h - 22, w, 22);
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, 8, h - 17,
                  "F1/ESC=Exit  Tab=Switch View  R=Rescan  W/S=Scroll");
}

static void render_channels(void) {
    int w         = pax_buf_get_width(&fb);
    int h         = pax_buf_get_height(&fb);
    int top       = 28;
    int bottom    = h - 22;
    int label_h   = 18;
    int content_h = bottom - top - label_h;

    // Strongest RSSI and AP count per channel (1-13)
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

        // Channel number label
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%d", ch);
        pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, x + slot_w / 2 - 5, lbl_y, lbl);

        if (cnt[ch] == 0) continue;

        // Bar height: map -95..-30 dBm → 0..content_h
        int bar_h = (best[ch] + 95) * content_h / 65;
        if (bar_h < 6) bar_h = 6;
        if (bar_h > content_h) bar_h = content_h;

        int bar_x = x + 3;
        int bar_y = lbl_y - bar_h;

        pax_simple_rect(&fb, rssi_color(best[ch]), bar_x, bar_y, bar_w, bar_h);

        // RSSI value above bar
        char rssi_str[6];
        snprintf(rssi_str, sizeof(rssi_str), "%d", best[ch]);
        pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 11, bar_x + 1, bar_y - 14, rssi_str);

        // AP count on bar (if > 1)
        if (cnt[ch] > 1) {
            char cnt_str[12];
            snprintf(cnt_str, sizeof(cnt_str), "%d", cnt[ch]);
            pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 11, bar_x + 2, bar_y + 2, cnt_str);
        }
    }
}

static void render_list(void) {
    int w       = pax_buf_get_width(&fb);
    int h       = pax_buf_get_height(&fb);
    int top     = 28;
    int row_h   = 24;
    int max_vis = (h - 28 - 22 - row_h) / row_h; // subtract column header row

    // Column headers
    pax_simple_rect(&fb, COLOR_HEADER, 0, top, w, row_h);
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, 10,      top + 5, "SSID");
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 170, top + 5, "CH");
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 120, top + 5, "RSSI");
    pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 60,  top + 5, "SEC");
    top += row_h;

    // Scroll indicator
    if (ap_count > (uint16_t)max_vis) {
        char scroll_info[20];
        snprintf(scroll_info, sizeof(scroll_info), "%d/%u", list_scroll + 1, ap_count);
        pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 13, w - 70, 8, scroll_info);
    }

    for (int i = list_scroll; i < ap_count && (i - list_scroll) < max_vis; i++) {
        int y = top + (i - list_scroll) * row_h;

        if ((i % 2) == 0) pax_simple_rect(&fb, COLOR_ROW_ALT, 0, y, w, row_h);

        // SSID — show BSSID (MAC) for hidden networks so manufacturer is identifiable
        char ssid[33];
        strncpy(ssid, (char *)ap_list[i].ssid, 32);
        ssid[32] = '\0';
        if (strlen(ssid) == 0) {
            snprintf(ssid, sizeof(ssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                     ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
        }
        pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 14, 10, y + 5, ssid);

        // Channel
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%2u", ap_list[i].primary);
        pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 14, w - 170, y + 5, tmp);

        // RSSI with signal color
        snprintf(tmp, sizeof(tmp), "%4d", ap_list[i].rssi);
        pax_draw_text(&fb, rssi_color(ap_list[i].rssi), pax_font_sky_mono, 14, w - 120, y + 5, tmp);

        // Security
        const char *sec = (ap_list[i].authmode == WIFI_AUTH_OPEN) ? "open" : "lock";
        pax_draw_text(&fb, COLOR_TEXT, pax_font_sky_mono, 14, w - 60, y + 5, sec);
    }
}

// Gaussian bell curve: value at x given center mu and sigma
static float gauss(float x, float mu, float sigma) {
    float d = (x - mu) / sigma;
    return expf(-0.5f * d * d);
}

// Graph view: one bell curve per SSID, x=channel, y=RSSI strength
static void render_graph(void) {
    int w         = pax_buf_get_width(&fb);
    int h         = pax_buf_get_height(&fb);
    int top       = 28;
    int bottom    = h - 22 - 18; // leave room for channel labels
    int content_h = bottom - top;
    int label_y   = bottom;

    // Channel axis: channels 1-13, map to x pixels
    // Leave margin on sides so channel 1 and 13 have room for bell shape
    float x_margin = 30.0f;
    float x_scale  = (float)(w - 2 * x_margin) / 12.0f; // 12 gaps between ch1..ch13

    // Draw channel labels
    for (int ch = 1; ch <= 13; ch++) {
        int x = (int)(x_margin + (ch - 1) * x_scale);
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "%d", ch);
        pax_draw_text(&fb, COLOR_DIM, pax_font_sky_mono, 12, x - 5, label_y, lbl);
        // Tick mark
        pax_simple_rect(&fb, COLOR_DIM, x, bottom - 4, 1, 4);
    }

    // Horizontal baseline
    pax_simple_rect(&fb, COLOR_DIM, (int)x_margin, bottom, w - 2 * (int)x_margin, 1);

    // RSSI range: -95 (floor) to -25 (ceiling) maps to 0..content_h
    float rssi_floor   = -95.0f;
    float rssi_range   = 70.0f;  // -95 to -25
    float sigma        = 1.8f;   // bell width in channel units (~3 channels wide)

    // Draw one curve per AP, limit to 15 to avoid clutter
    int draw_count = ap_count < 15 ? ap_count : 15;

    // Draw each curve as a polyline (not filled) — 1px wide line per AP
    for (int i = draw_count - 1; i >= 0; i--) {
        float mu        = (float)ap_list[i].primary;
        float norm_rssi = (ap_list[i].rssi - rssi_floor) / rssi_range;
        if (norm_rssi < 0.0f) norm_rssi = 0.0f;
        if (norm_rssi > 1.0f) norm_rssi = 1.0f;

        float     peak_h = norm_rssi * (float)content_h;
        pax_col_t color  = GRAPH_COLORS[i % 10];

        int prev_y = bottom;
        for (int px = 0; px < w; px++) {
            float ch_pos  = ((float)px - x_margin) / x_scale + 1.0f;
            float curve_y = gauss(ch_pos, mu, sigma) * peak_h;
            int   cur_y   = bottom - (int)curve_y;

            // Draw vertical segment between prev and current y to avoid gaps
            int y0 = prev_y < cur_y ? prev_y : cur_y;
            int y1 = prev_y < cur_y ? cur_y  : prev_y;
            if (y1 - y0 > 1) {
                pax_simple_rect(&fb, color, px, y0, 1, y1 - y0);
            } else {
                pax_simple_rect(&fb, color, px, cur_y, 1, 2);
            }
            prev_y = cur_y;
        }
    }

    // Draw SSID labels at curve peaks with de-collision to prevent overlap
    int placed_x[15] = {0};
    int placed_y[15] = {0};
    int placed_count = 0;

    for (int i = 0; i < draw_count; i++) {
        uint8_t ch = ap_list[i].primary;
        if (ch < 1 || ch > 13) continue;

        float norm_rssi = (ap_list[i].rssi - rssi_floor) / rssi_range;
        if (norm_rssi < 0.0f) norm_rssi = 0.0f;
        if (norm_rssi > 1.0f) norm_rssi = 1.0f;

        int peak_px = (int)(x_margin + (ch - 1) * x_scale);
        int lbl_y   = bottom - (int)(norm_rssi * content_h) - 16;

        // De-collision: push label down if it overlaps an already placed label
        bool moved = true;
        while (moved) {
            moved = false;
            for (int j = 0; j < placed_count; j++) {
                if (abs(placed_x[j] - peak_px) < 92 && abs(placed_y[j] - lbl_y) < 14) {
                    lbl_y = placed_y[j] + 14; // shift below conflicting label
                    moved = true;
                }
            }
        }
        if (lbl_y > bottom - 4) lbl_y = bottom - 4;
        if (lbl_y < top + 2)    lbl_y = top + 2;

        placed_x[placed_count] = peak_px;
        placed_y[placed_count] = lbl_y;
        placed_count++;

        // Build label: number + SSID (or OUI for hidden)
        char lbl[20];
        if (strlen((char *)ap_list[i].ssid) == 0) {
            snprintf(lbl, sizeof(lbl), "%d:%02X:%02X:%02X", i + 1,
                     ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2]);
        } else {
            snprintf(lbl, sizeof(lbl), "%d:%.12s", i + 1, (char *)ap_list[i].ssid);
        }

        pax_col_t color = GRAPH_COLORS[i % 10];
        pax_simple_rect(&fb, 0xCC0D1117, peak_px - 2, lbl_y - 1, 92, 14);
        pax_draw_text(&fb, color, pax_font_sky_mono, 12, peak_px, lbl_y, lbl);
    }
}

static void render(void) {
    pax_background(&fb, COLOR_BG);
    render_header();
    if (view == VIEW_CHANNELS) {
        render_channels();
    } else if (view == VIEW_LIST) {
        render_list();
    } else {
        render_graph();
    }
    render_footer();
    blit();
}

void app_main(void) {
    gpio_install_isr_service(0);

    // NVS init
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        res = nvs_flash_init();
    }
    if (res != ESP_OK) return;

    // BSP init
    const bsp_configuration_t bsp_cfg = {
        .display = {
            .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB,
            .num_fbs                = 1,
        },
    };
    if (bsp_device_initialize(&bsp_cfg) != ESP_OK) return;

    // Display init (copied from template to handle all color formats / rotations)
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

    // WiFi init (required init order per Tanmatsu docs)
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

    // Main loop
    while (1) {
        bsp_input_event_t event;
        if (xQueueReceive(input_event_queue, &event, portMAX_DELAY) != pdTRUE) continue;

        if (event.type == INPUT_EVENT_TYPE_NAVIGATION && event.args_navigation.state) {
            if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                bsp_device_restart_to_launcher();
            }
        }

        if (event.type == INPUT_EVENT_TYPE_KEYBOARD) {
            char c = event.args_keyboard.ascii;

            if (c == 0x1B) { // ESC
                bsp_device_restart_to_launcher();

            } else if (c == '\t') { // Tab — cycle views
                view = (view + 1) % 3;
                list_scroll = 0;
                render();

            } else if (c == 'r' || c == 'R') { // Rescan
                if (wifi_ready) {
                    do_scan();
                    render();
                }

            } else if ((c == 'w' || c == 'W') && view == VIEW_LIST) { // Scroll up
                if (list_scroll > 0) {
                    list_scroll--;
                    render();
                }

            } else if ((c == 's' || c == 'S') && view == VIEW_LIST) { // Scroll down
                int w_val = pax_buf_get_width(&fb);
                int h_val = pax_buf_get_height(&fb);
                int row_h   = 24;
                int max_vis = (h_val - 28 - 22 - row_h) / row_h;
                if (list_scroll + max_vis < ap_count) {
                    list_scroll++;
                    render();
                }
            }
        }
    }
}
