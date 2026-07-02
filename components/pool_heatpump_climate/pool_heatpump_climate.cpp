#include "pool_heatpump_climate.h"
#include "esphome/core/log.h"
#include <cmath>

namespace esphome {
namespace pool_heatpump_climate {

static const char *const TAG = "pool_heatpump_climate";

// State values decoded from BLE register 0x07D1 rd(18):
//   0 = off / standby (no fan, no compressor)
//   4 = fan only (~40W) — pre-circulation or post-run cooldown
//   5 = compressor running (~700W) — actively heating
// Other values may appear (defrost, error, cooling) — extend update_action_() as discovered.
static constexpr int STATE_OFF = 0;
static constexpr int STATE_FAN_ONLY = 4;
static constexpr int STATE_HEATING = 5;

void PoolHeatpumpClimate::setup() {
  // Seed initial values if already known
  if (this->current_sensor_ != nullptr && !std::isnan(this->current_sensor_->state))
    this->current_temperature = this->current_sensor_->state;

  if (this->target_number_ != nullptr && !std::isnan(this->target_number_->state)) {
    this->target_temperature = this->target_number_->state;
  } else {
    this->target_temperature = 28.0f;  // sensible default until first BLE poll
  }

  // Mirror current temperature from the inlet sensor
  if (this->current_sensor_ != nullptr) {
    this->current_sensor_->add_on_state_callback([this](float v) {
      if (std::isnan(v)) return;
      this->current_temperature = v;
      this->update_action_();
      this->publish_state();
    });
  }

  // Mirror target from the number (catches both user slider changes
  // and values arriving from the heat pump's 0x0416 register)
  if (this->target_number_ != nullptr) {
    this->target_number_->add_on_state_callback([this](float v) {
      if (std::isnan(v)) return;
      this->target_temperature = v;
      this->update_action_();
      this->publish_state();
    });
  }

  // React to heat pump state changes from register 0x07D1 rd(18)
  if (this->state_sensor_ != nullptr) {
    this->state_sensor_->add_on_state_callback([this](float /*v*/) {
      this->update_action_();
      this->publish_state();
    });
  }

  // CHANGED: react to the Power switch (whether toggled by the user via the
  // standalone HA switch, by HA automation, or by the BLE notify parser
  // reflecting the pump's real power state).
  if (this->power_switch_ != nullptr) {
    this->power_switch_->add_on_state_callback([this](bool /*v*/) {
      this->update_mode_();
      this->update_action_();
      this->publish_state();
    });
  }

  // CHANGED: react to the Mode select the same way.
  // IMPORTANT — the select callback signature changed in ESPHome 2026.1
  // (PR #12505): from `void(std::string, size_t)` to `void(size_t)`. The
  // current option string is read via `mode_select_->current_option()` inside the
  // handler rather than passed as an argument.
  if (this->mode_select_ != nullptr) {
    this->mode_select_->add_on_state_callback([this](size_t /*idx*/) {
      this->update_mode_();
      this->update_action_();
      this->publish_state();
    });
  }

  // CHANGED: react to the compressor / fan binary sensors so the climate's
  // `action` flips between HEATING/COOLING/FAN/IDLE the moment the hardware
  // actually changes — not on a temperature guess.
  if (this->compressor_sensor_ != nullptr) {
    this->compressor_sensor_->add_on_state_callback([this](bool /*v*/) {
      this->update_action_();
      this->publish_state();
    });
  }
  if (this->fan_sensor_ != nullptr) {
    this->fan_sensor_->add_on_state_callback([this](bool /*v*/) {
      this->update_action_();
      this->publish_state();
    });
  }

  // Seed initial mode now — the callbacks above only fire on subsequent changes.
  this->update_mode_();
  this->update_action_();
  this->publish_state();
}

void PoolHeatpumpClimate::dump_config() {
  LOG_CLIMATE("", "Pool Heatpump Climate", this);
  // The optional inputs decide which action-derivation path runs, so log
  // what's actually wired at boot.
  ESP_LOGCONFIG(TAG, "  State sensor: %s", YESNO(this->state_sensor_ != nullptr));
  ESP_LOGCONFIG(TAG, "  Compressor sensor: %s", YESNO(this->compressor_sensor_ != nullptr));
  ESP_LOGCONFIG(TAG, "  Fan sensor: %s", YESNO(this->fan_sensor_ != nullptr));
}

climate::ClimateTraits PoolHeatpumpClimate::traits() {
  auto traits = climate::ClimateTraits();
  // Modern feature-flag API (ESPHome 2025.11+). The old set_supports_* accessors
  // were deprecated in 2025.11 and removed in 2026.5.
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           climate::CLIMATE_SUPPORTS_ACTION);
  // CHANGED: expose all four modes the heat pump supports, matching the
  // underlying Mode select 1:1. CLIMATE_MODE_AUTO (rather than HEAT_COOL)
  // is chosen so HA displays the label "Auto" exactly like the select.
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_AUTO,
  });
  // CHANGED: visual range widened to cover cool (10–35) as well as heat
  // (15–40). The write_target_temp script clamps per-mode before sending.
  traits.set_visual_min_temperature(10.0);
  traits.set_visual_max_temperature(40.0);
  traits.set_visual_temperature_step(0.5);
  return traits;
}

void PoolHeatpumpClimate::control(const climate::ClimateCall &call) {
  // ── Target temperature ──
  // Delegate to the Number — its set_action does the BLE write + repoll.
  if (call.get_target_temperature().has_value() && this->target_number_ != nullptr) {
    float v = *call.get_target_temperature();
    auto ncall = this->target_number_->make_call();
    ncall.set_value(v);
    ncall.perform();
    this->target_temperature = v;
  }

  // ── HVAC mode ──
  // CHANGED: was hard-coded to CLIMATE_MODE_HEAT. Now we translate the
  // requested HVAC mode into the underlying Switch + Select state.
  if (call.get_mode().has_value()) {
    climate::ClimateMode m = *call.get_mode();
    bool want_on = (m != climate::CLIMATE_MODE_OFF);

    // 1) When going on, set the Mode select FIRST so the on_state_callback
    //    racing in from the Switch turn_on sees the correct select state
    //    and computes the right HVAC mode immediately. (Order matters
    //    because the callbacks fire synchronously inside the make_call.)
    if (want_on && this->mode_select_ != nullptr) {
      std::string sel;
      switch (m) {
        case climate::CLIMATE_MODE_HEAT: sel = "Heat"; break;
        case climate::CLIMATE_MODE_COOL: sel = "Cool"; break;
        case climate::CLIMATE_MODE_AUTO: sel = "Auto"; break;
        default: sel = ""; break;
      }
      // current_option() returns a StringRef (replaces the deprecated .state,
      // removed in ESPHome 2026.7). StringRef compares directly against
      // std::string, so no conversion is needed here.
      if (!sel.empty() &&
          (!this->mode_select_->has_state() || this->mode_select_->current_option() != sel)) {
        auto scall = this->mode_select_->make_call();
        scall.set_option(sel);
        scall.perform();
      }
    }

    // 2) Then drive the Power switch — only if it actually needs to change.
    //    Going OFF deliberately doesn't touch the Mode select; we preserve
    //    the last mode so turning back on restores it.
    if (this->power_switch_ != nullptr) {
      if (want_on && !this->power_switch_->state) {
        this->power_switch_->turn_on();
      } else if (!want_on && this->power_switch_->state) {
        this->power_switch_->turn_off();
      }
    }

    // Set authoritatively from the requested mode. The on_state callbacks
    // from the Switch/Select above will also have called update_mode_(),
    // which derives the same value — this is just belt-and-braces.
    this->mode = m;
  }

  this->update_action_();
  this->publish_state();
}

// CHANGED (NEW): derive this->mode from the underlying Switch + Select.
// Called from the on_state callbacks so external changes — physical-panel
// adjustments reflected via BLE, direct toggling of the standalone Switch
// or Select in HA, automation actions — all flow through to the climate
// entity's mode automatically.
void PoolHeatpumpClimate::update_mode_() {
  // No switch wired, or switch is off → OFF.
  if (this->power_switch_ == nullptr || !this->power_switch_->state) {
    this->mode = climate::CLIMATE_MODE_OFF;
    return;
  }
  // Switch is on — derive HVAC mode from the Mode select.
  if (this->mode_select_ == nullptr || !this->mode_select_->has_state()) {
    this->mode = climate::CLIMATE_MODE_HEAT;  // sensible default
    return;
  }
  // current_option() replaces the deprecated .state (removed in 2026.7).
  // Bind by value — the returned StringRef is a temporary, so a const ref
  // would dangle. It converts cleanly to std::string.
  std::string s = this->mode_select_->current_option();
  if (s == "Cool") this->mode = climate::CLIMATE_MODE_COOL;
  else if (s == "Auto") this->mode = climate::CLIMATE_MODE_AUTO;
  else this->mode = climate::CLIMATE_MODE_HEAT;  // "Heat" and anything unknown
}

void PoolHeatpumpClimate::update_action_() {
  // ── Priority 1: derive from the compressor + fan binary sensors ──
  // The most accurate path — these come straight off the pump's output
  // bitmask (register 0x07E3). Fixes the case where the heuristic below
  // reports HEATING simply because current < target, even when nothing
  // is running.
  if (this->compressor_sensor_ != nullptr) {
    bool comp_on = this->compressor_sensor_->state;
    if (comp_on) {
      // Compressor is running — the climate's mode tells us the direction.
      // In AUTO we don't have a direct heat/cool signal here, so tiebreak
      // with the temperature delta; if the 4-way valve sensor is ever
      // wired up, that'd be the more authoritative source.
      switch (this->mode) {
        case climate::CLIMATE_MODE_COOL:
          this->action = climate::CLIMATE_ACTION_COOLING;
          break;
        case climate::CLIMATE_MODE_AUTO:
          if (!std::isnan(this->current_temperature) &&
              !std::isnan(this->target_temperature) &&
              this->current_temperature > this->target_temperature) {
            this->action = climate::CLIMATE_ACTION_COOLING;
          } else {
            this->action = climate::CLIMATE_ACTION_HEATING;
          }
          break;
        case climate::CLIMATE_MODE_HEAT:
        default:
          this->action = climate::CLIMATE_ACTION_HEATING;
          break;
      }
      return;
    }
    // Compressor off — fan-only if we have a fan sensor and it's on.
    if (this->fan_sensor_ != nullptr && this->fan_sensor_->state) {
      this->action = climate::CLIMATE_ACTION_FAN;
      return;
    }
    // Compressor off, no fan running.
    this->action = (this->mode == climate::CLIMATE_MODE_OFF)
                       ? climate::CLIMATE_ACTION_OFF
                       : climate::CLIMATE_ACTION_IDLE;
    return;
  }

  // ── Priority 2: integer state_sensor (legacy path) ──
  if (this->state_sensor_ != nullptr && !std::isnan(this->state_sensor_->state)) {
    int s = (int) this->state_sensor_->state;
    switch (s) {
      case STATE_HEATING:
        this->action = climate::CLIMATE_ACTION_HEATING;
        break;
      case STATE_FAN_ONLY:
        this->action = climate::CLIMATE_ACTION_FAN;
        break;
      case STATE_OFF:
        this->action = climate::CLIMATE_ACTION_IDLE;
        break;
      default:
        // Unknown state value — log it so it can be characterised later
        ESP_LOGW(TAG, "Unknown heat pump state value: %d — defaulting to IDLE", s);
        this->action = climate::CLIMATE_ACTION_IDLE;
        break;
    }
    return;
  }

  // ── Priority 3: temperature-delta fallback ──
  // Inaccurate (this is the cause of the "shows heating when idle" bug
  // when neither of the above is wired). Kept only so the component still
  // produces something sensible if used without the hardware-state inputs.
  if (this->mode == climate::CLIMATE_MODE_OFF ||
      std::isnan(this->current_temperature) ||
      std::isnan(this->target_temperature)) {
    this->action = climate::CLIMATE_ACTION_IDLE;
    return;
  }

  float delta = this->target_temperature - this->current_temperature;
  switch (this->mode) {
    case climate::CLIMATE_MODE_HEAT:
      this->action = (delta > 0.3f) ? climate::CLIMATE_ACTION_HEATING
                                    : climate::CLIMATE_ACTION_IDLE;
      break;
    case climate::CLIMATE_MODE_COOL:
      this->action = (delta < -0.3f) ? climate::CLIMATE_ACTION_COOLING
                                     : climate::CLIMATE_ACTION_IDLE;
      break;
    case climate::CLIMATE_MODE_AUTO:
      if (delta > 0.3f) this->action = climate::CLIMATE_ACTION_HEATING;
      else if (delta < -0.3f) this->action = climate::CLIMATE_ACTION_COOLING;
      else this->action = climate::CLIMATE_ACTION_IDLE;
      break;
    default:
      this->action = climate::CLIMATE_ACTION_IDLE;
      break;
  }
}

}  // namespace pool_heatpump_climate
}  // namespace esphome
