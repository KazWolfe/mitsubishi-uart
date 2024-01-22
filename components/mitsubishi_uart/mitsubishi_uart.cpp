#include "mitsubishi_uart.h"

namespace esphome {
namespace mitsubishi_uart {

// Static packets for set requests
static const Packet PACKET_CONNECT_REQ = ConnectRequestPacket();
static const Packet PACKET_SETTINGS_REQ = GetRequestPacket(GetCommand::gc_settings);
static const Packet PACKET_TEMP_REQ = GetRequestPacket(GetCommand::gc_current_temp);
static const Packet PACKET_STATUS_REQ = GetRequestPacket(GetCommand::gc_status);
static const Packet PACKET_STANDBY_REQ = GetRequestPacket(GetCommand::gc_standby);

////
// MitsubishiUART
////

MitsubishiUART::MitsubishiUART(uart::UARTComponent *hp_uart_comp) : hp_uart{*hp_uart_comp}, hp_bridge{MUARTBridge(*hp_uart_comp, *this)} {

  /**
   * Climate pushes all its data to Home Assistant immediately when the API connects, this causes
   * the default 0 to be sent as temperatures, but since this is a valid value (0 deg C), it
   * can cause confusion and mess with graphs when looking at the state in HA.  Setting this to
   * NAN gets HA to treat this value as "unavailable" until we have a real value to publish.
   */
  target_temperature = NAN;
  current_temperature = NAN;
}

// Used to restore state of previous MUART-specific settings (like temperature source or pass-thru mode)
// Most other climate-state is preserved by the heatpump itself and will be retrieved after connection
void MitsubishiUART::setup() {
  // Using App.get_compilation_time() means these will get reset each time the firmware is updated, but this
  // is an easy way to prevent wierd conflicts if e.g. select options change.
  preferences_ = global_preferences->make_preference<MUARTPreferences>(get_object_id_hash() ^ fnv1_hash(App.get_compilation_time()));
  restore_preferences();
}

void MitsubishiUART::save_preferences() {
  MUARTPreferences prefs{};

  // currentTemperatureSource
  if (temperature_source_select->active_index().has_value()){
    prefs.currentTemperatureSourceIndex = temperature_source_select->active_index().value();
  }

  preferences_.save(&prefs);
}

// Restores previously set values, or sets sane defaults
void MitsubishiUART::restore_preferences() {
  MUARTPreferences prefs;
  if (preferences_.load(&prefs)) {
    // currentTemperatureSource
    if (prefs.currentTemperatureSourceIndex.has_value()
    && temperature_source_select->has_index(prefs.currentTemperatureSourceIndex.value())
    && temperature_source_select->at(prefs.currentTemperatureSourceIndex.value()).has_value()) {
      temperature_source_select->publish_state(temperature_source_select->at(prefs.currentTemperatureSourceIndex.value()).value());
    } else {
      temperature_source_select->publish_state(TEMPERATURE_SOURCE_INTERNAL);
    }
  } else {
      // TODO: Shouldn't need to define setting all these defaults twice
      temperature_source_select->publish_state(TEMPERATURE_SOURCE_INTERNAL);
    }
}

/* Used for receiving and acting on incoming packets as soon as they're available.
  Because packet processing happens as part of the receiving process, packet processing
  should not block for very long (e.g. no publishing inside the packet processing)
*/
void MitsubishiUART::loop() {
  // Loop bridge to handle sending and receiving packets
  hp_bridge.loop();

  // If it's been too long since we received a temperature update (and we're not set to Internal)
  if (millis() - lastReceivedTemperature > TEMPERATURE_SOURCE_TIMEOUT_MS && (currentTemperatureSource != TEMPERATURE_SOURCE_INTERNAL)) {
    ESP_LOGW(TAG, "No temperature received from %s for %i milliseconds, reverting to Internal source", currentTemperatureSource, TEMPERATURE_SOURCE_TIMEOUT_MS);
    // Set the select to show Internal (but do not change currentTemperatureSource)
    // TODO: I think this actually currently changes the currentTemperatureSource because publish_state triggers a call
    temperature_source_select->publish_state(TEMPERATURE_SOURCE_INTERNAL);
    // Send a packet to the heat pump to tell it to switch to internal temperature sensing
    hp_bridge.sendPacket(RemoteTemperatureSetRequestPacket().useInternalTemperature());
  }
  //
  // Send packet to HP to tell it to use internal temp sensor
}

/* Called periodically as PollingComponent; used to send packets to connect or request updates.

Possible TODO: If we only publish during updates, since data is received during loop, updates will always
be about `update_interval` late from their actual time.  Generally the update interval should be low enough
(default is 5seconds) this won't pose a practical problem.
*/
void MitsubishiUART::update() {

  // TODO: Temporarily wait 5 seconds on startup to help with viewing logs
  if (millis() < 5000) {
    return;
  }

  // If we're not yet connected, send off a connection request (we'll check again next update)
  if (!hpConnected) {
    hp_bridge.sendPacket(PACKET_CONNECT_REQ);
    return;
  }

  // Before requesting additional updates, publish any changes waiting from packets received
  if (publishOnUpdate){
    doPublish();

    publishOnUpdate = false;
  }

  // Request an update from the heatpump
  hp_bridge.sendPacket(PACKET_SETTINGS_REQ); // Needs to be done before status packet for mode logic to work
  hp_bridge.sendPacket(PACKET_STANDBY_REQ);
  hp_bridge.sendPacket(PACKET_STATUS_REQ);
  hp_bridge.sendPacket(PACKET_TEMP_REQ);
}

void MitsubishiUART::doPublish() {
  publish_state();
  save_preferences(); // We can save this every time we publish as writes to flash are by default collected and delayed


  // Check sensors and publish if needed.
  // This is a bit of a hack to avoid needing to publish sensor data immediately as packets arrive.
  // Instead, packet data is written directly to `raw_state` (which doesn't update `state`).  If they
  // differ, calling `publish_state` will update `state` so that it won't be published later
  if (current_temperature_sensor && (current_temperature_sensor->raw_state != current_temperature_sensor->state)) {
    ESP_LOGI(TAG, "Current temp differs, do publish");
    current_temperature_sensor->publish_state(current_temperature_sensor->raw_state);
  }
}

bool MitsubishiUART::select_temperature_source(const std::string &state) {
  // TODO: Possibly check to see if state is available from the select options?  (Might be a bit redundant)

  currentTemperatureSource = state;

  // If we've switched to internal, let the HP know right away
  if (TEMPERATURE_SOURCE_INTERNAL == state) {
    hp_bridge.sendPacket(RemoteTemperatureSetRequestPacket().useInternalTemperature());
  }

  return true;
}

// Called by temperature_source sensors to report values.  Will only take action if the currentTemperatureSource
// matches the incoming source.  Specifically this means that we are not storing any values
// for sensors other than the current source, and selecting a different source won't have any
// effect until that source reports a temperature.
// TODO: ? Maybe store all temperatures (and report on them using internal sensors??) so that selecting a new
// source takes effect immediately?  Only really needed if source sensors are configured with very slow update times.
void MitsubishiUART::temperature_source_report(const std::string &temperature_source, const float &v) {
  ESP_LOGI(TAG, "Received temperature from %s of %f.", temperature_source.c_str(), v);

  // Only proceed if the incomming source matches our chosen source.
  if (currentTemperatureSource == temperature_source) {

    //Reset the timeout for received temperature
    lastReceivedTemperature = millis();

    // Tell the heat pump about the temperature asap, but don't worry about setting it locally, the next update() will get it
    RemoteTemperatureSetRequestPacket pkt = RemoteTemperatureSetRequestPacket();
    pkt.setRemoteTemperature(v);
    hp_bridge.sendPacket(pkt);

    // If we've changed the select to reflect a temporary reversion to a different source, change it back.
    if (temperature_source_select->state != temperature_source) {
      temperature_source_select->publish_state(temperature_source);
    }
  }
}

}  // namespace mitsubishi_uart
}  // namespace esphome
