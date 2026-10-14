// Lilum BLE stack for the Mandalum pendant.
//
// Implements the lifecycle and GATT surface described in:
//   - lilum-app-prd-v3.2.md §§4, 5, 9
//   - docs/firmware-ble-protocol.md §§3–6
//
// State machine
// -------------
//   Boot ──► ConfigAdv ──► ConfigConnected ──► (Runtime{Master|Follower|Idle})
//                  │                                ▲
//                  └──── 120 s timer expires ──────┘
//                  │
//                  └──── disconnect (any reason) ──► Runtime{...}
//
// Once a Runtime state is entered, the element is *not* connectable until
// the next power-cycle. Master advertises continuously; Follower scans
// continuously; Idle is BLE-quiet (standalone elements).
#include "simple_BLE.h"

#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_nimble_hci.h"

#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "lilum_ble_protocol.h"
#include "lilum_config.h"
#include "lilum_nvs.h"
#include "orchestra_payload.h"
#include "led_engine.h"

#ifndef BLE_HS_CONN_HANDLE_NONE
#define BLE_HS_CONN_HANDLE_NONE 0xFFFF
#endif

static const char *TAG = "BLE";

// ── State ────────────────────────────────────────────────────────────────
enum class BleStateMachine : uint8_t {
    Boot = 0,
    ConfigAdv,          // Connectable extended advertising during 120 s window.
    ConfigConnected,    // Phone is connected; window timer paused.
    RuntimeMaster,      // Non-connectable continuous adv of runtime payload.
    RuntimeFollower,    // Continuous passive scan of master adv.
    RuntimeIdle,        // Standalone; BLE radio silent.
};

static BleStateMachine s_state = BleStateMachine::Boot;

static LilumBinding        s_binding;
static LilumElementConfig  s_element_cfg;
static uint8_t             s_own_addr_type = 0;
static uint16_t            s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static SemaphoreHandle_t   s_host_stopped_sem = NULL;
static bool                s_initialized   = false;
static bool                s_advertising   = false;
static bool                s_scanning      = false;
static bool                s_connected     = false;
static bool                s_follower_synced = false;
static uint8_t             s_live_brightness_override = 0;

// Time the configuration window opened (esp_timer microseconds). Zero when
// the window has already closed.
static int64_t             s_config_window_started_us = 0;
// While a phone is connected the timer is "paused"; we record how many
// seconds were remaining at the moment of connection so we can stop counting.
static uint8_t             s_config_window_paused_remaining_sec = 0;
static bool                s_config_window_paused = false;

// Extended-advertising instance the firmware uses everywhere.
static constexpr uint8_t   kExtAdvInstance = 0;

// Maintenance task: refreshes the config-window adv payload once per second
// (so the remaining-seconds field stays current) and triggers the window
// expiry transition.
static TaskHandle_t        s_maintenance_task = NULL;

// Runtime adv refresh task (master only).
static TaskHandle_t        s_runtime_refresh_task = NULL;

// ── UUIDs ────────────────────────────────────────────────────────────────
static const ble_uuid128_t UUID_SVC_PRIMARY  = { { BLE_UUID_TYPE_128 }, LILUM_SVC_PRIMARY_BYTES  };
static const ble_uuid128_t UUID_SVC_IDENTITY = { { BLE_UUID_TYPE_128 }, LILUM_SVC_IDENTITY_BYTES };
static const ble_uuid128_t UUID_SVC_BINDING  = { { BLE_UUID_TYPE_128 }, LILUM_SVC_BINDING_BYTES  };
static const ble_uuid128_t UUID_SVC_CONFIG   = { { BLE_UUID_TYPE_128 }, LILUM_SVC_CONFIG_BYTES   };
static const ble_uuid128_t UUID_SVC_LIVE     = { { BLE_UUID_TYPE_128 }, LILUM_SVC_LIVE_BYTES     };

static const ble_uuid128_t UUID_ID_TYPE      = { { BLE_UUID_TYPE_128 }, LILUM_CHR_IDENTITY_TYPE_BYTES   };
static const ble_uuid128_t UUID_ID_FWVER     = { { BLE_UUID_TYPE_128 }, LILUM_CHR_IDENTITY_FWVER_BYTES  };
static const ble_uuid128_t UUID_ID_HWREV     = { { BLE_UUID_TYPE_128 }, LILUM_CHR_IDENTITY_HWREV_BYTES  };
static const ble_uuid128_t UUID_ID_SERIAL    = { { BLE_UUID_TYPE_128 }, LILUM_CHR_IDENTITY_SERIAL_BYTES };
static const ble_uuid128_t UUID_ID_CFGINFO   = { { BLE_UUID_TYPE_128 }, LILUM_CHR_IDENTITY_CONFIG_INFO_BYTES };

static const ble_uuid128_t UUID_BIND_PAIR_ID    = { { BLE_UUID_TYPE_128 }, LILUM_CHR_BINDING_PAIR_ID_BYTES    };
static const ble_uuid128_t UUID_BIND_PAIR_ROLE  = { { BLE_UUID_TYPE_128 }, LILUM_CHR_BINDING_PAIR_ROLE_BYTES  };
static const ble_uuid128_t UUID_BIND_GROUP_ID   = { { BLE_UUID_TYPE_128 }, LILUM_CHR_BINDING_GROUP_ID_BYTES   };
static const ble_uuid128_t UUID_BIND_GROUP_ROLE = { { BLE_UUID_TYPE_128 }, LILUM_CHR_BINDING_GROUP_ROLE_BYTES };
static const ble_uuid128_t UUID_BIND_SLOT       = { { BLE_UUID_TYPE_128 }, LILUM_CHR_BINDING_SLOT_BYTES       };

static const ble_uuid128_t UUID_CFG_STANDALONE = { { BLE_UUID_TYPE_128 }, LILUM_CHR_CFG_STANDALONE_BYTES };
static const ble_uuid128_t UUID_CFG_SHARED     = { { BLE_UUID_TYPE_128 }, LILUM_CHR_CFG_SHARED_BYTES     };

static const ble_uuid128_t UUID_LIVE_BRIGHTNESS = { { BLE_UUID_TYPE_128 }, LILUM_CHR_LIVE_BRIGHTNESS_BYTES };
static const ble_uuid128_t UUID_LIVE_PREVIEW    = { { BLE_UUID_TYPE_128 }, LILUM_CHR_LIVE_PREVIEW_BYTES    };
static const ble_uuid128_t UUID_LIVE_FLASHLIGHT = { { BLE_UUID_TYPE_128 }, LILUM_CHR_LIVE_FLASHLIGHT_BYTES };
static const ble_uuid128_t UUID_LIVE_APPLYEXIT  = { { BLE_UUID_TYPE_128 }, LILUM_CHR_LIVE_APPLY_EXIT_BYTES };

// Forward declarations
static void enter_config_adv_window(void);
static void exit_to_runtime(void);
static void enter_runtime_master(void);
static void enter_runtime_follower(void);
static void enter_runtime_idle(void);
static void start_config_window_adv(void);
static void stop_all_radio(void);
static void start_runtime_master_adv(void);
static void start_runtime_follower_scan(void);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static void on_host_sync(void);
static void on_host_reset(int reason);
static void host_task(void *param);
static void maintenance_task(void *arg);
static void runtime_refresh_task(void *arg);
static uint8_t compute_remaining_seconds(void);
static void apply_element_config_to_engine(const LilumElementConfig &cfg);

// External symbols owned by led_engine.cpp.
extern uint8_t g_animation_mode;
extern bool    g_manual_mode_active;
extern uint8_t g_manual_pattern;
extern uint8_t workingBrightness;

// ── Public status helpers ────────────────────────────────────────────────
void ble_set_logging(bool enabled)
{
    esp_log_level_set(TAG, enabled ? ESP_LOG_INFO : ESP_LOG_NONE);
}

bool ble_is_currently_advertising(void) { return s_advertising; }
bool ble_is_currently_scanning(void)    { return s_scanning;    }
bool ble_is_connected(void)             { return s_connected;   }
bool ble_in_config_window(void)
{
    return s_state == BleStateMachine::ConfigAdv ||
           s_state == BleStateMachine::ConfigConnected;
}
uint8_t ble_config_window_remaining_sec(void) { return compute_remaining_seconds(); }
bool ble_runtime_is_master(void)   { return s_state == BleStateMachine::RuntimeMaster;   }
bool ble_runtime_is_follower(void) { return s_state == BleStateMachine::RuntimeFollower; }
bool ble_is_follower_synced(void)  { return s_follower_synced; }
uint8_t ble_live_brightness_override(void) { return s_live_brightness_override; }

// ── Helpers ──────────────────────────────────────────────────────────────
static uint8_t compute_remaining_seconds(void)
{
    if (s_config_window_paused) {
        return s_config_window_paused_remaining_sec;
    }
    if (s_config_window_started_us == 0) {
        return 0;
    }
    int64_t elapsed_us = esp_timer_get_time() - s_config_window_started_us;
    int64_t elapsed_s  = elapsed_us / 1000000;
    if (elapsed_s >= LILUM_CONFIG_WINDOW_SEC) return 0;
    return static_cast<uint8_t>(LILUM_CONFIG_WINDOW_SEC - elapsed_s);
}

static void apply_element_config_to_engine(const LilumElementConfig &cfg)
{
    // Minimum viable mapping: brightness is the most user-visible knob and is
    // already a writable global in the LED engine. Reactive flags, button
    // gestures and playlists are persisted but not yet wired through.
    workingBrightness = cfg.startup_brightness;
    g_manual_mode_active = (cfg.startup_mode == LILUM_STARTUP_MANUAL);
    if (cfg.startup_mode == LILUM_STARTUP_AUTO && cfg.auto_playlist_len > 0) {
        g_animation_mode = 0;  // start at first entry; cycling owned by led_engine
    }
    if (cfg.startup_mode == LILUM_STARTUP_MANUAL && cfg.manual_playlist_len > 0) {
        g_manual_pattern = 0;
    }
}

// ── Maintenance task ─────────────────────────────────────────────────────
//
// Runs once per second while the config window is open: updates the
// "remaining seconds" field in the advertising payload (so the phone's
// countdown stays accurate) and, when no phone has connected, triggers the
// transition to the runtime state once the window expires.
static void maintenance_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_state == BleStateMachine::ConfigAdv) {
            uint8_t remaining = compute_remaining_seconds();
            if (remaining == 0) {
                ESP_LOGI(TAG, "Config window expired without a connection; entering runtime");
                exit_to_runtime();
                continue;
            }
            // The remaining-seconds countdown is now exposed via the FF15
            // GATT "config info" characteristic (read on demand by the app);
            // no need to rewrite advertising data every second.
        }
    }
}

// ── Runtime master adv refresh task ──────────────────────────────────────
static void runtime_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_state == BleStateMachine::RuntimeMaster) {
            start_runtime_master_adv();   // re-set data with fresh timestamp
        }
    }
}

// ── Advertising primitives ───────────────────────────────────────────────
static void stop_all_radio(void)
{
    // With CONFIG_BT_NIMBLE_EXT_ADV=y all advertising (both legacy ADV_IND
    // via legacy_pdu=1 and runtime extended adv) goes through the EXT_ADV
    // instance, so a single stop suffices.
    int rc = ble_gap_ext_adv_stop(kExtAdvInstance);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGD(TAG, "ext_adv_stop rc=%d", rc);
    }
    rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGD(TAG, "disc_cancel rc=%d", rc);
    }
    s_advertising = false;
    s_scanning    = false;
}

static void start_config_window_adv(void)
{
    // Legacy ADV_IND (31-byte connectable + scannable) so iOS CoreBluetooth
    // and stock BLE scanners (nRF Connect with default settings) can
    // discover the pendant. The full 41-byte LilumConfigAdvPayload is
    // exposed via the FF15 "config info" GATT characteristic after the
    // phone connects.
    //
    // We use the EXT_ADV API with legacy_pdu=1 because the NimBLE host is
    // built with CONFIG_BT_NIMBLE_EXT_ADV=y, which disables the legacy
    // ble_gap_adv_start() entry point. legacy_pdu=1 + connectable=1 +
    // scannable=1 produces standard ADV_IND PDUs on the wire.
    //
    // AD layout (≤ 31 bytes):
    //   Flags             : 3 bytes  (1 len + 1 type + 1 value)
    //   Complete UUID128  : 18 bytes (1 len + 1 type + 16 value)
    //   Shortened name    : 10 bytes (1 len + 1 type + 8 chars "Mandalum")
    //                       Total = 31 bytes.
    struct ble_hs_adv_fields fields = {};
    fields.flags        = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128     = const_cast<ble_uuid128_t *>(&UUID_SVC_PRIMARY);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    static const char adv_name[] = "Mandalum";
    fields.name = (uint8_t *)adv_name;
    fields.name_len = strlen(adv_name);
    fields.name_is_complete = 0;  // "Mandalum" is shorter than "Lilum Mandalum"

    if (!s_advertising) {
        ble_gap_ext_adv_stop(kExtAdvInstance);

        struct ble_gap_ext_adv_params params = {};
        params.connectable   = 1;
        params.scannable     = 1;   // legacy ADV_IND requires scannable
        params.legacy_pdu    = 1;   // produce legacy 31-byte PDUs
        params.own_addr_type = s_own_addr_type;
        params.primary_phy   = BLE_HCI_LE_PHY_1M;
        params.secondary_phy = BLE_HCI_LE_PHY_1M;
        params.tx_power      = 127;
        params.sid           = 0;
        params.itvl_min      = LILUM_ADV_ITVL_MIN_0_625;
        params.itvl_max      = LILUM_ADV_ITVL_MAX_0_625;
        params.channel_map   = 0x07;

        int8_t selected_tx_power = 0;
        int rc = ble_gap_ext_adv_configure(kExtAdvInstance, &params,
                                           &selected_tx_power,
                                           gap_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ext_adv_configure(legacy) rc=%d", rc);
            return;
        }
    }

    struct os_mbuf *data = os_msys_get_pkthdr(BLE_HCI_MAX_ADV_DATA_LEN, 0);
    if (!data) {
        ESP_LOGE(TAG, "os_msys_get_pkthdr failed");
        return;
    }
    int rc = ble_hs_adv_set_fields_mbuf(&fields, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_adv_set_fields_mbuf rc=%d", rc);
        os_mbuf_free_chain(data);
        return;
    }
    rc = ble_gap_ext_adv_set_data(kExtAdvInstance, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_set_data(legacy) rc=%d", rc);
        return;
    }

    if (!s_advertising) {
        rc = ble_gap_ext_adv_start(kExtAdvInstance, 0, 0);
        if (rc != 0) {
            ESP_LOGE(TAG, "ext_adv_start(legacy) rc=%d", rc);
            return;
        }
        s_advertising = true;
        ESP_LOGI(TAG, "Config-window advertising started (legacy ADV_IND, remaining=%u s)",
                 compute_remaining_seconds());
    }
}

static void start_runtime_master_adv(void)
{
    LilumRuntimeAdvPayload payload = {};
    payload.element_type_id_hash = LILUM_ELEMENT_TYPE_ID_HASH16;
    if (!lilum_uuid_is_zero(s_binding.group_id)) {
        payload.session_kind = 1;
        memcpy(payload.session_id, s_binding.group_id, 16);
    } else {
        payload.session_kind = 0;
        memcpy(payload.session_id, s_binding.pair_id, 16);
    }
    payload.sync_timestamp_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    payload.animation_index   = g_animation_mode;
    payload.brightness        = workingBrightness;

    uint8_t mfg_data[LILUM_ADV_RUNTIME_MFG_BYTES];
    size_t mfg_len = lilum_encode_runtime_adv(mfg_data, sizeof(mfg_data), payload);
    if (mfg_len != LILUM_ADV_RUNTIME_MFG_BYTES) {
        ESP_LOGE(TAG, "encode_runtime_adv failed");
        return;
    }

    struct ble_hs_adv_fields fields = {};
    fields.flags        = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128     = const_cast<ble_uuid128_t *>(&UUID_SVC_PRIMARY);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.mfg_data     = mfg_data;
    fields.mfg_data_len = static_cast<uint8_t>(mfg_len);

    static const char adv_name[] = "Lilum Mandalum";
    fields.name = (uint8_t *)adv_name;
    fields.name_len = strlen(adv_name);
    fields.name_is_complete = 1;

    if (!s_advertising) {
        ble_gap_ext_adv_stop(kExtAdvInstance);

        struct ble_gap_ext_adv_params params = {};
        params.connectable   = 0;
        params.scannable     = 0;
        params.legacy_pdu    = 0;
        params.own_addr_type = s_own_addr_type;
        params.primary_phy   = BLE_HCI_LE_PHY_1M;
        params.secondary_phy = BLE_HCI_LE_PHY_1M;
        params.sid           = 1;
        params.itvl_min      = LILUM_ADV_ITVL_MIN_0_625;
        params.itvl_max      = LILUM_ADV_ITVL_MAX_0_625;
        params.channel_map   = 0x07;

        int8_t selected = 0;
        int rc = ble_gap_ext_adv_configure(kExtAdvInstance, &params, &selected,
                                           gap_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "runtime ext_adv_configure rc=%d", rc);
            return;
        }
    }

    struct os_mbuf *data = os_msys_get_pkthdr(BLE_HCI_MAX_ADV_DATA_LEN, 0);
    if (!data) return;
    if (ble_hs_adv_set_fields_mbuf(&fields, data) != 0) {
        os_mbuf_free_chain(data);
        return;
    }
    int rc = ble_gap_ext_adv_set_data(kExtAdvInstance, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "runtime ext_adv_set_data rc=%d", rc);
        return;
    }
    if (!s_advertising) {
        rc = ble_gap_ext_adv_start(kExtAdvInstance, 0, 0);
        if (rc != 0) {
            ESP_LOGE(TAG, "runtime ext_adv_start rc=%d", rc);
            return;
        }
        s_advertising = true;
        ESP_LOGI(TAG, "Runtime master advertising started");
    }
}

static void start_runtime_follower_scan(void)
{
    if (s_scanning) return;

    struct ble_gap_disc_params dp = {};
    dp.passive           = 1;
    dp.itvl              = 0x0030;
    dp.window            = 0x0030;
    dp.filter_duplicates = 0;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event_cb, NULL);
    if (rc == 0) {
        s_scanning = true;
        ESP_LOGI(TAG, "Runtime follower scan started");
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "follower scan start failed: %d", rc);
    }
}

// ── State transitions ────────────────────────────────────────────────────
static void enter_config_adv_window(void)
{
    s_state = BleStateMachine::ConfigAdv;
    s_config_window_started_us = esp_timer_get_time();
    s_config_window_paused = false;
    s_config_window_paused_remaining_sec = 0;
    start_config_window_adv();
    if (!s_maintenance_task) {
        xTaskCreate(maintenance_task, "BLE Maint", 3072, NULL, 4, &s_maintenance_task);
    }
}

static void enter_runtime_master(void)
{
    ESP_LOGI(TAG, "→ Runtime MASTER");
    s_state = BleStateMachine::RuntimeMaster;
    stop_all_radio();
    start_runtime_master_adv();
    if (!s_runtime_refresh_task) {
        xTaskCreate(runtime_refresh_task, "BLE RtMaster", 3072, NULL, 4, &s_runtime_refresh_task);
    }
}

static void enter_runtime_follower(void)
{
    ESP_LOGI(TAG, "→ Runtime FOLLOWER");
    s_state = BleStateMachine::RuntimeFollower;
    s_follower_synced = false;
    stop_all_radio();
    start_runtime_follower_scan();
}

static void enter_runtime_idle(void)
{
    ESP_LOGI(TAG, "→ Runtime IDLE (standalone)");
    s_state = BleStateMachine::RuntimeIdle;
    stop_all_radio();
}

static void exit_to_runtime(void)
{
    // Decide our runtime duty from the current binding (PRD §5.5).
    const bool in_group = !lilum_uuid_is_zero(s_binding.group_id);
    const bool in_pair  = !lilum_uuid_is_zero(s_binding.pair_id);

    if (in_group) {
        if (s_binding.group_role == LILUM_GROUP_ROLE_MASTER) {
            enter_runtime_master();
        } else if (in_pair && s_binding.pair_role == LILUM_PAIR_ROLE_MASTER) {
            // Group-follower that is also pair-master: act as pair-master so
            // its pair-follower mirrors it locally. (Group-master, if it
            // comes online, will eventually snap-to-master per PRD §5.6.)
            enter_runtime_master();
        } else {
            enter_runtime_follower();
        }
    } else if (in_pair) {
        if (s_binding.pair_role == LILUM_PAIR_ROLE_MASTER) {
            enter_runtime_master();
        } else {
            enter_runtime_follower();
        }
    } else {
        enter_runtime_idle();
    }
}

// ── GATT access callbacks ────────────────────────────────────────────────
static int read_str(struct ble_gatt_access_ctxt *ctxt, const char *s)
{
    return os_mbuf_append(ctxt->om, s, strlen(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static int read_bytes(struct ble_gatt_access_ctxt *ctxt, const void *p, size_t n)
{
    return os_mbuf_append(ctxt->om, p, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ident_type_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return read_str(ctxt, LILUM_ELEMENT_TYPE_ID);
}
static int ident_fwver_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t v[3] = { LILUM_FW_VERSION_MAJOR, LILUM_FW_VERSION_MINOR, LILUM_FW_VERSION_PATCH };
    return read_bytes(ctxt, v, sizeof(v));
}
static int ident_hwrev_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return read_str(ctxt, LILUM_HARDWARE_REVISION);
}
static int ident_serial_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return read_str(ctxt, LILUM_SERIAL_NUMBER);
}
// Returns the full 41-byte LilumConfigAdvPayload (company id + kind tag +
// protocol §3.1 body). Replaces the same payload that previously appeared
// as the manufacturer-specific data field of the BLE 5 extended config-
// window advertisement. Only meaningful while the configuration window is
// open; outside of it the body is still well-formed but the
// `config_window_remaining_sec` field reads 0.
static int ident_cfginfo_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

    LilumConfigAdvPayload payload = {};
    payload.element_type_id_hash        = LILUM_ELEMENT_TYPE_ID_HASH16;
    memcpy(payload.pair_id,  s_binding.pair_id,  16);
    memcpy(payload.group_id, s_binding.group_id, 16);
    payload.firmware_major              = LILUM_FW_VERSION_MAJOR;
    payload.firmware_minor              = LILUM_FW_VERSION_MINOR;
    payload.firmware_patch              = LILUM_FW_VERSION_PATCH;
    payload.config_window_remaining_sec = compute_remaining_seconds();

    uint8_t buf[LILUM_ADV_CONFIG_MFG_BYTES];
    size_t  len = lilum_encode_config_adv(buf, sizeof(buf), payload);
    if (len != LILUM_ADV_CONFIG_MFG_BYTES) return BLE_ATT_ERR_UNLIKELY;
    return read_bytes(ctxt, buf, len);
}

// Binding service — generic getter/setter for the small fields.
static int bind_pair_id_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return read_bytes(ctxt, s_binding.pair_id, 16);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 16) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint16_t out = 0;
        ble_hs_mbuf_to_flat(ctxt->om, s_binding.pair_id, 16, &out);
        if (lilum_uuid_is_zero(s_binding.pair_id)) s_binding.pair_role = LILUM_PAIR_ROLE_NONE;
        lilum_nvs_save_binding(&s_binding);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int bind_pair_role_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return read_bytes(ctxt, &s_binding.pair_role, 1);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t v = 0; uint16_t out = 0;
        ble_hs_mbuf_to_flat(ctxt->om, &v, 1, &out);
        if (v > LILUM_PAIR_ROLE_FOLLOWER) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        s_binding.pair_role = v;
        lilum_nvs_save_binding(&s_binding);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int bind_group_id_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return read_bytes(ctxt, s_binding.group_id, 16);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 16) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint16_t out = 0;
        ble_hs_mbuf_to_flat(ctxt->om, s_binding.group_id, 16, &out);
        lilum_nvs_save_binding(&s_binding);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int bind_group_role_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return read_bytes(ctxt, &s_binding.group_role, 1);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t v = 0; uint16_t out = 0;
        ble_hs_mbuf_to_flat(ctxt->om, &v, 1, &out);
        if (v > LILUM_GROUP_ROLE_MASTER) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        s_binding.group_role = v;
        lilum_nvs_save_binding(&s_binding);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int bind_slot_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return read_bytes(ctxt, &s_binding.group_slot_index, 1);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (OS_MBUF_PKTLEN(ctxt->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        uint8_t v = 0; uint16_t out = 0;
        ble_hs_mbuf_to_flat(ctxt->om, &v, 1, &out);
        if (v > 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        s_binding.group_slot_index = v;
        lilum_nvs_save_binding(&s_binding);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// Configuration service.
static int cfg_blob_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    if (total < 14 || total > LILUM_CONFIG_BLOB_MAX_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t flat[LILUM_CONFIG_BLOB_MAX_LEN];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, flat, sizeof(flat), &out_len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    LilumElementConfig tmp;
    size_t consumed = lilum_config_decode(flat, out_len, &tmp);
    if (consumed == 0) {
        ESP_LOGW(TAG, "config blob rejected (len=%u)", out_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    s_element_cfg = tmp;
    lilum_nvs_save_element_config(&s_element_cfg);
    apply_element_config_to_engine(s_element_cfg);
    ESP_LOGI(TAG, "ElementConfig applied (auto=%u, manual=%u entries)",
             s_element_cfg.auto_playlist_len, s_element_cfg.manual_playlist_len);
    return 0;
}

// Live Command service.
static int live_brightness_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (OS_MBUF_PKTLEN(ctxt->om) != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t v = 0; uint16_t out = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &v, 1, &out);
    if (v > 100) v = 100;
    s_live_brightness_override = v;
    workingBrightness = v;
    return 0;
}
static int live_preview_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (OS_MBUF_PKTLEN(ctxt->om) != LILUM_ANIM_TUPLE_BYTES) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    LilumAnimationTuple t;
    uint16_t out = 0;
    ble_hs_mbuf_to_flat(ctxt->om, &t, sizeof(t), &out);
    // For now: just log the preview request; full animation engine wiring is a
    // separate piece of work.
    ESP_LOGI(TAG, "previewAnimation base=%u sym=%u p={%u,%u,%u,%u,%u,%u}",
             t.base, t.symmetry, t.params[0], t.params[1], t.params[2],
             t.params[3], t.params[4], t.params[5]);
    g_animation_mode = t.base;
    return 0;
}
static int live_flashlight_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (OS_MBUF_PKTLEN(ctxt->om) != 5) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t buf[5]; uint16_t out = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out);
    led_engine_set_white_mode(true);
    workingBrightness = buf[4] > 100 ? 100 : buf[4];
    return 0;
}
static int live_applyexit_cb(uint16_t, uint16_t, struct ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    ESP_LOGI(TAG, "applyConfigAndExitWindow received");
    // Persist any pending state and disconnect; gap_event_cb's disconnect path
    // will pick up exit_to_runtime().
    lilum_nvs_save_binding(&s_binding);
    lilum_nvs_save_element_config(&s_element_cfg);
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        exit_to_runtime();
    }
    return 0;
}

// ── GATT services table ──────────────────────────────────────────────────
static const struct ble_gatt_svc_def gatt_svcs[] = {
    // Identity
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_IDENTITY.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_ID_TYPE.u,   .access_cb = ident_type_cb,   .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &UUID_ID_FWVER.u,  .access_cb = ident_fwver_cb,  .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &UUID_ID_HWREV.u,  .access_cb = ident_hwrev_cb,  .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &UUID_ID_SERIAL.u, .access_cb = ident_serial_cb, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &UUID_ID_CFGINFO.u,.access_cb = ident_cfginfo_cb,.flags = BLE_GATT_CHR_F_READ },
            { 0 }
        },
    },
    // Binding
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_BINDING.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_BIND_PAIR_ID.u,    .access_cb = bind_pair_id_cb,    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_BIND_PAIR_ROLE.u,  .access_cb = bind_pair_role_cb,  .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_BIND_GROUP_ID.u,   .access_cb = bind_group_id_cb,   .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_BIND_GROUP_ROLE.u, .access_cb = bind_group_role_cb, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_BIND_SLOT.u,       .access_cb = bind_slot_cb,       .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
            { 0 }
        },
    },
    // Configuration
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_CONFIG.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_CFG_STANDALONE.u, .access_cb = cfg_blob_cb, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_CFG_SHARED.u,     .access_cb = cfg_blob_cb, .flags = BLE_GATT_CHR_F_WRITE },
            { 0 }
        },
    },
    // Live Command
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC_LIVE.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_LIVE_BRIGHTNESS.u, .access_cb = live_brightness_cb, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &UUID_LIVE_PREVIEW.u,    .access_cb = live_preview_cb,    .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &UUID_LIVE_FLASHLIGHT.u, .access_cb = live_flashlight_cb, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &UUID_LIVE_APPLYEXIT.u,  .access_cb = live_applyexit_cb,  .flags = BLE_GATT_CHR_F_WRITE },
            { 0 }
        },
    },
    { 0 }
};

// ── GAP events ───────────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
                if (s_state == BleStateMachine::ConfigAdv) start_config_window_adv();
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_advertising = false;
            s_config_window_paused_remaining_sec = compute_remaining_seconds();
            s_config_window_paused = true;
            s_state = BleStateMachine::ConfigConnected;
            ESP_LOGI(TAG, "Phone connected; config window paused at %u s",
                     s_config_window_paused_remaining_sec);
            // Negotiate high connection priority + larger MTU per protocol §8.
            struct ble_gap_upd_params p = {};
            p.itvl_min = 12;   // 15 ms
            p.itvl_max = 12;
            p.latency  = 0;
            p.supervision_timeout = 400;  // 4 s
            ble_gap_update_params(s_conn_handle, &p);
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
            return 0;
        }

        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGI(TAG, "Phone disconnected; reason=%d", event->disconnect.reason);
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            // Per PRD §4.4 Option C: a disconnect ends the session
            // regardless of cause. Transition to runtime duty.
            exit_to_runtime();
            return 0;
        }

        case BLE_GAP_EVENT_ADV_COMPLETE:
            s_advertising = false;
            return 0;

        case BLE_GAP_EVENT_DISC: {
            // Runtime follower path: parse manufacturer data and apply.
            if (s_state != BleStateMachine::RuntimeFollower) return 0;
            struct ble_hs_adv_fields f = {};
            if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0) {
                return 0;
            }
            if (!f.mfg_data || f.mfg_data_len < LILUM_ADV_RUNTIME_MFG_BYTES) return 0;
            if (lilum_adv_kind(f.mfg_data, f.mfg_data_len) != LILUM_ADV_KIND_RUNTIME) return 0;
            LilumRuntimeAdvPayload p;
            if (!lilum_decode_runtime_adv(f.mfg_data, f.mfg_data_len, &p)) return 0;
            if (p.element_type_id_hash != LILUM_ELEMENT_TYPE_ID_HASH16) return 0;

            // Only follow our own group / pair.
            const bool in_group = !lilum_uuid_is_zero(s_binding.group_id);
            const uint8_t *session = in_group ? s_binding.group_id : s_binding.pair_id;
            const uint8_t expected_kind = in_group ? 1 : 0;
            if (p.session_kind != expected_kind) return 0;
            if (memcmp(p.session_id, session, 16) != 0) return 0;

            s_follower_synced = true;
            if (g_animation_mode != p.animation_index) {
                g_animation_mode = p.animation_index;
            }
            if (workingBrightness != p.brightness && p.brightness > 0) {
                workingBrightness = p.brightness;
            }
            return 0;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
            if (s_state == BleStateMachine::RuntimeFollower) {
                start_runtime_follower_scan();
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU negotiated: %u", event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

// ── Host bring-up ────────────────────────────────────────────────────────
static void on_host_sync(void)
{
    ESP_LOGI(TAG, "host synchronized");
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE addr %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    enter_config_adv_window();
}

static void on_host_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset; reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    if (s_host_stopped_sem) xSemaphoreGive(s_host_stopped_sem);
    nimble_port_freertos_deinit();
}

// ── Public init ──────────────────────────────────────────────────────────
void ble_init(void)
{
    if (s_initialized) return;

    // Pull persisted state before any radio activity.
    lilum_nvs_load_binding(&s_binding);
    lilum_nvs_load_element_config(&s_element_cfg);
    apply_element_config_to_engine(s_element_cfg);

    if (!s_host_stopped_sem) {
        s_host_stopped_sem = xSemaphoreCreateBinary();
    }

    esp_nimble_hci_and_controller_init();
    nimble_port_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Lilum Mandalum");

    ble_hs_cfg.reset_cb = on_host_reset;
    ble_hs_cfg.sync_cb  = on_host_sync;

    int rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    nimble_port_freertos_init(host_task);
    s_initialized = true;
}
