import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, climate, sensor, number, select, switch
from esphome.const import CONF_SENSOR

from . import pool_heatpump_ns

CONF_TARGET_NUMBER = "target_number"
CONF_STATE_SENSOR = "state_sensor"
CONF_POWER_SWITCH = "power_switch"
CONF_MODE_SELECT = "mode_select"
# CHANGED: optional binary sensors that report real running state. When wired,
# they take priority over the temperature-delta heuristic so the climate's
# `action` reflects what the hardware is actually doing (compressor running →
# HEATING/COOLING, fan-only → FAN, both off → IDLE).
CONF_COMPRESSOR_SENSOR = "compressor_sensor"
CONF_FAN_SENSOR = "fan_sensor"

PoolHeatpumpClimate = pool_heatpump_ns.class_(
    "PoolHeatpumpClimate", climate.Climate, cg.Component
)

def _validate_action_inputs(config):
    # The hardware-state action path in update_action_() is only entered when
    # a compressor sensor is wired; a fan sensor on its own would be silently
    # ignored, so reject that combination at validation time.
    if CONF_FAN_SENSOR in config and CONF_COMPRESSOR_SENSOR not in config:
        raise cv.Invalid(
            f"'{CONF_FAN_SENSOR}' requires '{CONF_COMPRESSOR_SENSOR}' to be set as well"
        )
    return config


CONFIG_SCHEMA = cv.All(
    climate.climate_schema(PoolHeatpumpClimate)
    .extend(
        {
            cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
            cv.Required(CONF_TARGET_NUMBER): cv.use_id(number.Number),
            cv.Required(CONF_POWER_SWITCH): cv.use_id(switch.Switch),
            cv.Required(CONF_MODE_SELECT): cv.use_id(select.Select),
            cv.Optional(CONF_STATE_SENSOR): cv.use_id(sensor.Sensor),
            # CHANGED: new optional binary-sensor inputs for accurate action.
            cv.Optional(CONF_COMPRESSOR_SENSOR): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_FAN_SENSOR): cv.use_id(binary_sensor.BinarySensor),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    _validate_action_inputs,
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)

    sens = await cg.get_variable(config[CONF_SENSOR])
    cg.add(var.set_current_sensor(sens))

    num = await cg.get_variable(config[CONF_TARGET_NUMBER])
    cg.add(var.set_target_number(num))

    sw = await cg.get_variable(config[CONF_POWER_SWITCH])
    cg.add(var.set_power_switch(sw))

    sel = await cg.get_variable(config[CONF_MODE_SELECT])
    cg.add(var.set_mode_select(sel))

    if CONF_STATE_SENSOR in config:
        st = await cg.get_variable(config[CONF_STATE_SENSOR])
        cg.add(var.set_state_sensor(st))

    # CHANGED: wire the optional compressor / fan binary sensors.
    if CONF_COMPRESSOR_SENSOR in config:
        comp = await cg.get_variable(config[CONF_COMPRESSOR_SENSOR])
        cg.add(var.set_compressor_sensor(comp))

    if CONF_FAN_SENSOR in config:
        fan = await cg.get_variable(config[CONF_FAN_SENSOR])
        cg.add(var.set_fan_sensor(fan))
