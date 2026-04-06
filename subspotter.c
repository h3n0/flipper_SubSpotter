#include <furi.h>
#include <furi_hal.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/devices.h>
#include <storage/storage.h>

#include <stdio.h>
#include <string.h>

#define SUBSPOTTER_SCAN_ENTRY_COUNT 6
#define SUBSPOTTER_MAX_SEEN_DEVICES 12
#define SUBSPOTTER_MAX_SAVED_CAPTURES 24
#define SUBSPOTTER_MIN_PACKET_PULSES 16
#define SUBSPOTTER_PACKET_GAP_US 5000U
#define SUBSPOTTER_HOP_INTERVAL_MS 1200U
#define SUBSPOTTER_UI_TICK_MS 100U
#define SUBSPOTTER_LABEL_COUNT 6
#define SUBSPOTTER_SAVE_DIR APP_DATA_PATH("subspotter")
#define SUBSPOTTER_SAVE_FILE APP_DATA_PATH("subspotter/captures.csv")

typedef enum {
    SubSpotterScreenLiveScan,
    SubSpotterScreenSeenDevices,
    SubSpotterScreenSavedCaptures,
    SubSpotterScreenCount,
} SubSpotterScreen;

typedef enum {
    SubSpotterFamilyUnknown,
    SubSpotterFamilyOutdoorThermometer,
    SubSpotterFamilyWeatherStation,
    SubSpotterFamilyDoorWindowSensor,
    SubSpotterFamilyTpmsDemo,
    SubSpotterFamilyIsmBeacon,
} SubSpotterFamily;

typedef struct {
    uint32_t frequency_hz;
    FuriHalSubGhzPreset preset;
    const char* preset_label;
    const char* modulation_label;
} SubSpotterScanEntry;

typedef struct {
    bool active;
    bool ready;
    uint16_t pulse_count;
    uint16_t short_count;
    uint16_t medium_count;
    uint16_t long_count;
    uint32_t total_duration_us;
    uint32_t min_duration_us;
    uint32_t max_duration_us;
    float peak_rssi;
} SubSpotterBurstStats;

typedef struct {
    bool used;
    SubSpotterFamily family;
    char modulation[10];
    char profile[24];
    uint32_t frequency_hz;
    uint32_t last_seen_ms;
    uint32_t repeat_ms;
    float peak_rssi;
    uint16_t packet_length;
    uint16_t hits;
    uint8_t confidence;
} SubSpotterSeenDevice;

typedef struct {
    bool used;
    SubSpotterFamily family;
    char label[18];
    char timestamp[32];
    char modulation[10];
    char profile[24];
    uint32_t frequency_hz;
    uint32_t repeat_ms;
    float peak_rssi;
    uint16_t packet_length;
    uint8_t confidence;
} SubSpotterSavedCapture;

typedef struct {
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
    Storage* storage;
    const SubGhzDevice* radio_device;
    bool running;
    bool radio_ready;
    SubSpotterScreen current_screen;
    uint8_t selected_seen;
    uint8_t selected_saved;
    uint8_t label_index;
    uint8_t current_scan_index;
    uint32_t last_hop_ms;
    float latest_rssi;
    uint16_t activity[3];
    SubSpotterBurstStats current_burst;
    SubSpotterSeenDevice seen_devices[SUBSPOTTER_MAX_SEEN_DEVICES];
    SubSpotterSavedCapture saved_captures[SUBSPOTTER_MAX_SAVED_CAPTURES];
    SubSpotterFamily last_family;
    char last_modulation[10];
    char last_profile[24];
    uint32_t last_frequency_hz;
    uint32_t last_repeat_ms;
    float last_peak_rssi;
    uint16_t last_packet_length;
    uint8_t last_confidence;
    char status_line[28];
} SubSpotterApp;

typedef struct {
    SubSpotterFamily family;
    int modulation_is_ook;
    uint16_t min_packet_length;
    uint16_t max_packet_length;
    uint32_t min_repeat_ms;
    uint32_t max_repeat_ms;
    uint8_t dominant_bucket;
} SubSpotterFingerprintRule;

static const SubSpotterScanEntry subspotter_scan_plan[SUBSPOTTER_SCAN_ENTRY_COUNT] = {
    {433920000U, FuriHalSubGhzPresetOok650Async, "OOK", "OOK"},
    {433920000U, FuriHalSubGhzPreset2FSKDev476Async, "2FSK", "FSK-ish"},
    {868350000U, FuriHalSubGhzPresetOok650Async, "OOK", "OOK"},
    {868350000U, FuriHalSubGhzPreset2FSKDev476Async, "2FSK", "FSK-ish"},
    {915000000U, FuriHalSubGhzPresetOok650Async, "OOK", "OOK"},
    {915000000U, FuriHalSubGhzPreset2FSKDev476Async, "2FSK", "FSK-ish"},
};

static const char* subspotter_label_options[SUBSPOTTER_LABEL_COUNT] = {
    "Field Note",
    "Greenhouse",
    "Window Rig",
    "Beacon Demo",
    "TPMS Lab",
    "Custom Tag",
};

static const SubSpotterFingerprintRule subspotter_rules[] = {
    {SubSpotterFamilyOutdoorThermometer, 1, 60, 220, 1000, 70000, 0},
    {SubSpotterFamilyWeatherStation, 1, 120, 360, 900, 10000, 1},
    {SubSpotterFamilyDoorWindowSensor, 1, 24, 120, 150, 2500, 0},
    {SubSpotterFamilyTpmsDemo, 0, 120, 520, 4000, 120000, 1},
    {SubSpotterFamilyIsmBeacon, -1, 12, 90, 150, 5000, 2},
};

static const char* subspotter_family_name(SubSpotterFamily family) {
    switch(family) {
    case SubSpotterFamilyOutdoorThermometer:
        return "Outdoor thermometer";
    case SubSpotterFamilyWeatherStation:
        return "Weather station";
    case SubSpotterFamilyDoorWindowSensor:
        return "Door/window sensor";
    case SubSpotterFamilyTpmsDemo:
        return "TPMS demo";
    case SubSpotterFamilyIsmBeacon:
        return "ISM beacon";
    case SubSpotterFamilyUnknown:
    default:
        return "Unknown burst";
    }
}

static const char* subspotter_family_short_name(SubSpotterFamily family) {
    switch(family) {
    case SubSpotterFamilyOutdoorThermometer:
        return "Thermo";
    case SubSpotterFamilyWeatherStation:
        return "Weather";
    case SubSpotterFamilyDoorWindowSensor:
        return "Door/Win";
    case SubSpotterFamilyTpmsDemo:
        return "TPMS";
    case SubSpotterFamilyIsmBeacon:
        return "Beacon";
    case SubSpotterFamilyUnknown:
    default:
        return "Unknown";
    }
}

static uint8_t subspotter_get_band_index(uint32_t frequency_hz) {
    if(frequency_hz < 500000000U) {
        return 0;
    } else if(frequency_hz < 900000000U) {
        return 1;
    }

    return 2;
}

static bool subspotter_scan_entry_is_ook(const SubSpotterScanEntry* entry) {
    return (entry->preset == FuriHalSubGhzPresetOok270Async) ||
           (entry->preset == FuriHalSubGhzPresetOok650Async);
}

static void subspotter_reset_burst(SubSpotterBurstStats* burst) {
    memset(burst, 0, sizeof(SubSpotterBurstStats));
}

static void subspotter_format_frequency_short(
    char* buffer,
    size_t buffer_size,
    uint32_t frequency_hz) {
    snprintf(buffer, buffer_size, "%lu.%02lu", frequency_hz / 1000000UL, (frequency_hz / 10000UL) % 100UL);
}

static void subspotter_format_profile(
    char* buffer,
    size_t buffer_size,
    uint16_t short_count,
    uint16_t medium_count,
    uint16_t long_count) {
    snprintf(buffer, buffer_size, "S%u M%u L%u", short_count, medium_count, long_count);
}

static size_t subspotter_count_seen_devices(SubSpotterApp* app) {
    size_t count = 0;
    for(size_t i = 0; i < SUBSPOTTER_MAX_SEEN_DEVICES; i++) {
        if(app->seen_devices[i].used) {
            count++;
        }
    }
    return count;
}

static size_t subspotter_count_saved_captures(SubSpotterApp* app) {
    size_t count = 0;
    for(size_t i = 0; i < SUBSPOTTER_MAX_SAVED_CAPTURES; i++) {
        if(app->saved_captures[i].used) {
            count++;
        }
    }
    return count;
}

static int32_t subspotter_get_seen_index_by_used_position(SubSpotterApp* app, size_t position) {
    size_t current = 0;
    for(size_t i = 0; i < SUBSPOTTER_MAX_SEEN_DEVICES; i++) {
        if(!app->seen_devices[i].used) {
            continue;
        }

        if(current == position) {
            return (int32_t)i;
        }
        current++;
    }

    return -1;
}

static int32_t subspotter_get_saved_index_by_used_position(SubSpotterApp* app, size_t position) {
    size_t current = 0;
    for(size_t i = 0; i < SUBSPOTTER_MAX_SAVED_CAPTURES; i++) {
        if(!app->saved_captures[i].used) {
            continue;
        }

        if(current == position) {
            return (int32_t)i;
        }
        current++;
    }

    return -1;
}

static size_t subspotter_get_seen_position(SubSpotterApp* app, uint8_t selected_index) {
    size_t position = 0;
    for(size_t i = 0; i < SUBSPOTTER_MAX_SEEN_DEVICES; i++) {
        if(!app->seen_devices[i].used) {
            continue;
        }

        if(i == selected_index) {
            return position;
        }
        position++;
    }

    return 0;
}

static size_t subspotter_get_saved_position(SubSpotterApp* app, uint8_t selected_index) {
    size_t position = 0;
    for(size_t i = 0; i < SUBSPOTTER_MAX_SAVED_CAPTURES; i++) {
        if(!app->saved_captures[i].used) {
            continue;
        }

        if(i == selected_index) {
            return position;
        }
        position++;
    }

    return 0;
}

static void subspotter_draw_tabs(Canvas* canvas, SubSpotterScreen current_screen) {
    static const char* tabs[SubSpotterScreenCount] = {"LIVE", "SEEN", "SAVED"};

    for(size_t i = 0; i < SubSpotterScreenCount; i++) {
        const int32_t x = 2 + (int32_t)(i * 42U);
        if(i == current_screen) {
            canvas_draw_box(canvas, x, 1, 40, 11);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, x + 9, 9, tabs[i]);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_frame(canvas, x, 1, 40, 11);
            canvas_draw_str(canvas, x + 9, 9, tabs[i]);
        }
    }
}

static void subspotter_draw_compact_meter(Canvas* canvas, int32_t x, int32_t y, uint8_t value, const char* label) {
    const uint8_t width = 24;
    const uint8_t fill = (uint8_t)((value * width) / 100U);
    canvas_draw_str(canvas, x, y, label);
    canvas_draw_frame(canvas, x + 18, y - 7, width, 6);
    if(fill > 0) {
        canvas_draw_box(canvas, x + 19, y - 6, fill, 4);
    }
}

static uint8_t subspotter_dominant_bucket(const SubSpotterBurstStats* burst) {
    uint16_t dominant = burst->short_count;
    uint8_t index = 0;

    if(burst->medium_count > dominant) {
        dominant = burst->medium_count;
        index = 1;
    }

    if(burst->long_count > dominant) {
        index = 2;
    }

    return index;
}

static uint8_t subspotter_match_family(
    const SubSpotterBurstStats* burst,
    bool modulation_is_ook,
    uint32_t repeat_ms,
    SubSpotterFamily* family) {
    uint8_t best_score = 15;
    SubSpotterFamily best_family = SubSpotterFamilyUnknown;
    const uint8_t dominant_bucket = subspotter_dominant_bucket(burst);

    for(size_t i = 0; i < COUNT_OF(subspotter_rules); i++) {
        const SubSpotterFingerprintRule* rule = &subspotter_rules[i];
        uint8_t score = 0;

        if((rule->modulation_is_ook == -1) || (rule->modulation_is_ook == (modulation_is_ook ? 1 : 0))) {
            score += 35;
        }

        if((burst->pulse_count >= rule->min_packet_length) &&
           (burst->pulse_count <= rule->max_packet_length)) {
            score += 25;
        }

        if(repeat_ms == 0U) {
            score += 10;
        } else if((repeat_ms >= rule->min_repeat_ms) && (repeat_ms <= rule->max_repeat_ms)) {
            score += 20;
        }

        if(dominant_bucket == rule->dominant_bucket) {
            score += 20;
        }

        if(burst->max_duration_us > 2000U) {
            score += 5;
        }

        if(score > best_score) {
            best_score = score;
            best_family = rule->family;
        }
    }

    *family = best_family;
    return best_score;
}

static int32_t subspotter_find_seen_device(
    SubSpotterApp* app,
    uint32_t frequency_hz,
    const char* modulation,
    uint16_t packet_length) {
    for(size_t i = 0; i < SUBSPOTTER_MAX_SEEN_DEVICES; i++) {
        SubSpotterSeenDevice* device = &app->seen_devices[i];
        if(!device->used) {
            continue;
        }

        if((subspotter_get_band_index(device->frequency_hz) == subspotter_get_band_index(frequency_hz)) &&
           (strncmp(device->modulation, modulation, sizeof(device->modulation)) == 0) &&
           (device->packet_length + 24U >= packet_length) &&
           (packet_length + 24U >= device->packet_length)) {
            return (int32_t)i;
        }
    }

    return -1;
}

static int32_t subspotter_allocate_seen_device(SubSpotterApp* app) {
    for(size_t i = 0; i < SUBSPOTTER_MAX_SEEN_DEVICES; i++) {
        if(!app->seen_devices[i].used) {
            return (int32_t)i;
        }
    }

    uint8_t oldest_index = 0;
    uint32_t oldest_tick = app->seen_devices[0].last_seen_ms;
    for(size_t i = 1; i < SUBSPOTTER_MAX_SEEN_DEVICES; i++) {
        if(app->seen_devices[i].last_seen_ms < oldest_tick) {
            oldest_tick = app->seen_devices[i].last_seen_ms;
            oldest_index = i;
        }
    }

    return oldest_index;
}

static bool subspotter_ensure_save_dir(SubSpotterApp* app) {
    if(!app->storage) {
        return false;
    }

    if(storage_dir_exists(app->storage, SUBSPOTTER_SAVE_DIR)) {
        return true;
    }

    return storage_common_mkdir(app->storage, SUBSPOTTER_SAVE_DIR) == FSE_OK;
}

static void subspotter_append_capture_log(
    SubSpotterApp* app,
    const SubSpotterSavedCapture* capture) {
    if(!app->storage || !subspotter_ensure_save_dir(app)) {
        return;
    }

    File* file = storage_file_alloc(app->storage);
    if(!file) {
        return;
    }

    if(storage_file_open(file, SUBSPOTTER_SAVE_FILE, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[192];
        const int32_t length = snprintf(
            line,
            sizeof(line),
            "%s,%lu,%.1f,%s,%s,%lu,%u,%s,%u,%s\n",
            capture->timestamp,
            capture->frequency_hz,
            (double)capture->peak_rssi,
            capture->modulation,
            subspotter_family_name(capture->family),
            capture->repeat_ms,
            capture->packet_length,
            capture->profile,
            capture->confidence,
            capture->label);

        if(length > 0) {
            storage_file_write(file, line, (size_t)length);
            storage_file_sync(file);
        }
    }

    storage_file_close(file);
    storage_file_free(file);
}

static void subspotter_timestamp_now(char* buffer, size_t buffer_size) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        buffer,
        buffer_size,
        "%04u-%02u-%02u %02u:%02u:%02u",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
}

static void subspotter_store_saved_capture(
    SubSpotterApp* app,
    SubSpotterFamily family,
    const char* modulation,
    const char* profile,
    uint32_t frequency_hz,
    uint32_t repeat_ms,
    float peak_rssi,
    uint16_t packet_length,
    uint8_t confidence) {
    uint8_t slot = 0;

    for(size_t i = 0; i < SUBSPOTTER_MAX_SAVED_CAPTURES; i++) {
        if(!app->saved_captures[i].used) {
            slot = i;
            goto fill_slot;
        }
    }

    slot = SUBSPOTTER_MAX_SAVED_CAPTURES - 1;

fill_slot:
    if(slot == (SUBSPOTTER_MAX_SAVED_CAPTURES - 1)) {
        for(size_t i = 1; i < SUBSPOTTER_MAX_SAVED_CAPTURES; i++) {
            app->saved_captures[i - 1] = app->saved_captures[i];
        }
    }

    SubSpotterSavedCapture* capture = &app->saved_captures[slot];
    memset(capture, 0, sizeof(SubSpotterSavedCapture));
    capture->used = true;
    capture->family = family;
    strncpy(capture->label, subspotter_label_options[app->label_index], sizeof(capture->label) - 1U);
    strncpy(capture->modulation, modulation, sizeof(capture->modulation) - 1U);
    strncpy(capture->profile, profile, sizeof(capture->profile) - 1U);
    capture->frequency_hz = frequency_hz;
    capture->repeat_ms = repeat_ms;
    capture->peak_rssi = peak_rssi;
    capture->packet_length = packet_length;
    capture->confidence = confidence;
    subspotter_timestamp_now(capture->timestamp, sizeof(capture->timestamp));
    subspotter_append_capture_log(app, capture);
}

static void subspotter_finalize_burst(SubSpotterApp* app) {
    const SubSpotterScanEntry* entry = &subspotter_scan_plan[app->current_scan_index];
    SubSpotterBurstStats burst = app->current_burst;
    uint32_t now_ms = furi_get_tick();

    if(burst.pulse_count < SUBSPOTTER_MIN_PACKET_PULSES) {
        subspotter_reset_burst(&app->current_burst);
        return;
    }

    int32_t existing_index =
        subspotter_find_seen_device(app, entry->frequency_hz, entry->modulation_label, burst.pulse_count);
    uint32_t repeat_ms = 0;

    if(existing_index >= 0) {
        repeat_ms = now_ms - app->seen_devices[existing_index].last_seen_ms;
    }

    SubSpotterFamily family = SubSpotterFamilyUnknown;
    uint8_t confidence =
        subspotter_match_family(&burst, subspotter_scan_entry_is_ook(entry), repeat_ms, &family);

    if(existing_index < 0) {
        existing_index = subspotter_allocate_seen_device(app);
        memset(&app->seen_devices[existing_index], 0, sizeof(SubSpotterSeenDevice));
        app->seen_devices[existing_index].used = true;
    }

    SubSpotterSeenDevice* device = &app->seen_devices[existing_index];
    device->family = family;
    strncpy(device->modulation, entry->modulation_label, sizeof(device->modulation) - 1U);
    subspotter_format_profile(
        device->profile,
        sizeof(device->profile),
        burst.short_count,
        burst.medium_count,
        burst.long_count);
    device->frequency_hz = entry->frequency_hz;
    device->repeat_ms = repeat_ms;
    device->peak_rssi = burst.peak_rssi;
    device->packet_length = burst.pulse_count;
    device->confidence = confidence;
    device->hits++;
    device->last_seen_ms = now_ms;

    app->activity[subspotter_get_band_index(entry->frequency_hz)]++;
    app->last_family = family;
    strncpy(app->last_modulation, entry->modulation_label, sizeof(app->last_modulation) - 1U);
    strncpy(app->last_profile, device->profile, sizeof(app->last_profile) - 1U);
    app->last_frequency_hz = entry->frequency_hz;
    app->last_repeat_ms = repeat_ms;
    app->last_peak_rssi = burst.peak_rssi;
    app->last_packet_length = burst.pulse_count;
    app->last_confidence = confidence;
    snprintf(app->status_line, sizeof(app->status_line), "Latest: %s", subspotter_family_name(family));

    subspotter_reset_burst(&app->current_burst);
}

static void subspotter_capture_callback(bool level, uint32_t duration, void* context) {
    UNUSED(level);
    SubSpotterApp* app = context;
    SubSpotterBurstStats* burst = &app->current_burst;

    if(duration == 0U) {
        return;
    }

    if(burst->ready) {
        return;
    }

    if(duration > SUBSPOTTER_PACKET_GAP_US) {
        if(burst->pulse_count >= SUBSPOTTER_MIN_PACKET_PULSES) {
            burst->ready = true;
        } else {
            subspotter_reset_burst(burst);
        }
        return;
    }

    if(!burst->active) {
        burst->active = true;
        burst->min_duration_us = duration;
        burst->peak_rssi = app->latest_rssi;
    }

    burst->pulse_count++;
    burst->total_duration_us += duration;

    if(duration < burst->min_duration_us) {
        burst->min_duration_us = duration;
    }

    if(duration > burst->max_duration_us) {
        burst->max_duration_us = duration;
    }

    if(app->latest_rssi > burst->peak_rssi) {
        burst->peak_rssi = app->latest_rssi;
    }

    if(duration < 450U) {
        burst->short_count++;
    } else if(duration < 1500U) {
        burst->medium_count++;
    } else {
        burst->long_count++;
    }
}

static bool subspotter_apply_scan_entry(SubSpotterApp* app, uint8_t scan_index) {
    if(!app->radio_ready || !app->radio_device) {
        return false;
    }

    subghz_devices_stop_async_rx(app->radio_device);
    subghz_devices_idle(app->radio_device);

    if(app->current_burst.active || app->current_burst.ready) {
        subspotter_finalize_burst(app);
    }

    app->current_scan_index = scan_index % SUBSPOTTER_SCAN_ENTRY_COUNT;
    const SubSpotterScanEntry* entry = &subspotter_scan_plan[app->current_scan_index];

    subghz_devices_load_preset(app->radio_device, entry->preset, NULL);
    subghz_devices_set_frequency(app->radio_device, entry->frequency_hz);
    subghz_devices_flush_rx(app->radio_device);
    subghz_devices_set_rx(app->radio_device);
    subghz_devices_start_async_rx(app->radio_device, subspotter_capture_callback, app);

    return true;
}

static bool subspotter_init_radio(SubSpotterApp* app) {
    subghz_devices_init();
    app->radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!app->radio_device) {
        strncpy(app->status_line, "Radio device not found", sizeof(app->status_line) - 1U);
        return false;
    }

    if(!subghz_devices_begin(app->radio_device)) {
        strncpy(app->status_line, "Radio busy or unavailable", sizeof(app->status_line) - 1U);
        return false;
    }

    app->radio_ready = true;
    strncpy(app->status_line, "Passive scan running", sizeof(app->status_line) - 1U);
    return subspotter_apply_scan_entry(app, 0);
}

static void subspotter_deinit_radio(SubSpotterApp* app) {
    if(!app->radio_ready || !app->radio_device) {
        subghz_devices_deinit();
        return;
    }

    subghz_devices_stop_async_rx(app->radio_device);
    subghz_devices_idle(app->radio_device);
    subghz_devices_sleep(app->radio_device);
    subghz_devices_end(app->radio_device);
    app->radio_ready = false;
    subghz_devices_deinit();
}

static void subspotter_save_latest(SubSpotterApp* app) {
    if(app->last_frequency_hz == 0U) {
        snprintf(app->status_line, sizeof(app->status_line), "No capture to save yet");
        return;
    }

    subspotter_store_saved_capture(
        app,
        app->last_family,
        app->last_modulation,
        app->last_profile,
        app->last_frequency_hz,
        app->last_repeat_ms,
        app->last_peak_rssi,
        app->last_packet_length,
        app->last_confidence);
    snprintf(app->status_line, sizeof(app->status_line), "Saved as %s", subspotter_label_options[app->label_index]);
}

static void subspotter_draw_activity_bars(Canvas* canvas, SubSpotterApp* app) {
    uint16_t peak = 1;
    for(size_t i = 0; i < 3; i++) {
        if(app->activity[i] > peak) {
            peak = app->activity[i];
        }
    }

    for(size_t i = 0; i < 3; i++) {
        const uint8_t bar_width = (uint8_t)((app->activity[i] * 26U) / peak);
        const int32_t y = 35 + (int32_t)(i * 6U);
        const char* label = (i == 0U) ? "433" : ((i == 1U) ? "868" : "915");

        canvas_draw_str(canvas, 4, y, label);
        canvas_draw_frame(canvas, 28, y - 5, 28, 5);
        if(bar_width > 0U) {
            canvas_draw_box(canvas, 29, y - 4, bar_width, 3);
        }
    }
}

static void subspotter_draw_live_scan(Canvas* canvas, SubSpotterApp* app) {
    char line[32];
    char frequency[12];
    uint8_t rssi_pct = 0;
    const SubSpotterScanEntry* entry = &subspotter_scan_plan[app->current_scan_index];

    if(app->latest_rssi <= -100.0f) {
        rssi_pct = 0U;
    } else if(app->latest_rssi >= -40.0f) {
        rssi_pct = 100U;
    } else {
        rssi_pct = (uint8_t)((app->latest_rssi + 100.0f) * 100.0f / 60.0f);
    }

    subspotter_draw_tabs(canvas, app->current_screen);
    canvas_set_font(canvas, FontSecondary);
    subspotter_format_frequency_short(frequency, sizeof(frequency), entry->frequency_hz);
    snprintf(line, sizeof(line), "%s %s", frequency, entry->preset_label);
    canvas_draw_str(canvas, 4, 20, line);
    snprintf(line, sizeof(line), "Burst %s", subspotter_family_short_name(app->last_family));
    canvas_draw_str(canvas, 4, 28, line);
    snprintf(line, sizeof(line), "Len %u Rep %lu", app->last_packet_length, app->last_repeat_ms);
    canvas_draw_str(canvas, 64, 20, line);
    snprintf(line, sizeof(line), "%s %u%%", app->last_modulation[0] ? app->last_modulation : "--", app->last_confidence);
    canvas_draw_str(canvas, 64, 28, line);

    subspotter_draw_activity_bars(canvas, app);
    subspotter_draw_compact_meter(canvas, 64, 38, rssi_pct, "RSSI");
    canvas_draw_str(canvas, 64, 46, app->last_profile[0] ? app->last_profile : "S0 M0 L0");
    elements_button_left(canvas, "Seen");
    elements_button_center(canvas, "Save");
    elements_button_right(canvas, "Saved");
}

static void subspotter_draw_seen_devices(Canvas* canvas, SubSpotterApp* app) {
    const size_t used_count = subspotter_count_seen_devices(app);
    const size_t selected_position = subspotter_get_seen_position(app, app->selected_seen);
    const size_t window_start = (selected_position >= 2U) ? (selected_position - 2U) : 0U;

    subspotter_draw_tabs(canvas, app->current_screen);
    canvas_set_font(canvas, FontSecondary);

    if(used_count == 0U) {
        canvas_draw_str(canvas, 6, 24, "No fingerprints yet.");
        canvas_draw_str(canvas, 6, 34, "Leave LIVE open near");
        canvas_draw_str(canvas, 6, 44, "your weather or test gear.");
    } else {
        for(size_t row = 0; row < 2U; row++) {
            const int32_t index = subspotter_get_seen_index_by_used_position(app, window_start + row);
            if(index < 0) {
                break;
            }

            const SubSpotterSeenDevice* device = &app->seen_devices[index];
            const int32_t y = 22 + (int32_t)(row * 12U);
            char left[18];
            char right[24];

            if((uint8_t)index == app->selected_seen) {
                canvas_draw_box(canvas, 2, y - 7, 124, 9);
                canvas_set_color(canvas, ColorWhite);
            }

            snprintf(left, sizeof(left), "%s", subspotter_family_short_name(device->family));
            snprintf(right, sizeof(right), "%s %u%% %u", device->modulation, device->confidence, device->packet_length);
            canvas_draw_str(canvas, 6, y, left);
            canvas_draw_str(canvas, 60, y, right);

            if((uint8_t)index == app->selected_seen) {
                canvas_set_color(canvas, ColorBlack);
            }
        }
    }

    if(used_count > 0U && app->seen_devices[app->selected_seen].used) {
        char line[32];
        const SubSpotterSeenDevice* selected = &app->seen_devices[app->selected_seen];
        canvas_draw_frame(canvas, 3, 41, 122, 10);
        snprintf(line, sizeof(line), "%s H%u R%lu", selected->profile, selected->hits, selected->repeat_ms);
        canvas_draw_str(canvas, 6, 48, line);
        if(used_count > 2U) {
            elements_scrollbar_pos(canvas, 124, 14, 37, selected_position, used_count);
        }
    }

    elements_button_left(canvas, "Live");
    elements_button_center(canvas, "Save");
    elements_button_right(canvas, "Saved");
}

static void subspotter_draw_saved_captures(Canvas* canvas, SubSpotterApp* app) {
    const size_t used_count = subspotter_count_saved_captures(app);
    const size_t selected_position = subspotter_get_saved_position(app, app->selected_saved);
    const size_t window_start = (selected_position >= 2U) ? (selected_position - 2U) : 0U;

    subspotter_draw_tabs(canvas, app->current_screen);
    canvas_set_font(canvas, FontSecondary);

    if(used_count == 0U) {
        canvas_draw_str(canvas, 6, 24, "No saved captures yet.");
        canvas_draw_str(canvas, 6, 34, "Press OK in LIVE or SEEN");
        canvas_draw_str(canvas, 6, 44, "to append to the CSV log.");
        canvas_draw_str(canvas, 6, 50, "/data/subspotter/captures.csv");
    } else {
        for(size_t row = 0; row < 2U; row++) {
            const int32_t index = subspotter_get_saved_index_by_used_position(app, window_start + row);
            if(index < 0) {
                break;
            }

            const SubSpotterSavedCapture* capture = &app->saved_captures[index];
            const int32_t y = 22 + (int32_t)(row * 12U);
            char left[20];
            char right[20];

            if((uint8_t)index == app->selected_saved) {
                canvas_draw_box(canvas, 2, y - 7, 124, 9);
                canvas_set_color(canvas, ColorWhite);
            }

            snprintf(left, sizeof(left), "%s", capture->label);
            snprintf(right, sizeof(right), "%s %u%%", capture->modulation, capture->confidence);
            canvas_draw_str(canvas, 6, y, left);
            canvas_draw_str(canvas, 78, y, right);

            if((uint8_t)index == app->selected_saved) {
                canvas_set_color(canvas, ColorBlack);
            }
        }
    }

    if(used_count > 0U && app->saved_captures[app->selected_saved].used) {
        char line[34];
        const SubSpotterSavedCapture* selected = &app->saved_captures[app->selected_saved];
        canvas_draw_frame(canvas, 3, 41, 122, 10);
        snprintf(
            line,
            sizeof(line),
            "%.8s %lu.%02lu",
            selected->timestamp + 11,
            selected->frequency_hz / 1000000UL,
            (selected->frequency_hz / 10000UL) % 100UL);
        canvas_draw_str(canvas, 6, 48, line);
        if(used_count > 2U) {
            elements_scrollbar_pos(canvas, 124, 14, 37, selected_position, used_count);
        }
    }

    elements_button_left(canvas, "Seen");
    elements_button_right(canvas, "Live");
}

static void subspotter_draw_callback(Canvas* canvas, void* context) {
    SubSpotterApp* app = context;

    canvas_clear(canvas);
    switch(app->current_screen) {
    case SubSpotterScreenLiveScan:
        subspotter_draw_live_scan(canvas, app);
        break;
    case SubSpotterScreenSeenDevices:
        subspotter_draw_seen_devices(canvas, app);
        break;
    case SubSpotterScreenSavedCaptures:
        subspotter_draw_saved_captures(canvas, app);
        break;
    default:
        break;
    }
}

static void subspotter_input_callback(InputEvent* input_event, void* context) {
    SubSpotterApp* app = context;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

static void subspotter_select_next_used_seen(SubSpotterApp* app, bool forward) {
    uint8_t index = app->selected_seen;
    for(size_t attempt = 0; attempt < SUBSPOTTER_MAX_SEEN_DEVICES; attempt++) {
        index = (uint8_t)((index + (forward ? 1U : (SUBSPOTTER_MAX_SEEN_DEVICES - 1U))) % SUBSPOTTER_MAX_SEEN_DEVICES);
        if(app->seen_devices[index].used) {
            app->selected_seen = index;
            return;
        }
    }
}

static void subspotter_select_next_used_saved(SubSpotterApp* app, bool forward) {
    uint8_t index = app->selected_saved;
    for(size_t attempt = 0; attempt < SUBSPOTTER_MAX_SAVED_CAPTURES; attempt++) {
        index = (uint8_t)((index + (forward ? 1U : (SUBSPOTTER_MAX_SAVED_CAPTURES - 1U))) % SUBSPOTTER_MAX_SAVED_CAPTURES);
        if(app->saved_captures[index].used) {
            app->selected_saved = index;
            return;
        }
    }
}

static void subspotter_handle_press(SubSpotterApp* app, InputKey key) {
    switch(key) {
    case InputKeyBack:
        app->running = false;
        return;
    case InputKeyLeft:
        app->current_screen = (SubSpotterScreen)((app->current_screen + SubSpotterScreenCount - 1U) % SubSpotterScreenCount);
        break;
    case InputKeyRight:
        app->current_screen = (SubSpotterScreen)((app->current_screen + 1U) % SubSpotterScreenCount);
        break;
    case InputKeyOk:
        if(app->current_screen != SubSpotterScreenSavedCaptures) {
            subspotter_save_latest(app);
        }
        break;
    case InputKeyUp:
        if(app->current_screen == SubSpotterScreenLiveScan) {
            app->label_index = (uint8_t)((app->label_index + 1U) % SUBSPOTTER_LABEL_COUNT);
            snprintf(app->status_line, sizeof(app->status_line), "Label: %s", subspotter_label_options[app->label_index]);
        } else if(app->current_screen == SubSpotterScreenSeenDevices) {
            subspotter_select_next_used_seen(app, false);
        } else {
            subspotter_select_next_used_saved(app, false);
        }
        break;
    case InputKeyDown:
        if(app->current_screen == SubSpotterScreenLiveScan) {
            app->label_index = (uint8_t)((app->label_index + SUBSPOTTER_LABEL_COUNT - 1U) % SUBSPOTTER_LABEL_COUNT);
            snprintf(app->status_line, sizeof(app->status_line), "Label: %s", subspotter_label_options[app->label_index]);
        } else if(app->current_screen == SubSpotterScreenSeenDevices) {
            subspotter_select_next_used_seen(app, true);
        } else {
            subspotter_select_next_used_saved(app, true);
        }
        break;
    default:
        break;
    }
}

static void subspotter_tick(SubSpotterApp* app) {
    uint32_t now_ms = furi_get_tick();

    if(app->radio_ready && app->radio_device) {
        app->latest_rssi = subghz_devices_get_rssi(app->radio_device);
        if(app->current_burst.ready) {
            subspotter_finalize_burst(app);
        }

        if((now_ms - app->last_hop_ms) >= SUBSPOTTER_HOP_INTERVAL_MS) {
            app->last_hop_ms = now_ms;
            subspotter_apply_scan_entry(app, (uint8_t)(app->current_scan_index + 1U));
        }
    }

    view_port_update(app->view_port);
}

int32_t subspotter_app(void* p) {
    UNUSED(p);

    SubSpotterApp* app = malloc(sizeof(SubSpotterApp));
    memset(app, 0, sizeof(SubSpotterApp));
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->running = true;
    app->current_screen = SubSpotterScreenLiveScan;
    app->label_index = 0;
    strncpy(app->status_line, "Passive scan only", sizeof(app->status_line) - 1U);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, subspotter_draw_callback, app);
    view_port_input_callback_set(app->view_port, subspotter_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    subspotter_init_radio(app);
    app->last_hop_ms = furi_get_tick();

    while(app->running) {
        InputEvent input_event;

        if(furi_message_queue_get(app->input_queue, &input_event, SUBSPOTTER_UI_TICK_MS) == FuriStatusOk) {
            if(input_event.type == InputTypeShort) {
                subspotter_handle_press(app, input_event.key);
            }
        }

        subspotter_tick(app);
    }

    if(app->current_burst.active || app->current_burst.ready) {
        subspotter_finalize_burst(app);
    }

    subspotter_deinit_radio(app);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->input_queue);
    free(app);

    return 0;
}
