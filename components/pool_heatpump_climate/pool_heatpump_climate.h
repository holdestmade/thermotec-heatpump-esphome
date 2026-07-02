#pragma once

#include "esphome/core/component.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/number/number.h"
// CHANGED: pull in switch and select so the climate can drive them and
// subscribe to their state changes.
#include "esphome/components/switch/switch.h"
#include "esphome/components/select/select.h"
// CHANGED: pull in binary_sensor for the compressor / fan running-state inputs.
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace pool_heatpump_climate {

class PoolHeatpumpClimate : public Component, public climate::Climate {
 public:
  void set_current_sensor(sensor::Sensor *s) { current_sensor_ = s; }
  void set_target_number(number::Number *n) { target_number_ = n; }
  void set_state_sensor(sensor::Sensor *s) { state_sensor_ = s; }
  // CHANGED: new setters wired up from climate.py.
  // Note `switch_` (with trailing underscore) — `switch` is a C++ keyword,
  // so ESPHome's switch component lives in namespace `switch_`.
  void set_power_switch(switch_::Switch *s) { power_switch_ = s; }
  void set_mode_select(select::Select *s) { mode_select_ = s; }
  // CHANGED: optional binary-sensor inputs from the pump's output bitmask.
  // When provided, update_action_() derives the climate's action from them
  // instead of the temperature-delta heuristic — so HEATING/COOLING is only
  // reported when the compressor is genuinely running, and FAN is reported
  // when the fan is on without the compressor.
  void set_compressor_sensor(binary_sensor::BinarySensor *s) { compressor_sensor_ = s; }
  void set_fan_sensor(binary_sensor::BinarySensor *s) { fan_sensor_ = s; }

  void setup() override;
  void dump_config() override;
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;

 protected:
  void update_action_();
  // CHANGED: new helper that derives this->mode from the underlying
  // Power switch + Mode select. Called whenever either of them changes.
  void update_mode_();

  sensor::Sensor *current_sensor_{nullptr};
  number::Number *target_number_{nullptr};
  sensor::Sensor *state_sensor_{nullptr};
  switch_::Switch *power_switch_{nullptr};
  select::Select *mode_select_{nullptr};
  // CHANGED: new optional inputs.
  binary_sensor::BinarySensor *compressor_sensor_{nullptr};
  binary_sensor::BinarySensor *fan_sensor_{nullptr};
};

}  // namespace pool_heatpump_climate
}  // namespace esphome
