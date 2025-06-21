#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <nrf24.h>
#include "fz_nrf24_jammer_icons.h"

#define TAG "nRF24_jammer_app"
#define nrf24 (FuriHalSpiBusHandle*)&furi_hal_spi_bus_handle_external
#define HOLD_DELAY_MS 100

// Nightfall submenu options
typedef enum {
    NIGHTFALL_PS4,
    NIGHTFALL_PS5,
    NIGHTFALL_XBOX,
    NIGHTFALL_SWITCH,
    NIGHTFALL_WIRELESS_AUDIO,
    NIGHTFALL_AIRPODS,
    NIGHTFALL_COUNT
} NightfallMenuType;

typedef enum {
    MENU_BLUETOOTH,
    MENU_DRONE,
    MENU_WIFI,
    MENU_BLE,
    MENU_ZIGBEE,
    MENU_NIGHTFALL,
    MENU_MISC,
    MENU_COUNT
} MenuType;

typedef enum {
    WIFI_MODE_SELECT,
    WIFI_MODE_ALL,
    WIFI_MODE_COUNT
} WifiMode;

typedef enum {
    MISC_STATE_IDLE,
    MISC_STATE_SET_START,
    MISC_STATE_SET_STOP,
    MISC_STATE_ERROR,
} MiscState;

typedef enum {
    MISC_MODE_CHANNEL_SWITCHING,
    MISC_MODE_PACKET_SENDING,
    MISC_MODE_COUNT
} MiscMode;

typedef struct {
    FuriMutex* mutex;
    NotificationApp* notifications;
    FuriThread* thread;
    ViewPort* view_port;
    
    bool is_running;
    bool is_stop;
    bool wifi_menu_active;
    bool show_jamming_started;
    bool wifi_channel_select;
    bool nightfall_menu_active;
    
    MenuType current_menu;
    WifiMode wifi_mode;
    MiscState misc_state;
    NightfallMenuType nightfall_menu;
    MiscMode misc_mode;
    uint8_t wifi_channel;
    uint8_t misc_start;
    uint8_t misc_stop;
    
    InputKey held_key;
    uint32_t hold_counter;
} PluginState;

typedef enum {
    EVENT_TICK,
    EVENT_KEY,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;

const NotificationSequence error_sequence = {
    &message_red_255,
    &message_vibro_on,
    &message_delay_250,
    &message_vibro_off,
    &message_red_0,
    NULL,
};

// --- Bluetooth, Drone, BLE, Zigbee channel arrays ---
static uint8_t bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
static uint8_t drone_channels[125];
static uint8_t ble_channels[] = {2, 26, 80};
static uint8_t zigbee_channels[] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26};

static const int bluetooth_channels_count = sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]);
static const int drone_channels_count = sizeof(drone_channels) / sizeof(drone_channels[0]);
static const int ble_channels_count = sizeof(ble_channels) / sizeof(ble_channels[0]);
static const int zigbee_channels_count = sizeof(zigbee_channels) / sizeof(zigbee_channels[0]);

// --- Nightfall's Options channel arrays ---
static uint8_t nightfall_xbox_channels[] = {7, 17, 27, 37, 47, 57, 67, 75};
static uint8_t nightfall_switch_channels[] = {12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62};
static uint8_t nightfall_wireless_audio_channels[] = {0, 10, 20, 30, 40, 50, 60, 70, 80};
// BLE-relevante Kanäle für BLE-basierte Geräte (PS4, PS5, AirPods/Pro)
static uint8_t nightfall_ble_channels[] = {2, 26, 80}; // BLE: 37, 38, 39

static const int nightfall_xbox_channels_count = sizeof(nightfall_xbox_channels) / sizeof(nightfall_xbox_channels[0]);
static const int nightfall_switch_channels_count = sizeof(nightfall_switch_channels) / sizeof(nightfall_switch_channels[0]);
static const int nightfall_wireless_audio_channels_count = sizeof(nightfall_wireless_audio_channels) / sizeof(nightfall_wireless_audio_channels[0]);
static const int nightfall_ble_channels_count = sizeof(nightfall_ble_channels) / sizeof(nightfall_ble_channels[0]);

static void jam_bluetooth(PluginState* state) {
    nrf24_set_tx_mode(nrf24);
    nrf24_startConstCarrier(nrf24, 7, 0);
    
    while(!state->is_stop) {
        for(int i = 0; i < bluetooth_channels_count && !state->is_stop; i++) {
            nrf24_write_reg(nrf24, REG_RF_CH, bluetooth_channels[i]);
        }
    }
    
    nrf24_stopConstCarrier(nrf24);
}

static void jam_drone(PluginState* state) {
    nrf24_set_tx_mode(nrf24);
    nrf24_startConstCarrier(nrf24, 7, 0);
    
    while(!state->is_stop) {
        for(int i = 0; i < drone_channels_count && !state->is_stop; i++) {
            nrf24_write_reg(nrf24, REG_RF_CH, drone_channels[i]);
        }
    }
    
    nrf24_stopConstCarrier(nrf24);
}

static void jam_ble(PluginState* state) {
    uint8_t mac[] = {0xFF, 0xFF};
    nrf24_configure(nrf24, 2, mac, mac, 2, 1, true, true);

    uint8_t setup;
    nrf24_read_reg(nrf24, REG_RF_SETUP, &setup, 1);
    setup = (setup & 0xF8) | 7;
    nrf24_write_reg(nrf24, REG_RF_SETUP, setup);

    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};
    nrf24_set_tx_mode(nrf24);
    
    while(!state->is_stop) {
        for(int i = 0; i < ble_channels_count && !state->is_stop; i++) {
            nrf24_write_reg(nrf24, REG_RF_CH, ble_channels[i]);
            nrf24_spi_trx(nrf24, tx, NULL, 3, nrf24_TIMEOUT);
        }
    }
}

static void jam_misc(PluginState* state) {
    if(state->misc_mode == MISC_MODE_CHANNEL_SWITCHING){
        nrf24_set_tx_mode(nrf24);
        nrf24_startConstCarrier(nrf24, 7, 0);
    
        while(!state->is_stop) {
            for(uint8_t ch = state->misc_start; ch < state->misc_stop; ch++) {
                nrf24_write_reg(nrf24, REG_RF_CH, ch);
            }
        }
    
        nrf24_stopConstCarrier(nrf24);
    } else {
        uint8_t mac[] = {0xFF, 0xFF};
        nrf24_configure(nrf24, 2, mac, mac, 2, state->misc_start, true, true);

        uint8_t setup;
        nrf24_read_reg(nrf24, REG_RF_SETUP, &setup, 1);
        setup = (setup & 0xF8) | 7;
        nrf24_write_reg(nrf24, REG_RF_SETUP, setup);

        uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};
        nrf24_set_tx_mode(nrf24);

        while(!state->is_stop) {
            for(uint8_t ch = state->misc_start; ch < state->misc_stop; ch++) {
                nrf24_write_reg(nrf24, REG_RF_CH, ch);
                nrf24_spi_trx(nrf24, tx, NULL, 3, nrf24_TIMEOUT);
            }
        }
    }

    nrf24_set_idle(nrf24);
}

static void jam_wifi(PluginState* state) {
    // Für maximale Effizienz: 
    // - WIFI_MODE_ALL: Jammen von RF_CH 1 bis 83 (2402–2483 MHz, deckt alle 2,4 GHz WiFi-Kanäle weltweit ab)
    // - WIFI_MODE_SELECT: Jammen von Mittenfrequenz ±10 (insgesamt 21 Werte) für den gewählten Kanal
    static const uint8_t wifi_rf_channels[13] = {
        1, 6, 11, 16, 21, 26, 31, 36, 41, 46, 51, 56, 61
    };
    uint8_t mac[] = {0xFF, 0xFF};
    nrf24_configure(nrf24, 2, mac, mac, 2, 1, true, true);

    uint8_t setup;
    nrf24_read_reg(nrf24, REG_RF_SETUP, &setup, 1);
    setup = (setup & 0xF8) | 7; // Maximale Sendeleistung
    nrf24_write_reg(nrf24, REG_RF_SETUP, setup);

    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};
    nrf24_set_tx_mode(nrf24);

    while(!state->is_stop) {
        if(state->wifi_mode == WIFI_MODE_ALL) {
            // Jammen von RF_CH 1 bis 83 (WiFi 2,4 GHz Bereich)
            for(int ch = 1; ch <= 83 && !state->is_stop; ch++) {
                nrf24_write_reg(nrf24, REG_RF_CH, ch);
                nrf24_spi_trx(nrf24, tx, NULL, 3, nrf24_TIMEOUT);
            }
        } else {
            // Jammen von Mittenfrequenz ±10 (insgesamt 21 Werte)
            int base_ch = wifi_rf_channels[state->wifi_channel];
            for(int offset = -10; offset <= 10 && !state->is_stop; offset++) {
                int ch = base_ch + offset;
                if(ch >= 0 && ch <= 125) {
                    nrf24_write_reg(nrf24, REG_RF_CH, ch);
                    nrf24_spi_trx(nrf24, tx, NULL, 3, nrf24_TIMEOUT);
                }
            }
        }
    }
    nrf24_set_idle(nrf24);
}

static void jam_zigbee(PluginState* state) {
    uint8_t mac[] = {0xFF, 0xFF};
    nrf24_configure(nrf24, 2, mac, mac, 2, 1, true, true);

    uint8_t setup;
    nrf24_read_reg(nrf24, REG_RF_SETUP, &setup, 1);
    setup = (setup & 0xF8) | 7;
    nrf24_write_reg(nrf24, REG_RF_SETUP, setup);

    uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};
    nrf24_set_tx_mode(nrf24);

    while(!state->is_stop) {
        for(int i = 0; i < zigbee_channels_count && !state->is_stop; i++) {
            for(int ch = 5 + 5 * (zigbee_channels[i] - 11); ch < (5 + 5 * (zigbee_channels[i] - 11)) + 6 && !state->is_stop; ch++) {
                nrf24_write_reg(nrf24, REG_RF_CH, ch);
                nrf24_spi_trx(nrf24, tx, NULL, 3, nrf24_TIMEOUT);
            }
        }
    }
}

static void jam_nightfall(PluginState* state) {
    // BLE-basierte Geräte: PS4, PS5, AirPods/Pro
    if(state->nightfall_menu == NIGHTFALL_PS4 || state->nightfall_menu == NIGHTFALL_PS5 || state->nightfall_menu == NIGHTFALL_AIRPODS) {
        uint8_t mac[] = {0xFF, 0xFF};
        nrf24_configure(nrf24, 2, mac, mac, 2, 1, true, true);
        uint8_t setup;
        nrf24_read_reg(nrf24, REG_RF_SETUP, &setup, 1);
        setup = (setup & 0xF8) | 7;
        nrf24_write_reg(nrf24, REG_RF_SETUP, setup);
        uint8_t tx[3] = {W_TX_PAYLOAD_NOACK, mac[0], mac[1]};
        nrf24_set_tx_mode(nrf24);
        while(!state->is_stop) {
            for(int i = 0; i < nightfall_ble_channels_count && !state->is_stop; i++) {
                nrf24_write_reg(nrf24, REG_RF_CH, nightfall_ble_channels[i]);
                nrf24_spi_trx(nrf24, tx, NULL, 3, nrf24_TIMEOUT);
            }
        }
        nrf24_set_idle(nrf24);
        return;
    }
    // Proprietäre Geräte: Xbox, Switch, Wireless Audio
    nrf24_set_tx_mode(nrf24);
    nrf24_startConstCarrier(nrf24, 7, 0);
    while(!state->is_stop) {
        switch(state->nightfall_menu) {
            case NIGHTFALL_XBOX:
                for(int i = 0; i < nightfall_xbox_channels_count && !state->is_stop; i++)
                    nrf24_write_reg(nrf24, REG_RF_CH, nightfall_xbox_channels[i]);
                break;
            case NIGHTFALL_SWITCH:
                for(int i = 0; i < nightfall_switch_channels_count && !state->is_stop; i++)
                    nrf24_write_reg(nrf24, REG_RF_CH, nightfall_switch_channels[i]);
                break;
            case NIGHTFALL_WIRELESS_AUDIO:
                for(int i = 0; i < nightfall_wireless_audio_channels_count && !state->is_stop; i++)
                    nrf24_write_reg(nrf24, REG_RF_CH, nightfall_wireless_audio_channels[i]);
                break;
            default: break;
        }
    }
    nrf24_stopConstCarrier(nrf24);
}

static int32_t jam_thread(void* ctx) {
    PluginState* state = ctx;
    state->is_running = true;
    state->is_stop = false;

    switch(state->current_menu) {
        case MENU_BLUETOOTH: jam_bluetooth(state); break;
        case MENU_DRONE: jam_drone(state); break;
        case MENU_WIFI: jam_wifi(state); break;
        case MENU_BLE: jam_ble(state); break;
        case MENU_ZIGBEE: jam_zigbee(state); break;
        case MENU_NIGHTFALL: jam_nightfall(state); break;
        case MENU_MISC: jam_misc(state); break;
        default: break;
    }

    nrf24_set_idle(nrf24);
    state->is_running = false;
    if(state->current_menu == MENU_MISC) {
        state->show_jamming_started = false;
    }
    return 0;
}

static void render_settings_screen(Canvas* canvas, PluginState* state) {
    char buffer[32];
    canvas_set_font(canvas, FontPrimary);
    
    if(state->misc_state == MISC_STATE_SET_START) {
        snprintf(buffer, sizeof(buffer), "Start channel: %d", state->misc_start);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, buffer);
        canvas_set_font(canvas, FontSecondary);
        snprintf(buffer, sizeof(buffer), "Mode: %s", 
            state->misc_mode == MISC_MODE_CHANNEL_SWITCHING ? "Channel Switching" : "Packet Sending");
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, buffer);
        canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Press OK to set stop");
    } else if(state->misc_state == MISC_STATE_SET_STOP) {
        snprintf(buffer, sizeof(buffer), "Start: %d Stop: %d", state->misc_start, state->misc_stop);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, buffer);
        canvas_set_font(canvas, FontSecondary);
        snprintf(buffer, sizeof(buffer), "Mode: %s", 
            state->misc_mode == MISC_MODE_CHANNEL_SWITCHING ? "Channel Switching" : "Packet Sending");
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignCenter, buffer);
        if(state->misc_stop > state->misc_start) {
            canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Press OK to start");
        } else {
            canvas_draw_str_aligned(canvas, 64, 40, AlignCenter, AlignCenter, "Error: Start < Stop");
        }
    } else if(state->misc_state == MISC_STATE_ERROR) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Invalid range");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignCenter, "Start must be < Stop");
    }
}

static void render_wifi_channel_select(Canvas* canvas, PluginState* state) {
    char buffer[32];
    canvas_set_font(canvas, FontPrimary);
    snprintf(buffer, sizeof(buffer), "WiFi channel: %d", state->wifi_channel);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, buffer);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignCenter, "Press OK to start");
}

static void render_wifi_menu(Canvas* canvas, PluginState* state) {
    if(state->wifi_mode == WIFI_MODE_ALL) {
        canvas_draw_icon(canvas, 0, 0, &I_wifi_all);
    } else {
        canvas_draw_icon(canvas, 0, 0, &I_wifi_select);
    }
}

static void render_active_jamming(Canvas* canvas, MenuType menu) {
    switch(menu) {
        case MENU_BLUETOOTH: canvas_draw_icon(canvas, 0, 0, &I_bluetooth_jam); break;
        case MENU_DRONE: canvas_draw_icon(canvas, 0, 0, &I_drone_jam); break;
        case MENU_WIFI: canvas_draw_icon(canvas, 0, 0, &I_wifi_jam); break;
        case MENU_BLE: canvas_draw_icon(canvas, 0, 0, &I_ble_jam); break;
        case MENU_ZIGBEE: canvas_draw_icon(canvas, 0, 0, &I_zigbee_jam); break;
        case MENU_NIGHTFALL:
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Nightfall's Jamming");
            break;
        default: break;
    }
}

static void render_menu_icons(Canvas* canvas, MenuType menu) {
    switch(menu) {
        case MENU_BLUETOOTH: canvas_draw_icon(canvas, 0, 0, &I_bluetooth_jammer); break;
        case MENU_DRONE: canvas_draw_icon(canvas, 0, 0, &I_drone_jammer); break;
        case MENU_WIFI: canvas_draw_icon(canvas, 0, 0, &I_wifi_jammer); break;
        case MENU_BLE: canvas_draw_icon(canvas, 0, 0, &I_ble_jammer); break;
        case MENU_ZIGBEE: canvas_draw_icon(canvas, 0, 0, &I_zigbee_jammer); break;
        case MENU_MISC: canvas_draw_icon(canvas, 0, 0, &I_misc_jammer); break;
        case MENU_NIGHTFALL:
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Nightfall's Options");
            break;
        default: break;
    }
}

static void render_nightfall_menu(Canvas* canvas, NightfallMenuType menu) {
    static const char* names[NIGHTFALL_COUNT] = {
        "PS4 Gamepad", "PS5 Gamepad", "Xbox Gamepad", "Switch Gamepad", "Wireless Audio", "AirPods/Pro"
    };
    const int visible_count = 4; // Wie im File-Explorer
    int scroll_offset = 0;
    // Berechne scroll_offset so, dass Auswahl immer sichtbar bleibt
    if(menu < visible_count/2) {
        scroll_offset = 0;
    } else if(menu > NIGHTFALL_COUNT - 1 - visible_count/2) {
        scroll_offset = NIGHTFALL_COUNT - visible_count;
    } else {
        scroll_offset = menu - visible_count/2;
    }
    if(scroll_offset < 0) scroll_offset = 0;
    if(scroll_offset > NIGHTFALL_COUNT - visible_count) scroll_offset = NIGHTFALL_COUNT - visible_count;
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 8, AlignCenter, AlignCenter, "Nightfall's Options");
    canvas_set_font(canvas, FontSecondary);
    // Abstand wie am Anfang: 12 Pixel pro Eintrag
    for(int i = 0; i < visible_count; i++) {
        int idx = scroll_offset + i;
        if(idx >= NIGHTFALL_COUNT) break;
        int y = 22 + i * 12;
        if(idx == menu) {
            // Markiere die aktuelle Auswahl
            canvas_draw_box(canvas, 12, y - 8, 104, 12);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 18, y, names[idx]);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 18, y, names[idx]);
        }
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Nightfall's Options");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, names[menu]);
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter, "OK: Start  BACK: Exit");
}

static void render_callback(Canvas* canvas, void* ctx) {
    PluginState* state = ctx;
    canvas_clear(canvas);
    // Rahmen nur zeichnen, wenn NICHT im Nightfall-Feature-Auswahlmenü
    if(!(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active && !state->is_running)) {
        canvas_draw_frame(canvas, 0, 0, 128, 64);
    }
    canvas_draw_frame(canvas, 0, 0, 128, 64);
    if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active && state->is_running) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Jamming started");
    } else if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
        render_nightfall_menu(canvas, state->nightfall_menu);
    } else if(state->current_menu == MENU_MISC && state->show_jamming_started) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Jamming started");
    }
    else if(state->is_running) {
        render_active_jamming(canvas, state->current_menu);
    }
    else if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
        render_settings_screen(canvas, state);
    }
    else if(state->current_menu == MENU_WIFI) {
        if(state->wifi_menu_active) {
            if(state->wifi_channel_select) {
                render_wifi_channel_select(canvas, state);
            } else {
                render_wifi_menu(canvas, state);
            }
        } else {
            render_menu_icons(canvas, state->current_menu);
        }
    }
    else {
        render_menu_icons(canvas, state->current_menu);
    }
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    PluginEvent plugin_event = {.type = EVENT_KEY, .input = *event};
    furi_message_queue_put(queue, &plugin_event, FuriWaitForever);
}

static void handle_settings_input(PluginState* state, InputKey key) {
    if(key == InputKeyUp || key == InputKeyRight) {
        if(state->misc_state == MISC_STATE_SET_START) {
            if(state->misc_start < 125) state->misc_start++;
        } else if(state->misc_state == MISC_STATE_SET_STOP) {
            if(state->misc_stop < 125) state->misc_stop++;
        }
    } else if(key == InputKeyDown || key == InputKeyLeft) {
        if(state->misc_state == MISC_STATE_SET_START) {
            if(state->misc_start > 0) state->misc_start--;
        } else if(state->misc_state == MISC_STATE_SET_STOP) {
            if(state->misc_stop > 0) state->misc_stop--;
        }
    }

    if(state->misc_state == MISC_STATE_SET_STOP && state->misc_stop <= state->misc_start) {
        state->misc_stop = state->misc_start + 1;
        if(state->misc_stop > 125) state->misc_stop = 125;
    }
}

static void handle_menu_input(PluginState* state, InputKey key) {
    if(key == InputKeyUp || key == InputKeyRight) {
        state->current_menu = (state->current_menu + 1) % MENU_COUNT;
    } else if(key == InputKeyDown || key == InputKeyLeft) {
        state->current_menu = (state->current_menu == 0) ? 
            (MENU_COUNT - 1) : (state->current_menu - 1);
    }
    state->misc_state = MISC_STATE_IDLE;
    state->wifi_menu_active = false;
    state->wifi_channel_select = false;
    state->nightfall_menu_active = false;
}

static void handle_wifi_input(PluginState* state, InputKey key) {
    if(key == InputKeyUp || key == InputKeyRight) {
        state->wifi_channel = (state->wifi_channel + 1) % 13;
    } else if(key == InputKeyDown || key == InputKeyLeft) {
        state->wifi_channel = (state->wifi_channel == 0) ? 12 : (state->wifi_channel - 1);
    }
}

static void handle_nightfall_menu_input(PluginState* state, InputKey key) {

    if(key == InputKeyUp) {
        state->nightfall_menu = (state->nightfall_menu == 0) ? (NIGHTFALL_COUNT - 1) : (state->nightfall_menu - 1);
    } else if(key == InputKeyDown) {
        state->nightfall_menu = (state->nightfall_menu + 1) % NIGHTFALL_COUNT;
    } else if(key == InputKeyLeft || key == InputKeyRight) {
        // Optional: keine Aktion oder wie gehabt
    } else if(key == InputKeyBack) {
        // Always allow exit from submenu
        state->nightfall_menu_active = false;
    }
}

int32_t nRF24_jammer_app(void* p) {
    UNUSED(p);
    PluginState* state = malloc(sizeof(PluginState));
    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(PluginEvent));
    
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    state->notifications = furi_record_open(RECORD_NOTIFICATION);
    state->is_running = false;
    state->is_stop = true;
    state->wifi_menu_active = false;
    state->wifi_channel_select = false;
    state->show_jamming_started = false;
    state->current_menu = MENU_BLUETOOTH;
    state->wifi_mode = WIFI_MODE_ALL;
    state->misc_state = MISC_STATE_IDLE;
    state->wifi_channel = 0;
    state->misc_start = 0;
    state->misc_stop = 0;
    state->held_key = InputKeyMAX;
    state->hold_counter = 0;
    state->nightfall_menu_active = false;
    state->nightfall_menu = NIGHTFALL_PS4;
    state->misc_mode = MISC_MODE_CHANNEL_SWITCHING;
    
    for(int i = 0; i < 125; i++) drone_channels[i] = i;
    
    if(!furi_hal_power_is_otg_enabled()) furi_hal_power_enable_otg();

    state->view_port = view_port_alloc();
    view_port_draw_callback_set(state->view_port, render_callback, state);
    view_port_input_callback_set(state->view_port, input_callback, queue);
    
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, state->view_port, GuiLayerFullscreen);
    
    state->thread = furi_thread_alloc_ex("nRFJammer", 1024, jam_thread, state);
    nrf24_init();
    
    PluginEvent event;
    bool running = true;
    uint32_t last_tick = furi_get_tick();
    
    while(running) {
        FuriStatus status = furi_message_queue_get(queue, &event, 50);
        uint32_t current_tick = furi_get_tick();
        
        if(current_tick - last_tick >= HOLD_DELAY_MS) {
            last_tick = current_tick;
            if(state->held_key != InputKeyMAX && !state->is_running) {
                state->hold_counter++;
                if(state->hold_counter >= 3) {
                    if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
                        handle_settings_input(state, state->held_key);
                        view_port_update(state->view_port);
                    } else if(state->current_menu == MENU_WIFI && state->wifi_channel_select) {
                        handle_wifi_input(state, state->held_key);
                        view_port_update(state->view_port);
                    }
                }
            }
        }
        
        if(status == FuriStatusOk && event.type == EVENT_KEY) {
            if(event.input.type == InputTypePress) {
                switch(event.input.key) {
                case InputKeyUp:
                    state->held_key = InputKeyUp;
                    state->hold_counter = 0;
                    if(!state->is_running) {
                        if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
                            handle_settings_input(state, InputKeyUp);
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyUp);
                            } else {
                                state->wifi_mode = (state->wifi_mode + 1) % WIFI_MODE_COUNT;
                            }
                        } else if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
                            handle_nightfall_menu_input(state, InputKeyUp);
                        } else {
                            handle_menu_input(state, InputKeyUp);
                        }
                    }
                    break;
                    
                case InputKeyDown:
                    state->held_key = InputKeyDown;
                    state->hold_counter = 0;
                    if(!state->is_running) {
                        if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
                            handle_settings_input(state, InputKeyDown);
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyDown);
                            } else {
                                state->wifi_mode = (state->wifi_mode == 0) ? 
                                    (WIFI_MODE_COUNT - 1) : (state->wifi_mode - 1);
                            }
                        } else if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
                            handle_nightfall_menu_input(state, InputKeyDown);
                        } else {
                            handle_menu_input(state, InputKeyDown);
                        }
                    }
                    break;
                    
                case InputKeyOk:
                    if(state->is_running) {
                        // Fix: Allow OK to stop jamming and return to menu, just like BACK
                        state->is_stop = true;
                        furi_thread_join(state->thread);
                        if(state->current_menu == MENU_MISC) {
                            state->show_jamming_started = false;
                        }
                        if(state->current_menu == MENU_NIGHTFALL) {
                            state->is_running = false;
                        }
                        break;
                    }
                    if(!nrf24_check_connected(nrf24)) {
                        notification_message(state->notifications, &error_sequence);
                    } else if(!state->is_running) {
                        if(state->current_menu == MENU_MISC) {
                            if(state->misc_state == MISC_STATE_IDLE) {
                                state->misc_state = MISC_STATE_SET_START;
                                state->misc_start = 0;
                                state->misc_stop = 0;
                            } else if(state->misc_state == MISC_STATE_SET_START) {
                                state->misc_state = MISC_STATE_SET_STOP;
                            } else if(state->misc_state == MISC_STATE_SET_STOP) {
                                if(state->misc_stop > state->misc_start) {
                                    state->show_jamming_started = true;
                                    furi_thread_start(state->thread);
                                } else {
                                    state->misc_state = MISC_STATE_ERROR;
                                    notification_message(state->notifications, &error_sequence);
                                }
                            } else if(state->misc_state == MISC_STATE_ERROR) {
                                state->misc_state = MISC_STATE_SET_STOP;
                            }
                        } else if(state->current_menu == MENU_WIFI) {
                            if(state->wifi_menu_active) {
                                if(state->wifi_channel_select) {
                                    furi_thread_start(state->thread);
                                } else {
                                    if(state->wifi_mode == WIFI_MODE_SELECT) {
                                        state->wifi_channel_select = true;
                                    } else {
                                        furi_thread_start(state->thread);
                                    }
                                }
                            } else {
                                state->wifi_menu_active = true;
                            }
                        } else if(state->current_menu == MENU_NIGHTFALL) {
                            if(state->nightfall_menu_active) {
                                state->is_running = true;
                                furi_thread_start(state->thread);
                            } else {
                                state->nightfall_menu_active = true;
                            }
                        } else {
                            furi_thread_start(state->thread);
                        }
                    }
                    break;
                    
                case InputKeyBack:
                    if(state->is_running) {
                        state->is_stop = true;
                        furi_thread_join(state->thread);
                        if(state->current_menu == MENU_MISC) {
                            state->show_jamming_started = false;
                        }
                        if(state->current_menu == MENU_NIGHTFALL) {
                            state->is_running = false;
                        }
                    } else if(state->current_menu == MENU_MISC) {
                        if(state->misc_state == MISC_STATE_SET_STOP) {
                            state->misc_state = MISC_STATE_SET_START;
                        } else if(state->misc_state == MISC_STATE_SET_START) {
                            state->misc_state = MISC_STATE_IDLE;
                        } else {
                            running = false;
                        }
                    } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                        if(state->wifi_channel_select) {
                            state->wifi_channel_select = false;
                        } else {
                            state->wifi_menu_active = false;
                        }
                    } else if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
                        state->nightfall_menu_active = false;
                    } else {
                        running = false;
                    }
                    break;
                case InputKeyLeft:
                    state->held_key = InputKeyLeft;
                    state->hold_counter = 0;
                    if(!state->is_running) {
                        if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
                            state->misc_mode = (state->misc_mode == 0) ? (MISC_MODE_COUNT - 1) : (state->misc_mode - 1);
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyLeft);
                            } else {
                                state->wifi_mode = (state->wifi_mode == 0) ? 
                                    (WIFI_MODE_COUNT - 1) : (state->wifi_mode - 1);
                            }
                        } else if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
                            handle_nightfall_menu_input(state, InputKeyLeft);
                        } else {
                            handle_menu_input(state, InputKeyLeft);
                        }
                    }
                    break;
                case InputKeyRight:
                    state->held_key = InputKeyRight;
                    state->hold_counter = 0;
                    if(!state->is_running) {
                        if(state->current_menu == MENU_MISC && state->misc_state != MISC_STATE_IDLE) {
                            state->misc_mode = (state->misc_mode + 1) % MISC_MODE_COUNT;
                        } else if(state->current_menu == MENU_WIFI && state->wifi_menu_active) {
                            if(state->wifi_channel_select) {
                                handle_wifi_input(state, InputKeyRight);
                            } else {
                                state->wifi_mode = (state->wifi_mode + 1) % WIFI_MODE_COUNT;
                            }
                        } else if(state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
                            handle_nightfall_menu_input(state, InputKeyRight);
                        } else {
                            handle_menu_input(state, InputKeyRight);
                        }
                    }
                    break;
                default: break;
                }
                view_port_update(state->view_port);
            }
            else if(event.input.type == InputTypeRelease) {
                if(event.input.key == InputKeyUp || event.input.key == InputKeyDown || event.input.key == InputKeyRight || event.input.key == InputKeyLeft) {
                    state->held_key = InputKeyMAX;
                }
            }
        }
        
        if(state->is_running && state->current_menu == MENU_NIGHTFALL && state->nightfall_menu_active) {
            // Remove strict key filtering and revert to original event handling logic
            // Do not intercept or filter keys here; let the main switch/case handle all keys as before
        }
    }
    
    gui_remove_view_port(gui, state->view_port);
    nrf24_deinit();
    view_port_free(state->view_port);
    furi_thread_free(state->thread);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_hal_power_disable_otg();
    furi_mutex_free(state->mutex);
    furi_message_queue_free(queue);
    free(state);
    
    return 0;
}