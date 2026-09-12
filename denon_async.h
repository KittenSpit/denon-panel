#pragma once
// Denon Zone2 status polling + fire-and-forget command dispatch, run on a
// dedicated FreeRTOS task PINNED TO CPU1. ESPHome's main loop (LVGL, touch,
// wifi, api, everything else) runs on CPU0 - confirmed from this project's
// own build sdkconfig (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y) - so this task
// runs in genuine parallel with zero contention. Neither the periodic status
// poll nor a button-triggered command ever blocks touch/display again.
//
// Talks to the receiver with raw esp_http_client calls (not ESPHome's
// http_request component, which is built to run *on* the main loop).
//
// YAML usage:
//   on_boot (low prio): denon_async_init("192.168.1.91");
//   button on_click:    denon_async_send_command("/goform/formiPhoneAppDirect.xml?Z2UP");
//   fast interval:      DenonStatus st; if (denon_async_poll_status(&st)) { ...update LVGL... }
//   on_idle / wake:     denon_async_set_screen_dimmed(true/false);
//   mute switch tap:    denon_async_note_local_mute_action(new_state);
//   on wake:            if (denon_async_was_paused()) { ...blank labels... }

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct DenonStatus {
  bool valid = false;
  bool power_on = false;
  bool mute_on = false;
  float volume_db = -50.0f;
  char input[16] = "";
};

namespace denon_async_internal {

inline std::string xml_extract(const std::string &body, const std::string &tag) {
  std::string open_tag = "<" + tag + "><value>";
  std::string close_tag = "</value></" + tag + ">";
  auto p1 = body.find(open_tag);
  if (p1 == std::string::npos)
    return "";
  p1 += open_tag.size();
  auto p2 = body.find(close_tag, p1);
  if (p2 == std::string::npos)
    return "";
  std::string val = body.substr(p1, p2 - p1);
  while (!val.empty() && val.back() == ' ')
    val.pop_back();
  while (!val.empty() && val.front() == ' ')
    val.erase(val.begin());
  return val;
}

struct CmdMsg {
  char path[160];
};

inline char g_ip[40] = "";
inline QueueHandle_t g_cmd_queue = nullptr;
inline SemaphoreHandle_t g_mutex = nullptr;
inline DenonStatus g_status;
inline bool g_status_dirty = false;
inline uint32_t g_last_mute_action_ms = 0;
inline std::atomic<bool> g_screen_dimmed{false};
inline std::atomic<bool> g_was_paused{false};

// Only ever touched from the denon task itself (single-threaded within that
// task), so no locking needed for this.
inline char g_resp_buf[2048];

// The Denon's goform HTTP server (GoAhead-Webs) answers HTTP/1.0 with no
// Content-Length and no Transfer-Encoding header at all - it just writes the
// body and closes the connection (confirmed via raw curl -v). That's a
// completely ordinary "read until EOF" body, but esp_http_client_perform()'s
// automatic body-reading mishandles exactly this case and logs the
// misleading "Incomplete chunked data received -1" on every single request
// (this is NOT actually a chunked response - the receiver never sends that
// header). ESPHome's own http_request component sidesteps this by never
// calling perform() - it does the manual open()/fetch_headers()/read() loop
// below instead, which handles close-terminated bodies correctly. Mirroring
// that here fixes it (confirmed live).
inline int http_do_get(const char *url, char *out_buf, int out_buf_size) {
  esp_http_client_config_t config = {};
  config.url = url;
  config.timeout_ms = 3000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr)
    return -1;

  int result_len = -1;
  if (esp_http_client_open(client, 0) == ESP_OK) {
    esp_http_client_fetch_headers(client);  // returns 0 here (no Content-Length) - ignored, we read to EOF
    int status_code = esp_http_client_get_status_code(client);
    int total = 0;
    while (out_buf != nullptr && total < out_buf_size - 1) {
      int n = esp_http_client_read(client, out_buf + total, out_buf_size - 1 - total);
      if (n <= 0)
        break;  // 0 = connection closed/done, <0 = error - either way, stop
      total += n;
    }
    result_len = (status_code == 200) ? total : -1;
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return result_len;
}

// Fire a GET, discard the body - used for commands (fire and forget).
inline void http_get_discard(const char *url) {
  http_do_get(url, nullptr, 0);
}

// Fire a GET, capture + parse the Zone2 status XML into the mailbox.
inline void http_get_status() {
  char url[96];
  snprintf(url, sizeof(url), "http://%s/goform/formZone2_Zone2XmlStatus.xml", g_ip);

  int len = http_do_get(url, g_resp_buf, sizeof(g_resp_buf));
  if (len <= 0)
    return;
  g_resp_buf[len] = '\0';
  std::string body(g_resp_buf, len);

  std::string power = xml_extract(body, "Power");
  std::string input = xml_extract(body, "InputFuncSelect");
  std::string vol = xml_extract(body, "MasterVolume");
  std::string mute = xml_extract(body, "Mute");
  if (power.empty())
    return;  // malformed/short read - skip this cycle

  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_status.valid = true;
  g_status.power_on = (power == "ON");
  strncpy(g_status.input, input.c_str(), sizeof(g_status.input) - 1);
  g_status.input[sizeof(g_status.input) - 1] = '\0';
  if (!vol.empty())
    g_status.volume_db = atof(vol.c_str());
  // Same 3s grace period as before: don't trust <Mute> right after our own
  // local tap - the receiver needs a moment to actually update it, and an
  // unlucky poll landing in that window was flashing the display back to
  // the old state before the real change caught up (confirmed live).
  if (millis() - g_last_mute_action_ms > 3000) {
    g_status.mute_on = (mute == "on");
  }
  g_status_dirty = true;
  xSemaphoreGive(g_mutex);
}

inline void task_fn(void *param) {
  CmdMsg cmd;
  TickType_t last_poll = xTaskGetTickCount();
  uint32_t pause_candidate_ms = 0;
  const TickType_t poll_period = pdMS_TO_TICKS(2000);

  while (true) {
    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - last_poll;
    TickType_t remaining = (elapsed < poll_period) ? (poll_period - elapsed) : 0;

    if (xQueueReceive(g_cmd_queue, &cmd, remaining) == pdTRUE) {
      char url[200];
      snprintf(url, sizeof(url), "http://%s%s", g_ip, cmd.path);
      http_get_discard(url);
      continue;  // re-check timing; don't reset last_poll for a command
    }

    // Timed out waiting for a command -> it's time to (maybe) poll status.
    bool zone2_off;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    zone2_off = !g_status.power_on;
    xSemaphoreGive(g_mutex);

    bool pause_candidate = g_screen_dimmed.load() && zone2_off;
    bool paused = false;
    if (!pause_candidate) {
      pause_candidate_ms = 0;
    } else {
      if (pause_candidate_ms == 0)
        pause_candidate_ms = millis();
      if (millis() - pause_candidate_ms >= 30000)
        paused = true;
    }
    g_was_paused.store(paused);

    if (!paused)
      http_get_status();
    last_poll = xTaskGetTickCount();
  }
}

}  // namespace denon_async_internal

inline void denon_async_init(const char *ip) {
  using namespace denon_async_internal;
  strncpy(g_ip, ip, sizeof(g_ip) - 1);
  g_mutex = xSemaphoreCreateMutex();
  g_cmd_queue = xQueueCreate(8, sizeof(CmdMsg));
  xTaskCreatePinnedToCore(task_fn, "denon_async", 8192, nullptr, 5, nullptr, 1);
}

// path must start with "/", e.g. "/goform/formiPhoneAppDirect.xml?Z2UP"
inline void denon_async_send_command(const char *path) {
  using namespace denon_async_internal;
  CmdMsg cmd{};
  strncpy(cmd.path, path, sizeof(cmd.path) - 1);
  if (g_cmd_queue != nullptr)
    xQueueSend(g_cmd_queue, &cmd, 0);
}

// Call the instant a local tap changes the mute switch, with the state it
// was just set to. Writes that value straight into the shared status struct
// (not just starting the grace-period timer) so that a background poll
// landing mid-grace - which deliberately leaves mute_on untouched, see
// http_get_status() - can't push a *different* (older) value back out to
// LVGL just because g_status_dirty is true for some other field. Confirmed
// live this was the actual cause of mute flashing back to its old state
// shortly after a tap: the poll's own mute skip was working correctly, but
// the mailbox still held the pre-tap value underneath it.
inline void denon_async_note_local_mute_action(bool new_state) {
  using namespace denon_async_internal;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  g_last_mute_action_ms = millis();
  g_status.mute_on = new_state;
  g_status_dirty = true;
  xSemaphoreGive(g_mutex);
}

inline void denon_async_set_screen_dimmed(bool dimmed) {
  denon_async_internal::g_screen_dimmed.store(dimmed);
}

inline bool denon_async_was_paused() {
  return denon_async_internal::g_was_paused.load();
}

// Returns true (and fills *out) exactly once per new poll result; false if
// nothing new since the last call. Cheap - safe to call often (e.g. 250ms).
inline bool denon_async_poll_status(DenonStatus *out) {
  using namespace denon_async_internal;
  bool have_new = false;
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  if (g_status_dirty) {
    *out = g_status;
    g_status_dirty = false;
    have_new = true;
  }
  xSemaphoreGive(g_mutex);
  return have_new;
}
