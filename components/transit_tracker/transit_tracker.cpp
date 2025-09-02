#include "transit_tracker.h"
#include "string_utils.h"

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/components/network/util.h"

extern "C" {
  #include "esp_system.h"
  #include "esp_heap_caps.h"
}

namespace memstats {

inline void log_memory_stats(const char *tag = "mem") {
    ESP_LOGD("mem", "Total heap: %d", ESP.getHeapSize());
    ESP_LOGD("mem", "Free heap: %d", ESP.getFreeHeap());
    ESP_LOGD("mem", "Total PSRAM: %d", ESP.getPsramSize());
    ESP_LOGD("mem", "Free PSRAM: %d", ESP.getFreePsram());
}

} // namespace memstats

namespace esphome {
namespace transit_tracker {

static const char *TAG = "transit_tracker.component";

void TransitTracker::setup() {
  this->ws_client_.onMessage([this](websockets::WebsocketsMessage message) {
    this->on_ws_message_(message);
  });

  this->ws_client_.onEvent([this](websockets::WebsocketsEvent event, String data) {
    this->on_ws_event_(event, data);
  });

  this->connect_ws_();

  this->set_interval("check_stale_trips", 10000, [this]() {
    if (this->ws_client_.available() && !this->schedule_state_.trips.empty()) {
      bool has_stale_trips = false;

      this->schedule_state_.mutex.lock();

      auto now = this->rtc_->now();
      if (now.is_valid()) {
        for (auto &trip : this->schedule_state_.trips) {
          if (now.timestamp - trip.departure_time > 60) {
            has_stale_trips = true;
            break;
          }
        }
      }

      this->schedule_state_.mutex.unlock();

      if (has_stale_trips) {
        ESP_LOGD(TAG, "Stale trips detected, reconnecting");
        ESP_LOGD(TAG, "  Current RTC time: %d", now.timestamp);
        ESP_LOGD(TAG, "  Last heartbeat: %d", this->last_heartbeat_);
        this->reconnect();
      }
    }
  });
}

void TransitTracker::loop() {
  this->ws_client_.poll();

  if (this->last_heartbeat_ != 0 && millis() - this->last_heartbeat_ > 60000) {
    ESP_LOGW(TAG, "Heartbeat timeout, reconnecting");
    this->reconnect();
    return;
  }
}

void TransitTracker::dump_config() {
  ESP_LOGCONFIG(TAG, "Transit Tracker:");
  ESP_LOGCONFIG(TAG, "  Base URL: %s", this->base_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Schedule: %s", this->schedule_string_.c_str());
  ESP_LOGCONFIG(TAG, "  Limit: %d", this->limit_);
  ESP_LOGCONFIG(TAG, "  List mode: %s", this->list_mode_.c_str());
  ESP_LOGCONFIG(TAG, "  Display departure times: %s", this->display_departure_times_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Unit display: %s", this->unit_display_ == UNIT_DISPLAY_LONG ? "long" : this->unit_display_ == UNIT_DISPLAY_SHORT ? "short" : "none");
  memstats::log_memory_stats();
}

void TransitTracker::reconnect() {
  this->close();
  this->connect_ws_();
}

void TransitTracker::close(bool fully) {
  if (fully) {
    this->fully_closed_ = true;
  }

  this->ws_client_.close();
}

void TransitTracker::on_shutdown() {
  this->cancel_interval("check_stale_trips");
  this->close(true);
}

void TransitTracker::on_ws_message_(websockets::WebsocketsMessage message) {
  ESP_LOGV(TAG, "Received message: %s", message.rawData().c_str());
    
  // Tune to your payload ceiling (bytes). Keep headroom for parsing overhead.
   constexpr size_t JSON_CAP = 48 * 1024;
  
  // --- Allocate the JSON document in PSRAM so the internal pool lives there ---
  void* doc_mem = heap_caps_malloc(sizeof(StaticJsonDocument<JSON_CAP>),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!doc_mem) {
    this->status_set_error("No PSRAM for JSON doc");
    return;
  }
  auto* doc = new (doc_mem) StaticJsonDocument<JSON_CAP>();

  auto cleanup_doc = [&](){
    doc->~StaticJsonDocument();
    heap_caps_free(doc_mem);
  };

  DeserializationError err = deserializeJson(*doc, message.rawData());
  if (err) {
    cleanup_doc();
    this->status_set_error("Failed to parse schedule data");
    return;
  }

  JsonObject root = doc->as<JsonObject>();
  const char* event = root["event"] | "";

  if (strcmp(event, "heartbeat") == 0) {
    ESP_LOGD(TAG, "Received heartbeat");
    this->last_heartbeat_ = millis();
    cleanup_doc();
    return;
  }

  if (strcmp(event, "schedule") != 0) {
    this->status_set_error("Failed to parse schedule data");
    cleanup_doc();
    return;
  }

  ESP_LOGD(TAG, "Received schedule update");

  this->schedule_state_.mutex.lock();
  this->schedule_state_.trips.clear();

  JsonObject data = root["data"];
  JsonArray trips = data["trips"].as<JsonArray>();

  for (JsonObject trip : trips) {
    std::string headsign = trip["headsign"].as<const char*>();

    for (const auto &abbr : this->abbreviations_) {
      size_t pos = headsign.find(abbr.first);
      if (pos != std::string::npos) {
        ESP_LOGV(TAG, "Applying abbreviation '%s' -> '%s' in headsign",
                 abbr.first.c_str(), abbr.second.c_str());
        headsign.replace(pos, abbr.first.length(), abbr.second);
      }
    }

    std::string stop_id   = trip["stopId"].as<const char*>();
    std::string route_id  = trip["routeId"].as<const char*>();
    std::string route_name = trip["routeName"].as<const char*>();

    Color route_color = this->default_route_color_;

    auto route_style = this->route_styles_.find(route_id);
    if (route_style != this->route_styles_.end()) {
      route_color = route_style->second.color;
      route_name  = route_style->second.name;
    } else if (!trip["routeColor"].isNull()) {
      route_color = Color(std::stoul(trip["routeColor"].as<const char*>(), nullptr, 16));
    }

    this->schedule_state_.trips.push_back({
      .stop_id        = std::move(stop_id),
      .route_id       = std::move(route_id),
      .route_name     = std::move(route_name),
      .route_color    = route_color,
      .headsign       = std::move(headsign),
      .arrival_time   = trip["arrivalTime"].as<time_t>(),
      .departure_time = trip["departureTime"].as<time_t>(),
      .is_realtime    = trip["isRealtime"].as<bool>(),
    });
  }

  this->schedule_state_.mutex.unlock();

  cleanup_doc();
}

void TransitTracker::on_ws_event_(websockets::WebsocketsEvent event, String data) {
  if (event == websockets::WebsocketsEvent::ConnectionOpened) {
    ESP_LOGD(TAG, "WebSocket connection opened");

    constexpr size_t JSON_CAP = 4 * 1024; // Adjust based on max outbound size

    // --- Allocate StaticJsonDocument in PSRAM ---
    void* doc_mem = heap_caps_malloc(sizeof(StaticJsonDocument<JSON_CAP>),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!doc_mem) {
      ESP_LOGE(TAG, "Failed to allocate PSRAM for outbound JSON");
      return;
    }

    auto* doc = new (doc_mem) StaticJsonDocument<JSON_CAP>();

    // Helper to free PSRAM safely
    auto cleanup_doc = [&]() {
      doc->~StaticJsonDocument();
      heap_caps_free(doc_mem);
    };

    // --- Build JSON ---
    JsonObject root = doc->to<JsonObject>();
    root["event"] = "schedule:subscribe";

    JsonObject data = root.createNestedObject("data");

    if (!this->feed_code_.empty()) {
      data["feedCode"] = this->feed_code_;
    }

    data["routeStopPairs"]     = this->schedule_string_;
    data["limit"]              = this->limit_;
    data["sortByDeparture"]    = this->display_departure_times_;
    data["listMode"]           = this->list_mode_;

    // --- Serialize directly from PSRAM ---
    std::string message;
    serializeJson(*doc, message);

    ESP_LOGV(TAG, "Sending message: %s", message.c_str());
    this->ws_client_.send(message.c_str());

    // --- Free PSRAM ---
    cleanup_doc();
  } else if (event == websockets::WebsocketsEvent::ConnectionClosed) {
    ESP_LOGD(TAG, "WebSocket connection closed");
    if (!this->fully_closed_ && this->connection_attempts_ == 0) {
      this->defer([this]() {
        this->connect_ws_();
      });
    }
  } else if (event == websockets::WebsocketsEvent::GotPing) {
    ESP_LOGV(TAG, "Received ping");
  } else if (event == websockets::WebsocketsEvent::GotPong) {
    ESP_LOGV(TAG, "Received pong");
  }
}

void TransitTracker::connect_ws_() {
  if (this->base_url_.empty()) {
    ESP_LOGW(TAG, "No base URL set, not connecting");
    return;
  }

  if (this->fully_closed_) {
    ESP_LOGW(TAG, "Connection fully closed, not reconnecting");
    return;
  }

  if (this->ws_client_.available(true)) {
    ESP_LOGV(TAG, "Not reconnecting, already connected");
    return;
  }

  watchdog::WatchdogManager wdm(20000);

  this->last_heartbeat_ = 0;

  ESP_LOGD(TAG, "Connecting to WebSocket server (attempt %d): %s", this->connection_attempts_, this->base_url_.c_str());

  bool connection_success = false;
  if (esphome::network::is_connected()) {
    connection_success = this->ws_client_.connect(this->base_url_.c_str());
  } else {
    ESP_LOGW(TAG, "Not connected to network; skipping connection attempt");
  }

  if (!connection_success) {
    this->connection_attempts_++;

    if (this->connection_attempts_ >= 3) {
      this->status_set_error("Failed to connect to WebSocket server");
    }

    if (this->connection_attempts_ >= 15) {
      ESP_LOGE(TAG, "Could not connect to WebSocket server within 15 attempts.");
      ESP_LOGE(TAG, "It's likely that the network is not truly connected; rebooting the device to try to recover.");
      App.reboot();
    }

    auto timeout = std::min(15000, this->connection_attempts_ * 5000);
    ESP_LOGW(TAG, "Failed to connect, retrying in %ds", timeout / 1000);

    this->set_timeout("reconnect", timeout, [this]() {
      this->connect_ws_();
    });
  } else {
    this->has_ever_connected_ = true;
    this->connection_attempts_ = 0;
    this->status_clear_error();
  }
}

void TransitTracker::set_abbreviations_from_text(const std::string &text) {
  this->abbreviations_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 2) {
      ESP_LOGW(TAG, "Invalid abbreviation line: %s", line.c_str());
      continue;
    }
    this->add_abbreviation(parts[0], parts[1]);
  }
}

void TransitTracker::set_route_styles_from_text(const std::string &text) {
  this->route_styles_.clear();
  for (const auto &line : split(text, '\n')) {
    auto parts = split(line, ';');
    if (parts.size() != 3) {
      ESP_LOGW(TAG, "Invalid route style line: %s", line.c_str());
      continue;
    }
    uint32_t color = std::stoul(parts[2], nullptr, 16);
    this->add_route_style(parts[0], parts[1], Color(color));
  }
}

void TransitTracker::draw_text_centered_(const char *text, Color color) {
  int display_center_x = this->display_->get_width() / 2;
  int display_center_y = this->display_->get_height() / 2;
  this->display_->print(display_center_x, display_center_y, this->font_, color, display::TextAlign::CENTER, text);
}

std::string TransitTracker::from_now_(time_t unix_timestamp) const {
  if (this->rtc_ == nullptr) {
    return "";
  }

  uint now = this->rtc_->now().timestamp;

  int diff = unix_timestamp - now;

  if (diff < 30) {
    return "Now";
  }

  if (diff < 60) {
    switch (this->unit_display_) {
      case UNIT_DISPLAY_LONG:
        return "0min";
      case UNIT_DISPLAY_SHORT:
        return "0m";
      case UNIT_DISPLAY_NONE:
        return "0";
    }
  }

  int minutes = diff / 60;

  if (minutes < 60) {
    switch (this->unit_display_) {
      case UNIT_DISPLAY_LONG:
        return str_sprintf("%dmin", minutes);
      case UNIT_DISPLAY_SHORT:
        return str_sprintf("%dm", minutes);
      case UNIT_DISPLAY_NONE:
      default:
        return str_sprintf("%d", minutes);
    }
  }

  int hours = minutes / 60;
  minutes = minutes % 60;

  switch (this->unit_display_) {
    case UNIT_DISPLAY_LONG:
    case UNIT_DISPLAY_SHORT:
      return str_sprintf("%dh%dm", hours, minutes);
    case UNIT_DISPLAY_NONE:
    default:
      return str_sprintf("%d:%02d", hours, minutes);
  }
}

const uint8_t realtime_icon[6][6] = {
  {0, 0, 0, 3, 3, 3},
  {0, 0, 3, 0, 0, 0},
  {0, 3, 0, 0, 2, 2},
  {3, 0, 0, 2, 0, 0},
  {3, 0, 2, 0, 0, 1},
  {3, 0, 2, 0, 1, 1}
};

void HOT TransitTracker::draw_realtime_icon_(int bottom_right_x, int bottom_right_y) {
  const int num_frames = 6;
  const int idle_frame_duration = 3000;
  const int anim_frame_duration = 200;
  const int cycle_duration = idle_frame_duration + (num_frames - 1) * anim_frame_duration;

  long now = millis();
  long cycle_time = now % cycle_duration;

  int frame;
  if (cycle_time < idle_frame_duration) {
    frame = 0;
  } else {
    frame = 1 + (cycle_time - idle_frame_duration) / anim_frame_duration;
  }

  auto is_segment_lit = [frame](uint8_t segment) {
    switch (segment) {
      case 1: return frame >= 1 && frame <= 3;
      case 2: return frame >= 2 && frame <= 4;
      case 3: return frame >= 3 && frame <= 5;
      default: return false;
    }
  };

  const Color lit_color = Color(0x20FF00);
  const Color unlit_color = Color(0x00A700);

  for (uint8_t i = 0; i < 6; ++i) {
    for (uint8_t j = 0; j < 6; ++j) {
      uint8_t segment_number = realtime_icon[i][j];
      if (segment_number == 0) {
        continue;
      }

      Color icon_color = is_segment_lit(segment_number) ? lit_color : unlit_color;
      this->display_->draw_pixel_at(bottom_right_x - (5 - j), bottom_right_y - (5 - i), icon_color);
    }
  }
}

void TransitTracker::next_stop() {
  if (stop_ids_.empty()) {
      ESP_LOGW(TAG, "No stops loaded; skipping next_stop()");
      return;
  }

  current_stop_index_ = (current_stop_index_ + 1) % stop_ids_.size();

  const auto &stop_id = stop_ids_[current_stop_index_];
  const std::string &current_stop_name = stop_names_[stop_id];

  if (current_stop_name == last_displayed_stop_name_) {
    total_subpages_for_current_stop_ = 1;  // Only schedule page
  } else {
    total_subpages_for_current_stop_ = 2;  // Stop name + schedule page
    last_displayed_stop_name_ = current_stop_name;
  }

  current_subpage_index_ = 0;
}

void TransitTracker::draw_current_page() {
  if (total_subpages_for_current_stop_ == 1) {
    // Only schedule page exists
    this->draw_schedule();
  } else {
    if (current_subpage_index_ == 0) {
      this->draw_stop_name();
    } else {
      this->draw_schedule();
    }
  }
}

void TransitTracker::tick() {
  unsigned long now = millis();
  if (now - last_page_switch_ >= current_page_duration_) {
    current_subpage_index_++;

    if (current_subpage_index_ >= total_subpages_for_current_stop_) {
      this->next_stop();  // also resets current_subpage_index_ = 0
    }

    this->draw_current_page();

    // Set duration based on new subpage
    if (total_subpages_for_current_stop_ == 1 || current_subpage_index_ == 1) {
      current_page_duration_ = 8000;  // schedule page
    } else {
      current_page_duration_ = 5000;  // stop name page
    }

    last_page_switch_ = now;
  }
}

void HOT TransitTracker::draw_stop_name() {
  if (stop_ids_.empty()) {
    this->draw_text_centered_("No Stops Configured", Color(0x252627));
    return;
  }

  const auto &stop_id = stop_ids_[current_stop_index_];
  const auto it = stop_names_.find(stop_id);

  std::string stop_name = (it != stop_names_.end()) ? it->second : "Unknown Stop";

  int x = this->display_->get_width() / 2;
  int y = this->display_->get_height() / 2;
  this->display_->print(x, y - 6, this->font_, Color(0x00AEEF), display::TextAlign::CENTER, stop_name.c_str());

  if (this->display_departure_times_) {
    this->display_->print(x, y + 6, this->font_, Color(0xFFFFFF), display::TextAlign::CENTER, "Upcoming Bus Departures");
  } else {
    this->display_->print(x, y + 6, this->font_, Color(0xFFFFFF), display::TextAlign::CENTER, "Upcoming Bus Arrivals");
  }
}

void HOT TransitTracker::draw_schedule() {
  if (this->display_ == nullptr) {
    ESP_LOGW(TAG, "No display attached, cannot draw schedule");
    return;
  }

  if (!esphome::network::is_connected()) {
    this->draw_text_centered_("Connecting to Wi-Fi", Color(0x252627));
    return;
  }

  if (!this->rtc_->now().is_valid()) {
    this->draw_text_centered_("Waiting for time sync", Color(0x252627));
    return;
  }

  if (this->base_url_.empty()) {
    this->draw_text_centered_("No base URL set", Color(0x252627));
    return;
  }

  if (this->status_has_error()) {
    this->draw_text_centered_("Error loading schedule", Color(0xFE4C5C));
    return;
  }

  if (!this->has_ever_connected_) {
    this->draw_text_centered_("Loading...", Color(0x252627));
    return;
  }

  if (stop_ids_.empty()) {
    this->draw_text_centered_("No Stops Configured", Color(0x252627));
    return;
  }

  const auto &stop_id = stop_ids_[current_stop_index_];

  std::lock_guard<std::mutex> lock(this->schedule_state_.mutex);

  // Filter trips for this stop
  std::vector<const Trip*> matching_trips;
  for (const Trip &trip : this->schedule_state_.trips) {
    if (trip.stop_id == stop_id) {
      matching_trips.push_back(&trip);
    }
    if (matching_trips.size() >= this->display_limit_) {
      break;  // Stop once display limit is reached
    }
  }

  if (matching_trips.empty()) {
    auto message = "No upcoming arrivals";
    if (this->display_departure_times_) {
      message = "No upcoming departures";
    }

    this->draw_text_centered_(message, Color(0x252627));
    return;
  }

  int routeMaxWidth = 0;
  for (const Trip* trip : matching_trips) {
    int route_width, route_x_offset, route_baseline, route_height;
    this->font_->measure(trip->route_name.c_str(), &route_width, &route_x_offset, &route_baseline, &route_height);
    routeMaxWidth = std::max(routeMaxWidth, route_width);
  }

  int y_offset = 2;
  for (const Trip* trip : matching_trips) {
    this->display_->print(0, y_offset, this->font_, trip->route_color, display::TextAlign::TOP_LEFT, trip->route_name.c_str());

    int route_width, route_x_offset, route_baseline, route_height;
    this->font_->measure(trip->route_name.c_str(), &route_width, &route_x_offset, &route_baseline, &route_height);

    auto time_display = this->from_now_(this->display_departure_times_ ? trip->departure_time : trip->arrival_time);

    int time_width, time_x_offset, time_baseline, time_height;
    this->font_->measure(time_display.c_str(), &time_width, &time_x_offset, &time_baseline, &time_height);

    int headsign_clipping_end = this->display_->get_width() - time_width - 4;

    Color time_color = trip->is_realtime ? Color(0x20FF00) : Color(0xa7a7a7);
    this->display_->print(this->display_->get_width() + 1, y_offset, this->font_, time_color, display::TextAlign::TOP_RIGHT, time_display.c_str());

    if (trip->is_realtime) {
      int icon_bottom_right_x = this->display_->get_width() - time_width - 2;
      int icon_bottom_right_y = y_offset + time_height - 6;
      headsign_clipping_end -= 8;
      this->draw_realtime_icon_(icon_bottom_right_x, icon_bottom_right_y);
    }

    this->display_->start_clipping(0, 0, headsign_clipping_end, this->display_->get_height());
    this->display_->print(routeMaxWidth + 3, y_offset, this->font_, trip->headsign.c_str());
    this->display_->end_clipping();

    y_offset += route_height;
  }
}

}  // namespace transit_tracker
}  // namespace esphome
