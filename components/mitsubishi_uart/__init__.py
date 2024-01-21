import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart, sensor, select
from esphome.core import CORE
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_SUPPORTED_MODES,
    CONF_CUSTOM_FAN_MODES,
    CONF_SUPPORTED_FAN_MODES,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_CONFIG,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)
from esphome.core import coroutine

AUTO_LOAD = ["climate", "select", "sensor"]
DEPENDENCIES = ["uart", "climate", "sensor", "select"]

CONF_HP_UART = "heatpump_uart"

CONF_SENSORS = "sensors"
CONF_SENSORS_CURRENT_TEMP = "current_temperature"

CONF_SELECTS = "selects"
CONF_TEMPERATURE_SOURCE_SELECT = "temperature_source_select" # This is to create a Select object for selecting a source
CONF_TEMPERATURE_SOURCES = "temperature_sources" # This is for specifying additinoal sources

DEFAULT_POLLING_INTERVAL = "5s"

mitsubishi_uart_ns = cg.esphome_ns.namespace("mitsubishi_uart")
MitsubishiUART = mitsubishi_uart_ns.class_("MitsubishiUART", cg.PollingComponent, climate.Climate)

TemperatureSourceSelect = mitsubishi_uart_ns.class_("TemperatureSourceSelect", select.Select)

DEFAULT_CLIMATE_MODES = ["OFF", "HEAT", "DRY", "COOL", "FAN_ONLY", "HEAT_COOL"]
DEFAULT_FAN_MODES = ["AUTO", "QUIET", "LOW", "MEDIUM", "HIGH"]
CUSTOM_FAN_MODES = {
    "VERYHIGH": mitsubishi_uart_ns.FAN_MODE_VERYHIGH
}

INTERNAL_TEMPERATURE_SOURCE_OPTIONS = [mitsubishi_uart_ns.TEMPERATURE_SOURCE_INTERNAL] # These will always be available

validate_custom_fan_modes = cv.enum(CUSTOM_FAN_MODES, upper=True)

BASE_SCHEMA = cv.polling_component_schema(DEFAULT_POLLING_INTERVAL).extend(climate.CLIMATE_SCHEMA).extend({
    cv.GenerateID(CONF_ID): cv.declare_id(MitsubishiUART),
    cv.Required(CONF_HP_UART): cv.use_id(uart.UARTComponent),
    cv.Optional(CONF_NAME, default="Climate") : cv.string,

    cv.Optional(CONF_SUPPORTED_MODES, default=DEFAULT_CLIMATE_MODES) : cv.ensure_list(climate.validate_climate_mode),
    cv.Optional(CONF_SUPPORTED_FAN_MODES, default=DEFAULT_FAN_MODES): cv.ensure_list(climate.validate_climate_fan_mode),
    cv.Optional(CONF_CUSTOM_FAN_MODES, default=["VERYHIGH"]) : cv.ensure_list(validate_custom_fan_modes),
    cv.Optional(CONF_TEMPERATURE_SOURCES, default=[]) : cv.ensure_list(cv.use_id(sensor.Sensor))
    })

SENSORS = {
    CONF_SENSORS_CURRENT_TEMP: (
        "Current Temperature",
        sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
        accuracy_decimals=1,
        )
    )
}

SENSORS_SCHEMA = cv.All({
    cv.Optional(sensor_designator, default={"name": f"{sensor_name}"}): sensor_schema
    for sensor_designator, (sensor_name, sensor_schema) in SENSORS.items()
})

SELECTS = {
    CONF_TEMPERATURE_SOURCE_SELECT: (
        "Temperature Source",
        select.select_schema(
            TemperatureSourceSelect,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:thermometer-check"
        ),
        INTERNAL_TEMPERATURE_SOURCE_OPTIONS
    )
}

SELECTS_SCHEMA = cv.All({
    cv.Optional(select_designator, default={"name": f"{select_name}"}): select_schema
    for select_designator, (select_name, select_schema, select_options) in SELECTS.items()
})


CONFIG_SCHEMA = BASE_SCHEMA.extend({
    cv.Optional(CONF_SENSORS, default={}): SENSORS_SCHEMA,
    cv.Optional(CONF_SELECTS, default={}): SELECTS_SCHEMA,
})


@coroutine
async def to_code(config):
    hp_uart_component = await cg.get_variable(config[CONF_HP_UART])
    muart_component = cg.new_Pvariable(config[CONF_ID], hp_uart_component)

    await cg.register_component(muart_component, config)
    await climate.register_climate(muart_component, config)

    # Traits

    traits = muart_component.config_traits()

    if CONF_SUPPORTED_MODES in config:
        cg.add(traits.set_supported_modes(config[CONF_SUPPORTED_MODES]))

    if CONF_SUPPORTED_FAN_MODES in config:
        cg.add(traits.set_supported_fan_modes(config[CONF_SUPPORTED_FAN_MODES]))

    if CONF_CUSTOM_FAN_MODES in config:
        cg.add(traits.set_supported_custom_fan_modes(config[CONF_CUSTOM_FAN_MODES]))

    # Sensors

    for sensor_designator in SENSORS:
        sensor_conf = config[CONF_SENSORS][sensor_designator]
        sensor_component = cg.new_Pvariable(sensor_conf[CONF_ID])
        await sensor.register_sensor(sensor_component, sensor_conf)
        cg.add(getattr(muart_component, f"set_{sensor_designator}_sensor")(sensor_component))

    # Selects

    for ts_id in config[CONF_TEMPERATURE_SOURCES]:
        ts = await cg.get_variable(ts_id)
        SELECTS[CONF_TEMPERATURE_SOURCE_SELECT][2].append(ts.get_name())
        cg.add(getattr(ts, "add_on_state_callback")(
            # TODO: Is there anyway to do this without a raw expression?
            cg.RawExpression(
                f"[](float v){{{getattr(muart_component, 'temperature_source_report')}({ts.get_name()}, v);}}"
            )
        ))
        # TODO: Add callback to sensor pointed to MUART


    for select_designator, (select_name, select_schema, select_options) in SELECTS.items():
        select_conf = config[CONF_SELECTS][select_designator]
        select_component = cg.new_Pvariable(select_conf[CONF_ID])
        await select.register_select(select_component, select_conf, options=select_options)
        cg.add(getattr(muart_component, f"set_{select_designator}")(select_component))
        await cg.register_parented(select_component, muart_component)
