/* PS2 transceiver host PoC - Arduino (ESP32 - HSPI) */

#include <PS2X_lib.h>
#include <si4432.h>
#include <SPI.h>
#include <Wire.h>

/* pin config */
#define RADIO_NSEL            15
#define RADIO_SDN             25 // TODO: use MCP100/101 for POR
#define BTN_PROG              39

#define EEP_ADDR              0x50
#define EEP_WR_DELAY          10

// #define ENABLE_EEPROM_PROGRAMMING

Si4432 radio(RADIO_NSEL, RADIO_SDN, -1);
PS2X ps2;

/* TODO: switch to SPI EEPROM config */
uint8_t radio_config[] = {
  0x89, 0x3C, 0x02,  // 0x1C-1E
  0xAB, 0x00, 0xBF, 0x26, 0x00, 0xB4,  // 0x20-25
  0xFF,  // 0x2A
  0xAD,  // 0x30
  0x0F, 0x66, 0x08, 0x38, 0x2D, 0xD4, 0x00, 0x00, 0x55, 0xAA, 0x55, 0xAA,  // 0x32-3D
  0x55, 0xAA, 0x55, 0xAA, 0xFF, 0xFF, 0xFF, 0xFF,  // 0x3F-46
  0x60,  // 0x69
  0x1F, 0x11, 0xEC, 0x00, 0x23, 0xF0,  // 0x6D-72
  0x53, 0x4A, 0xFF,  // 0x75-77
  0x03, 0x32 // 0x79-7A
};

#define PKT_TIMEOUT             50 // packet timeout duration

uint8_t trx_buf[64], trx_len; // radio packet buffer and length
uint16_t pktid = 0xFFFF; // will increment to 0 on first run

void send_packet() {
  pktid = (pktid + 1) & 0x7FFF;
  trx_buf[0] = (uint8_t) (pktid & 0xFF);
  trx_buf[1] = (uint8_t) (pktid >> 8);
  for(uint8_t i = 0; i < 3; i++) {
    if(radio.sendPacket(trx_len, trx_buf) == true) {
      radio.startListening(); // switch back to listening now
      return;
    }
  }
  Serial.println(F("FATAL: Radio packet transmission failed"));
}

uint8_t rssi;

bool wait_packet() {
  uint32_t t_start = millis();
  while(millis() - t_start < PKT_TIMEOUT) {
    if(radio.isPacketReceived()) {
      rssi = radio.getPacketReceived(&trx_len, trx_buf);
      uint16_t id = trx_buf[0] | (trx_buf[1] << 8);
      if((id & (1 << 15)) || (pktid == id)) return true; // accept packet if ID indicates error or matches packet ID
      else Serial.printf_P(PSTR("WARN: Packet ID mismatch (expected 0x%04X, got 0x%04X)\n"), pktid, id);
    }
  }
  Serial.println(F("WARN: Timed out waiting for response packet"));
  return false;
}

SPIClass* hspi = NULL;

#ifdef ENABLE_EEPROM_PROGRAMMING
bool program_eeprom(uint8_t addr, uint8_t len, uint8_t* data) {
  uint8_t wrb = 0;

  /* program */
  Wire.beginTransmission(EEP_ADDR);
  Wire.write(addr); // initial address
  for(uint8_t i = 0; i < len; i++, wrb++) {
    if(wrb == 8 || (i != 0 && (addr + i) % 8 == 0)) {
      /* page boundary/page writing limit reached */
      Wire.endTransmission();
      delay(EEP_WR_DELAY);
      Wire.beginTransmission(EEP_ADDR);
      Wire.write(addr + i);
      wrb = 0;
    }
    Wire.write(data[i]);
  }
  Wire.endTransmission();
  delay(EEP_WR_DELAY);

  /* verify */
  Wire.beginTransmission(EEP_ADDR);
  Wire.write(addr); // initial address for reading
  Wire.endTransmission();
  for(uint8_t i = 0; i < len; i++) {
    if(i % 32 == 0) Wire.requestFrom(EEP_ADDR, ((len - i) >= 32) ? 32 : (len - i), true);
    uint8_t b = Wire.read();
    if(b != data[i]) {
      Serial.printf_P(PSTR("ERROR: EEPROM verify failed at addr 0x%02X (%02X vs %02X)\n"), addr + i, b, data[i]);
      return false;
    }
  }
  
  return true; // ok
}
#endif

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  
  Serial.println(F("itsmevjnk's PS2 transceiver host PoC\n"));

  Wire.begin();
  Serial.print(F("Detecting EEPROM"));
  while(1) {
    Serial.print('.');
    Wire.beginTransmission(EEP_ADDR);
    if(Wire.endTransmission() == 0) {
      Serial.println(F("done."));
      break;
    } else delay(500);
  }

#ifdef ENABLE_EEPROM_PROGRAMMING
  pinMode(BTN_PROG, INPUT_PULLUP);
  if(digitalRead(BTN_PROG) == LOW) {
    /* EEPROM programming requested */
    Serial.println(F("BTN_PROG held low - EEPROM programming requested.\nOn each READY line, please enter a continuous HEX string of up to 16 bytes of the new EEPROM contents, followed by a newline character via serial.\nEnd EEPROM programming by setting BTN_PROG high, then sending a newline character.\nPlease note that up to 256 bytes of the EEPROM may be programmed in total."));
    
    uint16_t address = 0;
    
    while(address <= 0xFF && digitalRead(BTN_PROG) == LOW) {
      uint8_t i = 0;
      bool hinib = true; // high nibble
      Serial.printf_P(PSTR("READY (addr: 0x%02X)\n"), address);
      while(address + i <= 0xFF && i <= 15) {
        if(Serial.available() > 0) {
          char c = tolower(Serial.read());
          if(c == '\n') break;
          uint8_t nib = 0;
          if(c >= '0' && c <= '9') nib = c - '0';
          else if(c >= 'a' && c <= 'f') nib = 10 + (c - 'a');
          if(hinib) trx_buf[i] = nib << 4;
          else {
            trx_buf[i] |= nib;
            i++;
          }
          hinib = !hinib;
        }
      }
      if(!hinib) {
        trx_buf[i] >>= 4; // move back last nibble to low
        i++;
      }
      if(Serial.available() > 0 && Serial.peek() == '\n') Serial.read(); // remove newline character

      /* write */
      if(i != 0) {
        if(program_eeprom(address, i, trx_buf) == true) address += i;
        else Serial.println(F("FATAL: EEPROM data verification failed, please reprogram the last data chunk."));
      }
    }
  
    Serial.printf_P(PSTR("%d byte(s) written\n"), address);
  }
#endif

  Wire.beginTransmission(EEP_ADDR);
  Wire.write(0); // read from beginning
  Wire.endTransmission();
  Serial.print(F("Radio configuration data as loaded:"));
  for(uint8_t i = 0; i < 43; i++) {
    if(i % 32 == 0) Wire.requestFrom(EEP_ADDR, (i == 0) ? 32 : (43 - i), true);
    radio_config[i] = Wire.read();
    if(i % 8 == 0) {
      Serial.printf_P(PSTR("\n%02X: "), i);
    }
    Serial.printf_P(PSTR("%02X "), radio_config[i]);
  }
  Serial.println();
  
  Serial.print(F("Initializing radio"));
  hspi = new SPIClass(HSPI);
  while(1) {
    Serial.print('.');
    if(radio.init(hspi, radio_config)) {
      Serial.println(F("done."));
      radio.startListening(); // why?
      break;
    } else delay(500);
  }

  Serial.print(F("Retrieving transceiver information"));
  while(1) {
    Serial.print('.');
    trx_len = 3;
    trx_buf[2] = 0x03; // refer to transceiver sketch for details
    send_packet();
    if(wait_packet() == true) {
      if(trx_buf[2] != 0x03) {
        Serial.printf_P(PSTR("FATAL: Unexpected response 0x%02X (ID 0x%02X%02X)\n"), trx_buf[2], trx_buf[1], trx_buf[0]);
        continue;
      }
      Serial.printf_P(PSTR("done (pkt length: %d).\nTransceiver info string: %s\n"), trx_len, &trx_buf[3]);
      break;
    }
  }
}

uint32_t n_resp = 0, n_pkt = 0;
bool first_pkt = false;

bool prog_on = false;

uint32_t t_drop = 0;
uint32_t t_drop_min = 0, t_drop_max = 0;
uint64_t t_drop_total = 0;
uint32_t n_drop = 0;

void loop() {
#ifdef ENABLE_EEPROM_PROGRAMMING
  if(digitalRead(BTN_PROG) == LOW) {
    /* config copy */
    if(!prog_on) {
      prog_on = true;
      Wire.beginTransmission(EEP_ADDR);
      if(Wire.endTransmission() != 0) {
        Serial.println(F("ERROR: No target EEPROM detected for copying."));
      } else {
        Serial.print(F("Copying radio configuration to EEPROM..."));
        if(program_eeprom(0, 43, radio_config) == true) Serial.println(F("done.")); else Serial.println(F("failed."));
      }
    }
  } else prog_on = false;
#endif
  
  Serial.print(F("Polling PS2 controller"));
  uint32_t t_poll_total = millis(), t_poll;
  bool resend = false; // set to request packet resend instead of polling again
  while(1) {
    Serial.print('.');
    trx_len = (resend) ? 3 : 5;
    trx_buf[2] = (resend) ? 0x01 : 0x02;
    trx_buf[3] = 0; trx_buf[4] = 0;
    t_poll = millis();
    send_packet();
    n_pkt++;
    if(wait_packet() == true) {
      if(trx_buf[2] == 0x01) break;
      Serial.print(trx_buf[2], HEX); // print error response
      if(trx_buf[2] != 0xF2) resend = true; // not PS2 error, so we want the packet resent instead of polled again
    } else if(t_drop == 0) t_drop = millis();
  }
  if(t_drop) {
    t_drop = millis() - t_drop;
    t_drop_total += t_drop;
    n_drop++;
    if(t_drop_min == 0 || t_drop < t_drop_min) t_drop_min = t_drop;
    if(t_drop_max == 0 || t_drop > t_drop_max) t_drop_max = t_drop;
  }
  n_resp++;
  Serial.printf_P(PSTR("done (round-trip %u ms, %u ms total, RSSI %u)\nPS2 packet: "), millis() - t_poll, millis() - t_poll_total, rssi);
  for(uint8_t i = 0; i < 18; i++) Serial.printf_P(PSTR("%02X "), trx_buf[3 + i]);
  Serial.printf_P(PSTR("\nSuccessful polls: %u, out of %u sent packets (success rate %f%%)\n"), n_resp, n_pkt, 100*(float)n_resp/(float)n_pkt);
  if(t_drop) {
    Serial.printf_P(PSTR("Signal dropout duration: %d ms (min %d ms, max %d ms, avg %d ms)\n\n"), t_drop, t_drop_min, t_drop_max, t_drop_total / n_drop);
    t_drop = 0;
  } else if(n_drop != 0) Serial.printf_P(PSTR("Signal dropout stats: min %d ms, max %d ms, avg %d ms\n\n"), t_drop_min, t_drop_max, t_drop_total / n_drop);
  else Serial.printf_P(PSTR("\n\n"));
  delay(10);
}
