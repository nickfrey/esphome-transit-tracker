#pragma once

#include <map>
#include <ArduinoWebsockets.h>

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/time/real_time_clock.h"

#include "schedule_state.h"

namespace esphome {
namespace transit_tracker {

struct RouteStyle {
  std::string name;
  Color color;
};

enum UnitDisplay : uint8_t {
  UNIT_DISPLAY_LONG,
  UNIT_DISPLAY_SHORT,
  UNIT_DISPLAY_NONE
};

class TransitTracker : public Component {
  public:
    void setup() override;
    void loop() override;
    void dump_config() override;
    void on_shutdown() override;

    float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

    void reconnect();
    void close(bool fully = false);

    void draw_current_page();
    void tick();

    void set_display(display::Display *display) { display_ = display; }
    void set_font(font::Font *font) { font_ = font; }
    void set_rtc(time::RealTimeClock *rtc) { rtc_ = rtc; }

    void set_base_url(const std::string &base_url) { base_url_ = base_url; }
    void set_feed_code(const std::string &feed_code) { feed_code_ = feed_code; }
    void set_display_departure_times(bool display_departure_times) { display_departure_times_ = display_departure_times; }
    void set_schedule_string(const std::string &schedule_string) { schedule_string_ = schedule_string; }
    void set_list_mode(const std::string &list_mode) { list_mode_ = list_mode; }
    void set_limit(int limit) { limit_ = limit; }
    void set_display_limit(int limit) { display_limit_ = limit; }

    void set_unit_display(UnitDisplay unit_display) { unit_display_ = unit_display; }
    void add_abbreviation(const std::string &from, const std::string &to) { abbreviations_[from] = to; }
    void set_default_route_color(const Color &color) { default_route_color_ = color; }
    void add_route_style(const std::string &route_id, const std::string &name, const Color &color) { route_styles_[route_id] = RouteStyle{name, color}; }

    void set_abbreviations_from_text(const std::string &text);
    void set_route_styles_from_text(const std::string &text);
    void add_stop_name(const std::string &stop_id, const std::string &stop_name) {
        stop_ids_.push_back(stop_id);
        stop_names_[stop_id] = stop_name;
    }

  protected:
    std::string from_now_(time_t unix_timestamp) const;
    void draw_text_centered_(const char *text, Color color);
    void draw_realtime_icon_(int bottom_right_x, int bottom_right_y);

    ScheduleState schedule_state_;

    display::Display *display_;
    font::Font *font_;
    time::RealTimeClock *rtc_;

    websockets::WebsocketsClient ws_client_{};

    void on_ws_message_(websockets::WebsocketsMessage message);
    void on_ws_event_(websockets::WebsocketsEvent event, String data);
    void connect_ws_();
    int connection_attempts_ = 0;
    long last_heartbeat_ = 0;
    bool has_ever_connected_ = false;
    bool fully_closed_ = false;

    std::string base_url_;
    std::string feed_code_;
    std::string schedule_string_;
    std::string list_mode_;
    bool display_departure_times_ = true;
    int limit_;
    int display_limit_;

    UnitDisplay unit_display_ = UNIT_DISPLAY_LONG;
    std::map<std::string, std::string> abbreviations_;
    Color default_route_color_ = Color(0x028e51);
    std::map<std::string, RouteStyle> route_styles_;
    std::map<std::string, std::string> stop_names_;
    std::vector<std::string> stop_ids_;
    
    int current_stop_index_ = 0;
    int current_subpage_index_ = 0;
    int total_subpages_for_current_stop_ = 1;
    std::string last_displayed_stop_name_;
    unsigned long last_page_switch_ = 0;
    unsigned long current_page_duration_ = 0;
    
    void next_stop();
    void draw_stop_name();
    void draw_schedule();
};


}  // namespace transit_tracker
}  // namespace esphome
