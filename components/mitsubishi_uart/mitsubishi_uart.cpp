#include "mitsubishi_uart.h"

namespace esphome {
namespace mitsubishi_uart {

// TODO (Can I move these into the packets files?)
// Pre-built packets
const Packet PACKET_CONNECT_REQ = PacketConnectRequest();
const Packet PACKET_SETTINGS_REQ = PacketGetRequest(PacketGetCommand::settings);
const Packet PACKET_TEMP_REQ = PacketGetRequest(PacketGetCommand::room_temp);
const Packet PACKET_STATUS_REQ = PacketGetRequest(PacketGetCommand::status);
const Packet PACKET_STANDBY_REQ = PacketGetRequest(PacketGetCommand::standby);

#define DIR_MC_HP "MC->HP"
#define DIR_HP_MC "MC<-HP"
#define DIR_TS_MC "TS->MC"
#define DIR_MC_TS "TS<-MC"

////
// MitsubishiUART
////

MitsubishiUART::MitsubishiUART(uart::UARTComponent *uart_comp) : hp_uart{uart_comp} {}

void MitsubishiUART::loop() {
  // If a packet is available, read and handle it.  Only do one packet per loop to keep things responsive
  readPacket(hp_uart, false);
  if (tstat_uart) {
    readPacket(tstat_uart, false);
  }
}

void MitsubishiUART::update() {
  ESP_LOGV(TAG, "Update called.");

  if (passive_mode) {
    // If we're not trying to request updates, go ahead and try to publish any updates we picked up on.
    this->climate_->lazy_publish_state({nullptr});
    return;
  }

  int packetsRead = 0;

  /**
   * For whatever reason:
   * - Sending multiple request packets very quickly will result in only a response to the first one.  It seems
   * like the heat pump may not be entirely breaking communications by control byte (or possibly it empties the
   * input buffer between requests?)
   *
   * - Sending multiple request packets sort of quickly results in multiple responses, but the first responses
   * are missing their checksums.
   *
   * As a result, we blocking-read for a response as part of sendPacket()
   */
  if (connectState == 2) {
    // Request room temp
    sendPacket(PACKET_TEMP_REQ, hp_uart) ? packetsRead++ : 0;
    // Request settings (needs to be done before status for mode logic to work)
    sendPacket(PACKET_SETTINGS_REQ, hp_uart) ? packetsRead++ : 0;
    // Request status
    sendPacket(PACKET_STATUS_REQ, hp_uart) ? packetsRead++ : 0;

    // Request standby info
    sendPacket(PACKET_STANDBY_REQ, hp_uart) ? packetsRead++ : 0;

    // This will publish the state IFF something has changed. Only called if connected
    // so any updates to connection status will need to be done outside this.
    this->climate_->lazy_publish_state({nullptr});
  }

  if (packetsRead > 0) {
    updatesSinceLastPacket = 0;
  } else {
    updatesSinceLastPacket++;
  }

  if (updatesSinceLastPacket > 10) {
    ESP_LOGI(TAG, "No packets received in %d updates, connection down.", updatesSinceLastPacket);
    connectState = 0;
  }

  // If we're not connected (or have become unconnected) try to send a connect packet again
  if (connectState < 2) {
    connect();
  }
}

void MitsubishiUART::dump_config() {
  ESP_LOGCONFIG(TAG, "Mitsubishi UART v%s", MUART_VERSION);
  ESP_LOGCONFIG(TAG, "Connection state: %d", connectState);
}

void MitsubishiUART::connect() {
  connectState = 1;  // Connecting...
  sendPacket(PACKET_CONNECT_REQ, hp_uart);
}

void logPacket(const char *direction, Packet packet) {
  ESP_LOGD(TAG, "%s [%02x] %s", direction, packet.getPacketType(),
           format_hex_pretty(&packet.getBytes()[PACKET_HEADER_SIZE], packet.getLength() - PACKET_HEADER_SIZE).c_str());
}

// Send packet on designated UART interface (as long as it exists, regardless of connection state)
// CAUTION: expecting a response will block until a packet is received, using this inappropriately
// could hang execution or cause infinite recursion issues.  TODO: Could we prevent that?
bool MitsubishiUART::sendPacket(Packet packet, uart::UARTComponent *uart, bool expectResponse) {
  if (!uart) {
    return false;
  }
  logPacket(uart == hp_uart ? DIR_MC_HP : DIR_MC_TS, packet);
  uart->write_array(packet.getBytes(), packet.getLength());
  if (expectResponse) {
    return readPacket(uart);
  }
  return false;
}

/**
 * Reads packets from UARTs, and sends them to appropriate handler methods.
 *
 * An assumption is made that all response packets received are coming from the heat pump,
 * and all request packets received are coming from an attached thermostat (i.e. we don't
 * need to track the source UART of any packets because we can deduce it.)
 */
bool MitsubishiUART::readPacket(uart::UARTComponent *uart, bool waitForPacket) {
  uint8_t p_byte;
  bool foundPacket = false;
  unsigned long readStop = millis() + PACKET_RECEIVE_TIMEOUT;

  if (!uart) {
    return false;
  }

  while (millis() < readStop) {
    // Search for control byte (or check that one is waiting for us)
    while (uart->available() > PACKET_HEADER_SIZE && uart->peek_byte(&p_byte)) {
      if (p_byte == BYTE_CONTROL) {
        foundPacket = true;
        ESP_LOGV(TAG, "FoundPacket!");
        break;
      } else {
        uart->read_byte(&p_byte);
      }
    }
    if (foundPacket) {
      break;
    }
    if (!waitForPacket) {
      break;
    }
    delay(10);
  }

  // If control byte has been found and there's at least a header available, parse the packet
  if (foundPacket && uart->available() > PACKET_HEADER_SIZE) {
    uint8_t p_header[PACKET_HEADER_SIZE];
    uart->read_array(p_header, PACKET_HEADER_SIZE);
    const int payloadSize = p_header[PACKET_HEADER_INDEX_PAYLOAD_SIZE];
    uint8_t p_payload[payloadSize];
    uint8_t checksum;
    uart->read_array(p_payload, payloadSize);
    uart->read_byte(&checksum);

    // TODO Don't like needing to build a packet just for this...
    logPacket(uart == hp_uart ? DIR_HP_MC : DIR_TS_MC, Packet(p_header, p_payload, payloadSize, checksum));

    switch (p_header[PACKET_HEADER_INDEX_PACKET_TYPE]) {
      case PacketType::connect_response:
        hResConnect(PacketConnectResponse(p_header, p_payload, payloadSize, checksum));
        break;
      case PacketType::extended_connect_response:
        hResExtendedConnect(PacketExtendedConnectResponse(p_header, p_payload, payloadSize, checksum));
        break;
      case PacketType::get_response:
        switch (p_payload[0]) {  // Switch on command type
          case PacketGetCommand::settings:
            hResGetSettings(PacketGetResponseSettings(p_header, p_payload, payloadSize, checksum));
            break;
          case PacketGetCommand::room_temp:
            hResGetRoomTemp(PacketGetResponseRoomTemp(p_header, p_payload, payloadSize, checksum));
            break;
          case PacketGetCommand::four:
            hResGetFour(Packet(p_header, p_payload, payloadSize, checksum));
            break;
          case PacketGetCommand::status:
            hResGetStatus(PacketGetResponseStatus(p_header, p_payload, payloadSize, checksum));
            break;
          case PacketGetCommand::standby:
            hResGetStandby(PacketGetResponseStandby(p_header, p_payload, payloadSize, checksum));
            break;
          default:
            sendPacket(Packet(p_header, p_payload, payloadSize, checksum), tstat_uart, false);
            ESP_LOGI(TAG, "Unknown get response command %x received.", p_payload[0]);
        }
        break;

      case PacketType::connect_request:
        hReqConnect(PacketConnectRequest(p_header, p_payload, payloadSize, checksum));
        break;
      case PacketType::extended_connect_request:
        hReqExtendedConnect(PacketExtendedConnectRequest(p_header, p_payload, payloadSize, checksum));
        break;
      case PacketType::get_request:
        hReqGet(Packet(p_header, p_payload, payloadSize, checksum));
        break;
      default:
        ESP_LOGI(TAG, "Unknown packet type %x received.", p_header[PACKET_HEADER_INDEX_PACKET_TYPE]);
        if (forwarding) {
          if (uart == tstat_uart) {
            sendPacket(Packet(p_header, p_payload, payloadSize, checksum), hp_uart, false);
          } else {
            sendPacket(Packet(p_header, p_payload, payloadSize, checksum), tstat_uart, false);
          }
        }
    }

    return true;
  }

  return false;
}

////
// Response Handlers
////

void MitsubishiUART::hResConnect(PacketConnectResponse packet) {
  // Not sure there's any info in the response.
  connectState = 2;
  ESP_LOGI(TAG, "Connected to heatpump.");
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }
}

void MitsubishiUART::hResExtendedConnect(PacketExtendedConnectResponse packet) {
  // TODO : Don't know what's in these
  connectState = 2;
  ESP_LOGI(TAG, "Connected to heatpump.");
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }
}

void MitsubishiUART::hResGetSettings(PacketGetResponseSettings packet) {
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }

  const bool power = packet.getPower();
  if (power) {
    switch (packet.getMode()) {
      case 0x01:
        this->climate_->mode = climate::CLIMATE_MODE_HEAT;
        break;
      case 0x02:
        this->climate_->mode = climate::CLIMATE_MODE_DRY;
        break;
      case 0x03:
        this->climate_->mode = climate::CLIMATE_MODE_COOL;
        break;
      case 0x07:
        this->climate_->mode = climate::CLIMATE_MODE_FAN_ONLY;
        break;
      case 0x08:
        this->climate_->mode = climate::CLIMATE_MODE_HEAT_COOL;
        break;
      default:
        this->climate_->mode = climate::CLIMATE_MODE_OFF;
    }
  } else {
    this->climate_->mode = climate::CLIMATE_MODE_OFF;
  }

  this->climate_->target_temperature = packet.getTargetTemp();

  switch (packet.getFan()) {
    case 0x00:
      this->climate_->fan_mode = climate::CLIMATE_FAN_AUTO;
      break;
    case 0x01:
      this->climate_->fan_mode = climate::CLIMATE_FAN_QUIET;
      break;
    case 0x02:
      this->climate_->fan_mode = climate::CLIMATE_FAN_LOW;
      break;
    case 0x03:
      this->climate_->fan_mode = climate::CLIMATE_FAN_MIDDLE;
      break;
    case 0x05:
      this->climate_->fan_mode = climate::CLIMATE_FAN_MEDIUM;
      break;
    case 0x06:
      this->climate_->fan_mode = climate::CLIMATE_FAN_HIGH;
      break;
  }

  uint8_t vane = packet.getVane();
  if (vane > 0x05) {
    vane = 0x06;
  }  // "Swing" is 0x07 and there's no 0x06, so the select menu index only goes to 6
  this->select_vane_direction->lazy_publish_state(this->select_vane_direction->traits.get_options().at(vane));

  const uint8_t h_vane = packet.getHorizontalVane();
  ESP_LOGD(TAG, "HVane set to: %x", h_vane);
}

void MitsubishiUART::hResGetRoomTemp(PacketGetResponseRoomTemp packet) {
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }

  this->climate_->current_temperature = packet.getRoomTemp();
  // My current understanding is that the reported internal temperature will always be here
  // even when we're using an external temperature for control.
  this->sensor_internal_temperature->lazy_publish_state(packet.getRoomTemp());
  ESP_LOGD(TAG, "Room temp: %.1f", this->climate_->current_temperature);
}

void MitsubishiUART::hResGetFour(Packet packet) {
  // This one is mysterious, keep an eye on it (might just be a ping?)
  int bytesSum = 0;
  for (int i = 6; i < packet.getLength() - 7; i++) {
    bytesSum +=
        packet.getBytes()[i];  // Sum of interesting payload bytes (i.e. not the first one, it's 4, and not checksum)
  }

  ESP_LOGD(TAG, "Get Four returned sum %d", bytesSum);
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }
}

void MitsubishiUART::hResGetStatus(PacketGetResponseStatus packet) {
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }

  const bool operating = packet.getOperating();
  this->sensor_compressor_frequency->lazy_publish_state(packet.getCompressorFrequency());

  // TODO Simplify this switch (too many redundant ACTION_IDLES)
  switch (this->climate_->mode) {
    case climate::CLIMATE_MODE_HEAT:
      if (operating) {
        this->climate_->action = climate::CLIMATE_ACTION_HEATING;
      } else {
        this->climate_->action = climate::CLIMATE_ACTION_IDLE;
      }
      break;
    case climate::CLIMATE_MODE_COOL:
      if (operating) {
        this->climate_->action = climate::CLIMATE_ACTION_COOLING;
      } else {
        this->climate_->action = climate::CLIMATE_ACTION_IDLE;
      }
      break;
    case climate::CLIMATE_MODE_HEAT_COOL:
      this->climate_->action = climate::CLIMATE_ACTION_IDLE;
      if (operating) {
        if (this->climate_->current_temperature > this->climate_->target_temperature) {
          this->climate_->action = climate::CLIMATE_ACTION_COOLING;
        } else if (this->climate_->current_temperature < this->climate_->target_temperature) {
          this->climate_->action = climate::CLIMATE_ACTION_HEATING;
        }
      }
      break;
    case climate::CLIMATE_MODE_DRY:
      if (operating) {
        this->climate_->action = climate::CLIMATE_ACTION_DRYING;
      } else {
        this->climate_->action = climate::CLIMATE_ACTION_IDLE;
      }
      break;
    case climate::CLIMATE_MODE_FAN_ONLY:
      this->climate_->action = climate::CLIMATE_ACTION_FAN;
      break;
    default:
      this->climate_->action = climate::CLIMATE_ACTION_OFF;
  }

  ESP_LOGD(TAG, "Operating: %s", YESNO(operating));
}
void MitsubishiUART::hResGetStandby(PacketGetResponseStandby packet) {
  if (forwarding) {
    sendPacket(packet, tstat_uart, false);
  }

  // TODO these are a little uncertain
  // 0x04 = pre-heat, 0x08 = standby
  this->sensor_loop_status->lazy_publish_state(packet.getLoopStatus());
  // 1 to 5, lowest to highest power
  this->sensor_stage->lazy_publish_state(packet.getStage());
}

////
//  Handle Requests (received from thermostat)
////
void MitsubishiUART::hReqConnect(PacketConnectRequest packet) {
  if (forwarding) {
    sendPacket(packet, hp_uart, true);
  }
}
void MitsubishiUART::hReqExtendedConnect(PacketExtendedConnectRequest packet) {
  if (forwarding) {
    sendPacket(packet, hp_uart, true);
  }
}
void MitsubishiUART::hReqGet(Packet packet) {
  if (forwarding) {
    sendPacket(packet, hp_uart, true);
  }
}

}  // namespace mitsubishi_uart
}  // namespace esphome
