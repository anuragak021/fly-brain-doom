/**
 * @file bl_client.c
 * @brief Brain link task. See bl_client.h.
 */
#include "bl_client.h"

#include <stddef.h>
#include <string.h>

#include "brain_protocol.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_DOOM_BRAIN_LEARN
#define LEARN_ENABLED (true)
#else
#define LEARN_ENABLED (false)
#endif
#ifdef CONFIG_DOOM_BRAIN_RESET_WEIGHTS_ON_BOOT
#define RESET_WEIGHTS_ON_BOOT (true)
#else
#define RESET_WEIGHTS_ON_BOOT (false)
#endif

static const char *TAG = "brain";

_Static_assert(BL_N_FEATURES == BRAIN_N_FEATURES, "feature vector must match the protocol");

#define LINK_UART (UART_NUM_1)
#define RX_BUFFER_BYTES (256)
#define TASK_STACK_BYTES (4096U)
#define TASK_PRIORITY (5U)
#define LINK_CORE (1) /* the engine runs on core 0 */
#define STEP_TIMEOUT_MS (100U)
#define PING_TIMEOUT_MS (200U)
#define PING_RETRY_MS (500U)
#define FEATURES_MAX_AGE_US (500000LL) /* no new 3D frame for this long: stop stepping */
#define REPLY_MAX_AGE_US (250000LL)    /* an older reply is not acted on */
#define MISSES_BEFORE_REPING (5U)
#define LOG_PERIOD_STEPS (100U)
#define US_PER_MS (1000LL)
#define IDLE_POLL_MS (5U)

/* Shared between the engine task and the link task; every access is inside the spinlock. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bl_features_t s_features;
static int64_t s_features_us; /* 0 = none yet */
static uint8_t s_actions;
static float s_steer_left;
static float s_steer_right;
static float s_drift;
static int64_t s_reply_us; /* 0 = no reply yet */
static float s_damage_left;
static float s_damage_right;
static float s_reward_acc; /* summed by the engine, taken (and zeroed) by the next STEP */
static bool s_learn_ok;

static brain_parser_t s_parser;
static uint8_t s_seq;
static uint32_t s_core_khz = 1U;
static bool s_reset_weights_pending = RESET_WEIGHTS_ON_BOOT;

typedef struct {
    uint32_t steps, misses, learn_steps;
    float reward;
    uint64_t rtt_us, cycles;
    float drift;
    int64_t window_start_us;
} stats_t;
static stats_t s_stats;

/** Send one request and wait for the reply with the same SEQ. */
static bool transact(uint8_t type, const uint8_t *payload, uint8_t len, uint32_t timeout_ms,
                     brain_frame_t *resp)
{
    uint8_t frame[BRAIN_MAX_FRAME_BYTES];
    size_t frame_len = 0U;
    const uint8_t seq = ++s_seq;
    if (brain_frame_encode(type, seq, payload, len, frame, sizeof(frame), &frame_len) != BRAIN_OK) {
        return false;
    }
    (void)uart_flush_input(LINK_UART);
    brain_parser_reset(&s_parser);
    if (uart_write_bytes(LINK_UART, frame, frame_len) < 0) {
        return false;
    }
    const int64_t deadline = esp_timer_get_time() + ((int64_t)timeout_ms * US_PER_MS);
    for (;;) {
        const int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return false;
        }
        uint8_t byte;
        const TickType_t ticks = pdMS_TO_TICKS((uint32_t)((left_us + US_PER_MS - 1) / US_PER_MS));
        if (uart_read_bytes(LINK_UART, &byte, 1U, (ticks > 0U) ? ticks : 1U) != 1) {
            continue;
        }
        bool got = false;
        if (brain_parser_push(&s_parser, byte, resp, &got) == BRAIN_OK && got && resp->seq == seq) {
            return true;
        }
    }
}

static bool ping(void)
{
    brain_frame_t r;
    if (!transact(BRAIN_MSG_PING, NULL, 0U, PING_TIMEOUT_MS, &r) || (r.type != BRAIN_MSG_INFO) ||
        (r.len != BRAIN_INFO_PAYLOAD_BYTES) ||
        (r.payload[BRAIN_INFO_OFF_PROTO_VERSION] != BRAIN_PROTOCOL_VERSION) ||
        (r.payload[BRAIN_INFO_OFF_N_FEATURES] != BRAIN_N_FEATURES)) {
        return false;
    }
    const uint8_t *p = r.payload;
    const uint32_t neurons =
        (uint32_t)p[BRAIN_INFO_OFF_N_NEURONS] | ((uint32_t)p[BRAIN_INFO_OFF_N_NEURONS + 1U] << 8);
    const uint32_t learnable = (uint32_t)p[BRAIN_INFO_OFF_N_LEARNABLE] |
                               ((uint32_t)p[BRAIN_INFO_OFF_N_LEARNABLE + 1U] << 8);
    s_core_khz = (uint32_t)p[BRAIN_INFO_OFF_CORE_CLOCK_KHZ] |
                 ((uint32_t)p[BRAIN_INFO_OFF_CORE_CLOCK_KHZ + 1U] << 8) |
                 ((uint32_t)p[BRAIN_INFO_OFF_CORE_CLOCK_KHZ + 2U] << 16) |
                 ((uint32_t)p[BRAIN_INFO_OFF_CORE_CLOCK_KHZ + 3U] << 24);
    if (s_core_khz == 0U) {
        s_core_khz = 1U;
    }
    ESP_LOGI(TAG,
             "brain online: protocol v%u, %u neurons, %u substeps/step, core %u kHz, %u learning "
             "synapses",
             (unsigned)p[BRAIN_INFO_OFF_PROTO_VERSION], (unsigned)neurons,
             (unsigned)p[BRAIN_INFO_OFF_SUBSTEPS], (unsigned)s_core_khz, (unsigned)learnable);
    return true;
}

static void log_stats_if_due(void)
{
    if (s_stats.steps < LOG_PERIOD_STEPS) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    const uint32_t rtt_us = (uint32_t)(s_stats.rtt_us / s_stats.steps);
    const uint32_t board_us =
        (uint32_t)((s_stats.cycles * 1000ULL) / ((uint64_t)s_stats.steps * s_core_khz));
    const uint32_t window_ms = (uint32_t)((now - s_stats.window_start_us) / US_PER_MS);
    ESP_LOGI(TAG,
             "%u steps in %u ms | round trip %u us | board step %u us | misses %u | learning "
             "%u steps, reward %+.2f, weight drift %.4f",
             (unsigned)s_stats.steps, (unsigned)window_ms, (unsigned)rtt_us, (unsigned)board_us,
             (unsigned)s_stats.misses, (unsigned)s_stats.learn_steps, (double)s_stats.reward,
             (double)s_stats.drift);
    s_stats = (stats_t){0};
    s_stats.window_start_us = now;
}

/** One STEP with the latest features; false on a miss. */
static bool step_once(const bl_features_t *f)
{
    uint8_t payload[BRAIN_STEP_PAYLOAD_BYTES];
    brain_frame_t r;
    brain_step_resp_t out;
    brain_step_req_t req = {.reward = 0.0F, .flags = 0U};
    (void)memcpy(req.features, f->feature, sizeof(req.features));
    bool learn_ok;
    portENTER_CRITICAL(&s_lock);
    req.features[BL_FEAT_DAMAGE_LEFT] = s_damage_left;
    req.features[BL_FEAT_DAMAGE_RIGHT] = s_damage_right;
    req.reward = s_reward_acc; /* taken now: a STEP that then times out loses it (the board
                                  may have applied it, so it is not re-sent) */
    s_reward_acc = 0.0F;
    learn_ok = s_learn_ok;
    portEXIT_CRITICAL(&s_lock);
    req.reward = (req.reward > 1.0F) ? 1.0F : ((req.reward < -1.0F) ? -1.0F : req.reward);
    const bool learning = LEARN_ENABLED && learn_ok;
    if (learning) {
        req.flags |= (uint8_t)BRAIN_STEP_FLAG_LEARN;
    }
    if (s_reset_weights_pending) {
        req.flags |= (uint8_t)BRAIN_STEP_FLAG_RESET_WEIGHTS;
    }
    if (brain_step_request_encode(&req, payload, sizeof(payload)) != BRAIN_OK) {
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    if (!transact(BRAIN_MSG_STEP, payload, (uint8_t)sizeof(payload), STEP_TIMEOUT_MS, &r) ||
        (r.type != BRAIN_MSG_STEP_RESP) ||
        (brain_step_resp_decode(r.payload, r.len, &out) != BRAIN_OK)) {
        return false;
    }
    const int64_t t1 = esp_timer_get_time();
    if (s_reset_weights_pending) {
        s_reset_weights_pending = false;
        ESP_LOGI(TAG, "brain weights reset to the connectome (boot option)");
    }
    portENTER_CRITICAL(&s_lock);
    s_actions = out.actions;
    s_steer_left = out.steer_left;
    s_steer_right = out.steer_right;
    s_drift = out.weight_drift;
    s_reply_us = t1;
    portEXIT_CRITICAL(&s_lock);
    s_stats.steps++;
    s_stats.rtt_us += (uint64_t)(t1 - t0);
    s_stats.cycles += out.step_cycles;
    s_stats.drift = out.weight_drift;
    s_stats.learn_steps += learning ? 1U : 0U;
    s_stats.reward += learning ? req.reward : 0.0F;
    return true;
}

static void link_task(void *arg)
{
    (void)arg;
    s_stats.window_start_us = esp_timer_get_time();
    bool online = false;
    uint32_t consecutive_misses = 0U;
    for (;;) {
        if (!online) {
            online = ping();
            consecutive_misses = 0U;
            if (!online) {
                vTaskDelay(pdMS_TO_TICKS(PING_RETRY_MS));
            }
            continue;
        }
        bl_features_t f;
        int64_t age_us;
        portENTER_CRITICAL(&s_lock);
        f = s_features;
        age_us = (s_features_us == 0) ? (FEATURES_MAX_AGE_US + 1)
                                      : (esp_timer_get_time() - s_features_us);
        portEXIT_CRITICAL(&s_lock);
        if (age_us > FEATURES_MAX_AGE_US) {
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
            continue;
        }
        if (step_once(&f)) {
            consecutive_misses = 0U;
        } else {
            s_stats.misses++;
            consecutive_misses++;
            if (consecutive_misses >= MISSES_BEFORE_REPING) {
                ESP_LOGW(TAG, "%u misses in a row; pinging again", (unsigned)consecutive_misses);
                online = false;
            }
        }
        log_stats_if_due();
    }
}

bl_status_t bl_client_start(const bl_cfg_t *cfg)
{
    if (cfg == NULL) {
        return BL_ERR_NULL;
    }
    const uart_config_t uc = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if ((uart_driver_install(LINK_UART, RX_BUFFER_BYTES, 0, 0, NULL, 0) != ESP_OK) ||
        (uart_param_config(LINK_UART, &uc) != ESP_OK) ||
        (uart_set_pin(LINK_UART, cfg->tx, cfg->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
         ESP_OK)) {
        ESP_LOGE(TAG, "UART%d setup failed", (int)LINK_UART);
        return BL_ERR_SIZE;
    }
    if (xTaskCreatePinnedToCore(link_task, "brain_link", TASK_STACK_BYTES, NULL, TASK_PRIORITY,
                                NULL, LINK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "link task not created");
        return BL_ERR_SIZE;
    }
    ESP_LOGI(TAG, "link on UART%d: TX GPIO%d RX GPIO%d at %u baud", (int)LINK_UART, (int)cfg->tx,
             (int)cfg->rx, (unsigned)cfg->baud);
    return BL_OK;
}

void bl_client_submit(const bl_features_t *features)
{
    if (features == NULL) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_features = *features;
    s_features_us = now;
    portEXIT_CRITICAL(&s_lock);
}

void bl_client_game(float damage_left, float damage_right, float reward, bool learn_ok)
{
    portENTER_CRITICAL(&s_lock);
    s_damage_left = damage_left;
    s_damage_right = damage_right;
    s_reward_acc += reward;
    s_learn_ok = learn_ok;
    portEXIT_CRITICAL(&s_lock);
}

void bl_client_snapshot(bl_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    out->actions = s_actions;
    out->view = s_features;
    out->steer_left = s_steer_left;
    out->steer_right = s_steer_right;
    out->weight_drift = s_drift;
    const int64_t reply_us = s_reply_us;
    portEXIT_CRITICAL(&s_lock);
    const int64_t age_us = (reply_us == 0) ? (REPLY_MAX_AGE_US + 1) : (now - reply_us);
    out->brain_ok = (age_us <= REPLY_MAX_AGE_US);
    out->reply_age_ms = (age_us < 0) ? 0U : (uint32_t)(age_us / US_PER_MS);
}
