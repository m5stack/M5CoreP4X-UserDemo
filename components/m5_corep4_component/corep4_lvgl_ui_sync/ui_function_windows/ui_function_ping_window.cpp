#include "ui_function_widgets.h"

#include "ping/ping_sock.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <cstdio>
#include <cstring>

namespace {

using namespace corep4_function_ui;

constexpr const char* TAG = "UI_FUNC_PING";
constexpr uint32_t PING_COUNT = 4;
constexpr uint32_t PING_TIMEOUT_MS = 1000;
constexpr uint32_t PING_INTERVAL_MS = 500;
constexpr uint32_t NET_REFRESH_MS = 500;

struct PingPageState {
    // Ethernet status items
    lv_obj_t* eth_status_dot = nullptr;
    lv_obj_t* eth_status_label = nullptr;
    lv_obj_t* eth_ip_label = nullptr;

    // Header action button
    lv_obj_t* header_ping_button = nullptr;

    // Targets & Results
    struct TargetRow {
        lv_obj_t* row_obj = nullptr;
        lv_obj_t* status_dot = nullptr;
        lv_obj_t* info_label = nullptr;
        lv_obj_t* result_label = nullptr;
        lv_obj_t* checkbox = nullptr;

        char host[64] = {};
        char label_name[32] = {};
        bool is_gateway = false;
    };

    TargetRow targets[4];

    // Summary line
    lv_obj_t* summary_label = nullptr;

    lv_timer_t* net_timer = nullptr;
    volatile bool page_alive = false;
    volatile bool task_running = false;
};

struct PingResult {
    esp_err_t status = ESP_OK;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration_ms = 0;
    uint32_t min_ms = UINT32_MAX;
    uint32_t max_ms = 0;
    uint64_t rtt_sum_ms = 0;
    char resolved_ip[32] = "--";
    char error[96] = {};
};

struct PingTaskContext {
    char target[64] = {};
    char targets[4][64] = {};
    uint8_t target_indices[4] = {};
    uint8_t target_count = 0;
    uint8_t current_ui_idx = 0;
    uint32_t count = PING_COUNT;
    SemaphoreHandle_t done_sem = nullptr;
    esp_ping_handle_t ping = nullptr;
    PingResult result;
};

PingPageState s_ping_page;

lv_obj_t* create_status_dot(lv_obj_t* parent, uint32_t color)
{
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(dot, 0, LV_PART_MAIN);
    return dot;
}

void refresh_header_ping_button_state()
{
    bool any_checked = false;
    for (int i = 0; i < 4; ++i) {
        if (s_ping_page.targets[i].checkbox && lv_obj_has_state(s_ping_page.targets[i].checkbox, LV_STATE_CHECKED)) {
            any_checked = true;
            break;
        }
    }
    set_button_enabled(s_ping_page.header_ping_button, any_checked && !s_ping_page.task_running);
}

void set_ping_ui_interactive(bool interactive)
{
    for (int i = 0; i < 4; ++i) {
        if (s_ping_page.targets[i].checkbox) {
            set_button_enabled(s_ping_page.targets[i].checkbox, interactive);
        }
        if (s_ping_page.targets[i].row_obj) {
            if (interactive) {
                lv_obj_add_flag(s_ping_page.targets[i].row_obj, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_remove_flag(s_ping_page.targets[i].row_obj, LV_OBJ_FLAG_CLICKABLE);
            }
        }
    }
    refresh_header_ping_button_state();
}

void refresh_network_labels()
{
    auto* tool = corep4_ui_sync_get_tool();
    if (!tool) {
        return;
    }

    auto* status = tool->get_device_status();
    if (!status) {
        return;
    }

    const bool eth_got_ip_live = tool->is_ethernet_connected();
    char live_ip[16] = "--";
    char live_gw[16] = "--";
    esp_netif_ip_info_t ip_info = {};
    if (eth_got_ip_live && tool->get_ethernet_ip_info(&ip_info) == ESP_OK) {
        std::snprintf(live_ip, sizeof(live_ip), IPSTR, IP2STR(&ip_info.ip));
        std::snprintf(live_gw, sizeof(live_gw), IPSTR, IP2STR(&ip_info.gw));
        std::snprintf(status->ethernet.ip_address, sizeof(status->ethernet.ip_address), "%.15s", live_ip);
        std::snprintf(status->ethernet.gateway, sizeof(status->ethernet.gateway), "%.15s", live_gw);
        status->ethernet.got_ip = true;
    }

    // Ethernet Status update
    if (s_ping_page.eth_status_dot && s_ping_page.eth_status_label && s_ping_page.eth_ip_label) {
        bool eth_ok = eth_got_ip_live || status->ethernet.got_ip;
        bool eth_link = status->ethernet.ethernet_connected;

        uint32_t eth_color = eth_ok ? COLOR_SUCCESS : (eth_link ? COLOR_WARNING : COLOR_MUTED);
        const char* eth_text = eth_ok ? "Got IP" : (eth_link ? "Link Up" : "Disconnected");

        lv_obj_set_style_bg_color(s_ping_page.eth_status_dot, lv_color_hex(eth_color), LV_PART_MAIN);
        set_label_text(s_ping_page.eth_status_label, eth_text);

        char detail_buf[128];
        std::snprintf(detail_buf, sizeof(detail_buf), "IP: %s   |   GW: %s",
                      eth_ok ? status->ethernet.ip_address : "--",
                      eth_ok ? status->ethernet.gateway : "--");
        set_label_text(s_ping_page.eth_ip_label, detail_buf);
    }
}

bool has_network_ready()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    return status && (tool->is_ethernet_connected() || status->ethernet.got_ip || status->wifi.wifi_connected);
}

bool resolve_ping_target(const char* target, ip_addr_t* out_addr, char* out_ip, size_t out_ip_len, char* err, size_t err_len)
{
    if (!target || !out_addr || !out_ip || out_ip_len == 0) {
        if (err && err_len) {
            std::snprintf(err, err_len, "invalid target");
        }
        return false;
    }

    std::memset(out_addr, 0, sizeof(*out_addr));

    struct in_addr direct_addr = {};
    if (inet_pton(AF_INET, target, &direct_addr) == 1) {
        out_addr->type = IPADDR_TYPE_V4;
        out_addr->u_addr.ip4.addr = direct_addr.s_addr;
        inet_ntop(AF_INET, &direct_addr, out_ip, out_ip_len);
        return true;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int gai_ret = getaddrinfo(target, nullptr, &hints, &res);
    if (gai_ret != 0 || !res) {
        if (err && err_len) {
            std::snprintf(err, err_len, "DNS failed: %d", gai_ret);
        }
        return false;
    }

    auto* ipv4 = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    out_addr->type = IPADDR_TYPE_V4;
    out_addr->u_addr.ip4.addr = ipv4->sin_addr.s_addr;
    inet_ntop(AF_INET, &ipv4->sin_addr, out_ip, out_ip_len);
    freeaddrinfo(res);
    return true;
}

void update_ui_target_start(uint8_t ui_idx)
{
    if (!lvgl_port_lock(200)) {
        return;
    }
    if (s_ping_page.page_alive) {
        auto& t = s_ping_page.targets[ui_idx];
        if (t.status_dot) {
            lv_obj_set_style_bg_color(t.status_dot, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
        }
        if (t.result_label) {
            set_label_text(t.result_label, "Testing...");
            lv_obj_set_style_text_color(t.result_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
        }

        char buf[128];
        std::snprintf(buf, sizeof(buf), "Testing: %s...", t.label_name);
        set_label_text(s_ping_page.summary_label, buf);
        if (s_ping_page.summary_label) {
            lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
        }
    }
    lvgl_port_unlock();
}

void update_ui_target_done(uint8_t ui_idx, bool ok, uint32_t avg_rtt, const char* error)
{
    if (!lvgl_port_lock(200)) {
        return;
    }
    if (s_ping_page.page_alive) {
        auto& t = s_ping_page.targets[ui_idx];
        if (t.status_dot) {
            lv_obj_set_style_bg_color(t.status_dot, lv_color_hex(ok ? COLOR_SUCCESS : COLOR_ERROR), LV_PART_MAIN);
        }
        if (t.result_label) {
            char res_buf[96];
            if (ok) {
                std::snprintf(res_buf, sizeof(res_buf), "%lu ms", static_cast<unsigned long>(avg_rtt));
                lv_obj_set_style_text_color(t.result_label, lv_color_hex(COLOR_SUCCESS), LV_PART_MAIN);
            } else {
                std::snprintf(res_buf, sizeof(res_buf), "%s", (error && error[0]) ? error : "Failed");
                lv_obj_set_style_text_color(t.result_label, lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
            }
            set_label_text(t.result_label, res_buf);
        }
    }
    lvgl_port_unlock();
}

void update_ui_ping_complete(uint32_t ok_count, uint32_t total_count)
{
    if (!lvgl_port_lock(200)) {
        return;
    }
    if (s_ping_page.page_alive) {
        s_ping_page.task_running = false;
        set_ping_ui_interactive(true);
        refresh_network_labels();

        char buf[128];
        if (ok_count == total_count) {
            std::snprintf(buf, sizeof(buf), "Test completed: All %lu targets succeeded!", (unsigned long)ok_count);
            set_label_text(s_ping_page.summary_label, buf);
            if (s_ping_page.summary_label) {
                lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_SUCCESS), LV_PART_MAIN);
            }
        } else {
            std::snprintf(buf, sizeof(buf), "Test completed: %lu/%lu targets succeeded.", (unsigned long)ok_count, (unsigned long)total_count);
            set_label_text(s_ping_page.summary_label, buf);
            if (s_ping_page.summary_label) {
                lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
            }
        }
    }
    lvgl_port_unlock();
}

uint32_t ping_attempt_display_index(uint32_t seqno, uint32_t count)
{
    if (seqno == 0) {
        return 1;
    }
    return seqno > count ? count : seqno;
}

void ping_success_cb(esp_ping_handle_t hdl, void* args)
{
    auto* ctx = static_cast<PingTaskContext*>(args);
    if (!ctx) {
        return;
    }

    uint32_t elapsed_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    ctx->result.received++;
    ctx->result.rtt_sum_ms += elapsed_ms;
    if (elapsed_ms < ctx->result.min_ms) {
        ctx->result.min_ms = elapsed_ms;
    }
    if (elapsed_ms > ctx->result.max_ms) {
        ctx->result.max_ms = elapsed_ms;
    }

    uint32_t seqno = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));

    if (lvgl_port_lock(100)) {
        if (s_ping_page.page_alive) {
            auto& t = s_ping_page.targets[ctx->current_ui_idx];
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Testing (%lu/%lu)...",
                          static_cast<unsigned long>(ping_attempt_display_index(seqno, ctx->count)),
                          static_cast<unsigned long>(ctx->count));
            set_label_text(t.result_label, buf);
        }
        lvgl_port_unlock();
    }
}

void ping_timeout_cb(esp_ping_handle_t hdl, void* args)
{
    (void)hdl;
    auto* ctx = static_cast<PingTaskContext*>(args);
    if (!ctx) {
        return;
    }

    uint32_t seqno = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));

    if (lvgl_port_lock(100)) {
        if (s_ping_page.page_alive) {
            auto& t = s_ping_page.targets[ctx->current_ui_idx];
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Timeout (%lu/%lu)",
                          static_cast<unsigned long>(ping_attempt_display_index(seqno, ctx->count)),
                          static_cast<unsigned long>(ctx->count));
            set_label_text(t.result_label, buf);
        }
        lvgl_port_unlock();
    }
}

void ping_end_cb(esp_ping_handle_t hdl, void* args)
{
    auto* ctx = static_cast<PingTaskContext*>(args);
    if (!ctx) {
        return;
    }

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST,
                         &ctx->result.transmitted,
                         sizeof(ctx->result.transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,
                         &ctx->result.received,
                         sizeof(ctx->result.received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION,
                         &ctx->result.duration_ms,
                         sizeof(ctx->result.duration_ms));

    if (ctx->done_sem) {
        xSemaphoreGive(ctx->done_sem);
    }
}

void run_ping_once(PingTaskContext* ctx)
{
    if (!ctx) {
        return;
    }

    ctx->result = {};
    ctx->result.min_ms = UINT32_MAX;
    ctx->ping = nullptr;
    ctx->done_sem = nullptr;

    ip_addr_t target_addr;
    char resolve_error[64] = {};
    if (!resolve_ping_target(ctx->target,
                             &target_addr,
                             ctx->result.resolved_ip,
                             sizeof(ctx->result.resolved_ip),
                             resolve_error,
                             sizeof(resolve_error))) {
        ctx->result.status = ESP_ERR_NOT_FOUND;
        std::snprintf(ctx->result.error, sizeof(ctx->result.error), "%s", resolve_error);
        return;
    }

    ctx->done_sem = xSemaphoreCreateBinary();
    if (!ctx->done_sem) {
        ctx->result.status = ESP_ERR_NO_MEM;
        std::snprintf(ctx->result.error, sizeof(ctx->result.error), "semaphore alloc failed");
        return;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = ctx->count;
    ping_config.timeout_ms = PING_TIMEOUT_MS;
    ping_config.interval_ms = PING_INTERVAL_MS;
    ping_config.data_size = 32;

    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = ping_success_cb;
    callbacks.on_ping_timeout = ping_timeout_cb;
    callbacks.on_ping_end = ping_end_cb;
    callbacks.cb_args = ctx;

    ctx->result.status = esp_ping_new_session(&ping_config, &callbacks, &ctx->ping);
    if (ctx->result.status == ESP_OK) {
        ctx->result.status = esp_ping_start(ctx->ping);
        if (ctx->result.status == ESP_OK) {
            const TickType_t wait_ticks = pdMS_TO_TICKS(ctx->count * (PING_TIMEOUT_MS + PING_INTERVAL_MS) + 3000);
            if (xSemaphoreTake(ctx->done_sem, wait_ticks) != pdTRUE) {
                ctx->result.status = ESP_ERR_TIMEOUT;
                std::snprintf(ctx->result.error, sizeof(ctx->result.error), "ping session timeout");
                esp_ping_stop(ctx->ping);
                xSemaphoreTake(ctx->done_sem, pdMS_TO_TICKS(1000));
            }
        } else {
            std::snprintf(ctx->result.error, sizeof(ctx->result.error),
                          "start failed: %s", esp_err_to_name(ctx->result.status));
        }
    } else {
        std::snprintf(ctx->result.error, sizeof(ctx->result.error),
                      "new session failed: %s", esp_err_to_name(ctx->result.status));
    }

    if (ctx->ping) {
        esp_ping_delete_session(ctx->ping);
        ctx->ping = nullptr;
    }
    vSemaphoreDelete(ctx->done_sem);
    ctx->done_sem = nullptr;
}

void ping_task(void* param)
{
    auto* ctx = static_cast<PingTaskContext*>(param);
    if (!ctx) {
        s_ping_page.task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    uint32_t ok_count = 0;
    for (uint8_t i = 0; i < ctx->target_count; ++i) {
        uint8_t ui_idx = ctx->target_indices[i];
        ctx->current_ui_idx = ui_idx;
        std::snprintf(ctx->target, sizeof(ctx->target), "%s", ctx->targets[i]);

        update_ui_target_start(ui_idx);

        run_ping_once(ctx);

        bool ok = (ctx->result.status == ESP_OK && ctx->result.received > 0);
        uint32_t avg_rtt = ctx->result.received ? static_cast<uint32_t>(ctx->result.rtt_sum_ms / ctx->result.received) : 0;

        if (ok) {
            ok_count++;
        }

        update_ui_target_done(ui_idx, ok, avg_rtt, ctx->result.error);
    }

    update_ui_ping_complete(ok_count, ctx->target_count);

    delete ctx;
    vTaskDelete(nullptr);
}

void set_target_to_gateway()
{
    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (status && status->ethernet.got_ip && status->ethernet.gateway[0] != '\0') {
        std::snprintf(s_ping_page.targets[3].host, sizeof(s_ping_page.targets[3].host), "%s", status->ethernet.gateway);
        return;
    }
    std::snprintf(s_ping_page.targets[3].host, sizeof(s_ping_page.targets[3].host), "192.168.1.1");
}

void start_ping()
{
    if (s_ping_page.task_running) {
        return;
    }

    refresh_network_labels();
    if (!has_network_ready()) {
        set_label_text(s_ping_page.summary_label, "Network not ready. Connect ETH or WiFi first.");
        if (s_ping_page.summary_label) {
            lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
        }
        return;
    }

    uint8_t checked_count = 0;
    for (int i = 0; i < 4; ++i) {
        if (s_ping_page.targets[i].checkbox && lv_obj_has_state(s_ping_page.targets[i].checkbox, LV_STATE_CHECKED)) {
            checked_count++;
        }
    }

    if (checked_count == 0) {
        set_label_text(s_ping_page.summary_label, "Select at least one target.");
        if (s_ping_page.summary_label) {
            lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
        }
        return;
    }

    for (int i = 0; i < 4; ++i) {
        auto& t = s_ping_page.targets[i];
        if (t.checkbox && lv_obj_has_state(t.checkbox, LV_STATE_CHECKED)) {
            lv_obj_set_style_bg_color(t.status_dot, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
            set_label_text(t.result_label, "Waiting...");
            lv_obj_set_style_text_color(t.result_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(t.status_dot, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
            set_label_text(t.result_label, "--");
            lv_obj_set_style_text_color(t.result_label, lv_color_hex(COLOR_MUTED), LV_PART_MAIN);
        }
    }

    auto* ctx = new PingTaskContext();
    ctx->target_count = 0;

    if (s_ping_page.targets[0].checkbox && lv_obj_has_state(s_ping_page.targets[0].checkbox, LV_STATE_CHECKED)) {
        std::snprintf(ctx->targets[ctx->target_count], sizeof(ctx->targets[0]), "%s", s_ping_page.targets[0].host);
        ctx->target_indices[ctx->target_count] = 0;
        ctx->target_count++;
    }
    if (s_ping_page.targets[1].checkbox && lv_obj_has_state(s_ping_page.targets[1].checkbox, LV_STATE_CHECKED)) {
        std::snprintf(ctx->targets[ctx->target_count], sizeof(ctx->targets[0]), "%s", s_ping_page.targets[1].host);
        ctx->target_indices[ctx->target_count] = 1;
        ctx->target_count++;
    }
    if (s_ping_page.targets[2].checkbox && lv_obj_has_state(s_ping_page.targets[2].checkbox, LV_STATE_CHECKED)) {
        std::snprintf(ctx->targets[ctx->target_count], sizeof(ctx->targets[0]), "%s", s_ping_page.targets[2].host);
        ctx->target_indices[ctx->target_count] = 2;
        ctx->target_count++;
    }
    if (s_ping_page.targets[3].checkbox && lv_obj_has_state(s_ping_page.targets[3].checkbox, LV_STATE_CHECKED)) {
        set_target_to_gateway();
        std::snprintf(ctx->targets[ctx->target_count], sizeof(ctx->targets[0]), "%s", s_ping_page.targets[3].host);
        ctx->target_indices[ctx->target_count] = 3;
        ctx->target_count++;
    }

    std::snprintf(ctx->target, sizeof(ctx->target), "%.63s", ctx->targets[0]);
    ctx->count = PING_COUNT;

    s_ping_page.task_running = true;
    set_ping_ui_interactive(false);

    char running_buf[128];
    std::snprintf(running_buf, sizeof(running_buf), "Starting test on %d target(s)...", checked_count);
    set_label_text(s_ping_page.summary_label, running_buf);
    if (s_ping_page.summary_label) {
        lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_WARNING), LV_PART_MAIN);
    }

    BaseType_t task_ret = xTaskCreate(ping_task,
                                      "func_ping",
                                      6144,
                                      ctx,
                                      configMAX_PRIORITIES - 4,
                                      nullptr);
    if (task_ret != pdPASS) {
        delete ctx;
        s_ping_page.task_running = false;
        set_ping_ui_interactive(true);
        set_label_text(s_ping_page.summary_label, "Ping failed: cannot create worker task");
        if (s_ping_page.summary_label) {
            lv_obj_set_style_text_color(s_ping_page.summary_label, lv_color_hex(COLOR_ERROR), LV_PART_MAIN);
        }
    }
}

void target_row_click_cb(lv_event_t* e)
{
    if (s_ping_page.task_running) {
        return;
    }
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_obj_t* clicked_obj = static_cast<lv_obj_t*>(lv_event_get_target(e));

    for (int i = 0; i < 4; ++i) {
        if (s_ping_page.targets[i].row_obj == row) {
            lv_obj_t* cb = s_ping_page.targets[i].checkbox;
            if (cb && clicked_obj != cb) {
                if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
                    lv_obj_remove_state(cb, LV_STATE_CHECKED);
                } else {
                    lv_obj_add_state(cb, LV_STATE_CHECKED);
                }
                refresh_header_ping_button_state();
            }
            break;
        }
    }
}

void checkbox_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        refresh_header_ping_button_state();
    }
}

void ping_button_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        s_ping_page.header_ping_button = static_cast<lv_obj_t*>(lv_event_get_target(e));
        start_ping();
    }
}

void net_timer_cb(lv_timer_t* /*timer*/)
{
    if (!s_ping_page.page_alive) {
        return;
    }
    refresh_network_labels();

    auto* tool = corep4_ui_sync_get_tool();
    auto* status = tool ? tool->get_device_status() : nullptr;
    if (status && status->ethernet.got_ip && status->ethernet.gateway[0] != '\0') {
        if (std::strcmp(s_ping_page.targets[3].host, status->ethernet.gateway) != 0) {
            std::snprintf(s_ping_page.targets[3].host, sizeof(s_ping_page.targets[3].host), "%s", status->ethernet.gateway);
            char buf[80];
            std::snprintf(buf, sizeof(buf), "Gateway (%s)", status->ethernet.gateway);
            set_label_text(s_ping_page.targets[3].info_label, buf);
        }
    } else {
        if (std::strcmp(s_ping_page.targets[3].host, "192.168.1.1") != 0) {
            std::snprintf(s_ping_page.targets[3].host, sizeof(s_ping_page.targets[3].host), "192.168.1.1");
            set_label_text(s_ping_page.targets[3].info_label, "Gateway (Not Connected)");
        }
    }

    set_ping_ui_interactive(!s_ping_page.task_running);
}

void ping_content_delete_event_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }

    s_ping_page.page_alive = false;
    if (s_ping_page.net_timer) {
        lv_timer_delete(s_ping_page.net_timer);
        s_ping_page.net_timer = nullptr;
    }
    s_ping_page.eth_status_dot = nullptr;
    s_ping_page.eth_status_label = nullptr;
    s_ping_page.eth_ip_label = nullptr;
    s_ping_page.header_ping_button = nullptr;
    s_ping_page.summary_label = nullptr;

    for (int i = 0; i < 4; ++i) {
        s_ping_page.targets[i] = {};
    }
}

void build_content(lv_obj_t* parent, const CoreP4FunctionWindowConfig& config)
{
    (void)config;

    prepare_content(parent);
    s_ping_page.page_alive = true;
    lv_obj_add_event_cb(parent, ping_content_delete_event_cb, LV_EVENT_DELETE, nullptr);

    // CARD 1: Network Status Card
    lv_obj_t* net_card = add_card(parent, 90);
    add_label(net_card, "Ethernet Status", COLOR_TEXT);

    lv_obj_t* eth_status_row = add_row(net_card, 22);
    s_ping_page.eth_status_dot = create_status_dot(eth_status_row, COLOR_MUTED);
    s_ping_page.eth_status_label = add_label(eth_status_row, "Ethernet: --", COLOR_TEXT, 150);

    s_ping_page.eth_ip_label = add_label(net_card, "IP: --   |   GW: --", COLOR_MUTED);

    // CARD 2: Targets and Results Card
    lv_obj_t* targets_card = add_card(parent, 246);
    add_label(targets_card, "Ping Check Targets & Results", COLOR_TEXT);

    struct TargetDef {
        const char* name;
        const char* default_host;
        bool is_gw;
    } target_defs[4] = {
        {"Google DNS", "8.8.8.8", false},
        {"Cloudflare DNS", "1.1.1.1", false},
        {"M5Stack Site", "m5stack.com", false},
        {"Gateway", "192.168.1.1", true}
    };

    for (int i = 0; i < 4; ++i) {
        auto& t = s_ping_page.targets[i];
        std::snprintf(t.host, sizeof(t.host), "%s", target_defs[i].default_host);
        std::snprintf(t.label_name, sizeof(t.label_name), "%s", target_defs[i].name);
        t.is_gateway = target_defs[i].is_gw;

        t.row_obj = add_row(targets_card, 40);
        lv_obj_add_flag(t.row_obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t.row_obj, target_row_click_cb, LV_EVENT_CLICKED, nullptr);

        t.status_dot = create_status_dot(t.row_obj, COLOR_MUTED);

        char info_buf[64];
        if (t.is_gateway) {
            std::snprintf(info_buf, sizeof(info_buf), "%s (--)", t.label_name);
        } else {
            std::snprintf(info_buf, sizeof(info_buf), "%s (%s)", t.label_name, t.host);
        }
        t.info_label = add_label(t.row_obj, info_buf, COLOR_TEXT, 200);

        t.result_label = add_label(t.row_obj, "--", COLOR_MUTED, 140);
        lv_obj_set_style_text_align(t.result_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

        t.checkbox = lv_checkbox_create(t.row_obj);
        lv_checkbox_set_text(t.checkbox, "");
        lv_obj_set_width(t.checkbox, 40);

        // Google & Cloudflare checked by default
        if (i < 2) {
            lv_obj_add_state(t.checkbox, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(t.checkbox, checkbox_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    lv_obj_t* summary_card = add_card(parent, 52);
    lv_obj_set_style_pad_all(summary_card, 8, LV_PART_MAIN);
    s_ping_page.summary_label =
        add_label(summary_card,
                  "Select targets and tap Ping to check network connectivity.",
                  COLOR_MUTED);
    lv_label_set_long_mode(s_ping_page.summary_label, LV_LABEL_LONG_DOT);

    refresh_network_labels();
    refresh_header_ping_button_state();

    s_ping_page.net_timer = lv_timer_create(net_timer_cb, NET_REFRESH_MS, nullptr);
}

constexpr CoreP4FunctionWindowConfig WINDOW_CONFIG = {
    "Ping",
    "click on ui_func1ping1con",
    build_content,
    "Ping",
    ping_button_event_cb,
};

} // namespace

extern "C" void func2ping2clicked2func(lv_event_t* e)
{
    corep4_ui_open_function_window(e, WINDOW_CONFIG);
}
