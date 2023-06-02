/* PS2 transceiver PoC - Arduino */

#include <PS2X_lib.h>
#include <si4432.h>
#include <SPI.h>
#include <Wire.h>

/* pin config */
#ifdef ESP32
#define RADIO_NSEL            15
#define RADIO_SDN             23
#define RADIO_NIRQ            19

#define PS2_ATT               32
#define PS2_CMD               25
#define PS2_DAT               4
#define PS2_CLK               33

#define LED_ACTY              5
#define LED_ERR1              27 // PS2 error
#define LED_ERR2              26 // radio error

#define BTN_PROG              39 // EEPROM write button
#else
#define RADIO_NSEL            10
#define RADIO_SDN             3
#define RADIO_NIRQ            2

#define PS2_ATT               A0
#define PS2_CMD               A1
#define PS2_DAT               A2
#define PS2_CLK               A3

#define LED_ACTY              4
#define LED_ERR1              5 // PS2 error
#define LED_ERR2              6 // radio error

#define BTN_PROG              8 // EEPROM write button
#endif

#define EEP_ADDR              0x50
#define EEP_WR_DELAY          10 // for safety, but according to DS only 5ms is needed

Si4432 radio(RADIO_NSEL, RADIO_SDN, RADIO_NIRQ);
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

uint32_t t_last_pkt; // timestamp of last packet reception

#define PKT_TIMEOUT       0 // packet reception timeout duration (mS) - set to 0 to disable

#define ENABLE_PRESSURE_EMULATION // enable if you absolutely need pressure readings
#define ENABLE_JOYSTICK_CORRECTION // enable to correct joystick drift, needed for most wired controllers

#ifdef ENABLE_PRESSURE_EMULATION
bool pressure_emu = false;
#endif

/* joystick center config */
#ifdef ENABLE_JOYSTICK_CORRECTION
#define STICK_RX_CENTER       0x7B
#define STICK_RY_CENTER       0x7B
#define STICK_LX_CENTER       0x7B
#define STICK_LY_CENTER       0x7B
#endif

uint8_t trx_buf[256]; // for radio mostly, but we can also use this for other things like serial in setup()
uint8_t trx_len; // packet length

bool program_eeprom(uint8_t addr, uint8_t len, uint8_t* data) {
  uint8_t wrb = 0;
  char buf[64];

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
      sprintf_P(buf, PSTR("ERROR: EEPROM verify failed at addr 0x%02X (%02X vs %02X)"), addr + i, b, data[i]);
      Serial.println(buf);
      return false;
    }
  }
  
  return true; // ok
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  pinMode(LED_ACTY, OUTPUT); digitalWrite(LED_ACTY, LOW); // will turn ACTY on upon EEPROM load
  pinMode(LED_ERR1, OUTPUT); digitalWrite(LED_ERR1, HIGH);
  pinMode(LED_ERR2, OUTPUT); digitalWrite(LED_ERR2, HIGH);
#ifdef ESP32
  pinMode(BTN_PROG, INPUT); // BTN_PROG might not have SW enabled pullup
#else
  pinMode(BTN_PROG, INPUT_PULLUP);
#endif
  
  Serial.println(F("itsmevjnk's PS2 transceiver PoC\n"));

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
  
  if(digitalRead(BTN_PROG) == LOW) {
    /* EEPROM programming requested */
    Serial.println(F("BTN_PROG held low - EEPROM programming requested.\nOn each READY line, please enter a continuous HEX string of up to 16 bytes of the new EEPROM contents, followed by a newline character via serial.\nEnd EEPROM programming by setting BTN_PROG high, then sending a newline character.\nPlease note that up to 256 bytes of the EEPROM may be programmed in total."));
    
    uint16_t address = 0;
    
    while(address <= 0xFF && digitalRead(BTN_PROG) == LOW) {
      uint8_t i = 0;
      bool hinib = true; // high nibble
      Serial.print(F("READY (addr: 0x")); if(address < 16) Serial.print('0'); Serial.print(address, HEX); Serial.println(')');
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

      digitalWrite(LED_ACTY, HIGH); // start programming EEPROM

      /* write */
      if(i != 0) {
        if(program_eeprom(address, i, trx_buf) == true) address += i;
        else Serial.println(F("FATAL: EEPROM data verification failed, please reprogram the last data chunk."));
      }
      
      digitalWrite(LED_ACTY, LOW); // programming ends
      
    }
  
    Serial.print(address, DEC); Serial.println(F(" byte(s) written"));
  }
  
  Wire.beginTransmission(EEP_ADDR);
  Wire.write(0); // read from beginning
  Wire.endTransmission();
  Serial.print(F("Radio configuration data as loaded:"));
  for(uint8_t i = 0; i < 43; i++) {
    if(i % 32 == 0) Wire.requestFrom(EEP_ADDR, (i == 0) ? 32 : (43 - i), true);
    radio_config[i] = Wire.read();
    if(i % 8 == 0) {
      Serial.println();
      if(i < 16) Serial.print('0'); Serial.print(i, HEX);
      Serial.print(F(": "));
    }
    if(radio_config[i] < 16) Serial.print('0'); Serial.print(radio_config[i], HEX); Serial.print(' ');
  }
  Serial.println();
  
  digitalWrite(LED_ACTY, HIGH);
    
  Serial.print(F("Initializing PS2 controller"));
  while(1) {
    Serial.print('.');
    byte ret = ps2.config_gamepad(PS2_CLK, PS2_CMD, PS2_ATT, PS2_DAT, true, true);
    if(ret == 0) {
      Serial.println(F("done."));
      break;
    } else if(ret == 3) {
      Serial.println(F("done (no pressure)."));
#ifdef ENABLE_PRESSURE_EMULATION
      pressure_emu = true; // enable pressure emulation
#endif
      break;
    } else {
      Serial.print(ret, DEC);
      delay(500);
    }
  }
  digitalWrite(LED_ERR1, LOW);

  Serial.print(F("Initializing radio"));
#ifdef ESP32
  SPIClass* hspi = new SPIClass(HSPI);
  hspi->begin();
#endif
  while(1) {
    Serial.print('.');
#ifdef ESP32
    if(radio.init(hspi, radio_config)) {
#else
    if(radio.init(&SPI, radio_config)) {
#endif
      Serial.println(F("done."));
      radio.startListening();
      t_last_pkt = millis();
      break;
    } else delay(500);
  }
  digitalWrite(LED_ERR2, LOW);

  t_last_pkt = millis();
  Serial.println(F("All done, awaiting packets from host"));
  digitalWrite(LED_ACTY, LOW);
}

/*  Packet structure:
 *  | IDL | IDH | CRB | Payload (depending on command/response type) |
 *  IDL, IDH: packet ID (set by host on command packet, ranging from 0000h-7FFFh)
 *  CRB: command/response byte (command for host, response for transceiver) - see below
 *  
 *  List of command bytes:
 *  00h: Loopback test - packet will be sent back to host.
 *  01h: Resend last controller packet to host.
 *  02h: Poll PS2 controller and send controller packet back to host. Payload contents:
 *       1st byte: small vibration motor enable (0 = off, non-zero value = on)
 *       2nd byte: large vibration motor intensity (0-255)
 *  03h: Get transceiver information.
 *  
 *  List of response bytes:
 *  00h: Loopback response - exact same packet as 00h command.
 *  01h: PS2 controller packet (response to 01h or 02h) - payload structure follows that of the PS2 controller.
 *  03h: Transceiver information (response to 03h). Payload contains a null-terminated string containing name and version.
 *  F0h: Invalid command - payload gets retransmitted.
 *  F1h: Radio error (typically CRC mismatch) - packet ID will be set to FFFEh.
 *  F2h: PS2 controller error.
 */

uint16_t last_pktid = 0xFFFE; // last packet ID

#ifdef ENABLE_PRESSURE_EMULATION
void emulate_pressure() {
  trx_buf[9] = (trx_buf[3] & (1 << 5)) ? 0 : 0xFF; // right
  trx_buf[10] = (trx_buf[3] & (1 << 7)) ? 0 : 0xFF; // left
  trx_buf[11] = (trx_buf[3] & (1 << 4)) ? 0 : 0xFF; // up
  trx_buf[12] = (trx_buf[3] & (1 << 6)) ? 0 : 0xFF; // down
  trx_buf[13] = (trx_buf[4] & (1 << 4)) ? 0 : 0xFF; // triangle
  trx_buf[14] = (trx_buf[4] & (1 << 5)) ? 0 : 0xFF; // circle
  trx_buf[15] = (trx_buf[4] & (1 << 6)) ? 0 : 0xFF; // cross
  trx_buf[16] = (trx_buf[4] & (1 << 7)) ? 0 : 0xFF; // square
  trx_buf[17] = (trx_buf[4] & (1 << 2)) ? 0 : 0xFF; // l1
  trx_buf[18] = (trx_buf[4] & (1 << 3)) ? 0 : 0xFF; // r1
  trx_buf[19] = (trx_buf[4] & (1 << 0)) ? 0 : 0xFF; // l2
  trx_buf[20] = (trx_buf[4] & (1 << 1)) ? 0 : 0xFF; // r2
}
#endif

#ifdef ENABLE_JOYSTICK_CORRECTION
void correct_stick() {
  if(trx_buf[5] == STICK_RX_CENTER) trx_buf[5] = 0x7F; // 0x7F for X and 0x80 for Y - that's how the wireless controller behaves
  if(trx_buf[6] == STICK_RY_CENTER) trx_buf[6] = 0x80;
  if(trx_buf[7] == STICK_LX_CENTER) trx_buf[7] = 0x7F;
  if(trx_buf[8] == STICK_LY_CENTER) trx_buf[8] = 0x80;
}
#endif

bool prog_on = false;

void loop() {
  // put your main code here, to run repeatedly:
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
  
  if(radio.isPacketReceived()) {
    digitalWrite(LED_ERR2, LOW);
    digitalWrite(LED_ACTY, HIGH);
    radio.getPacketReceived(&trx_len, trx_buf);
    
    uint16_t pktid = trx_buf[0] | (trx_buf[1] << 8);
    if(pktid != last_pktid) {
      // ignore duplicates
      last_pktid = pktid;
      int i;
      // sprintf(str_buf, "Packet length: %d, header: %02X %02X %02X\n", trx_len, trx_buf[0], trx_buf[1], trx_buf[2]); Serial.print((char*)&str_buf);
      switch(trx_buf[2]) {
        case 0x00: // loopback test
          break;
        case 0x02:
          if(ps2.read_gamepad((trx_buf[3] != 0), trx_buf[4]) == false) {
            digitalWrite(LED_ERR1, HIGH);
            trx_buf[2] = 0xF2;
            trx_len = 3;
          } else {
            digitalWrite(LED_ERR1, LOW);
            trx_len = 21;
            trx_buf[2] = 0x01;
#ifdef ENABLE_PRESSURE_EMULATION
            for(i = 3; i < 9; i++) trx_buf[i] = ps2.PS2data[i]; // copy packet over, ignoring pressure data (which is unavailable anyway)
            emulate_pressure();
#else
            for(i = 3; i < 21; i++) trx_buf[i] = ps2.PS2data[i]; // copy packet over
#endif
#ifdef ENABLE_JOYSTICK_CORRECTION
            correct_stick();
#endif
          }
          break;
        case 0x01:
          trx_len = 21;
#ifdef ENABLE_PRESSURE_EMULATION
          for(i = 3; i < 9; i++) trx_buf[i] = ps2.PS2data[i]; // copy packet over, ignoring pressure data (which is unavailable anyway)
          emulate_pressure();
#else
          for(i = 3; i < 21; i++) trx_buf[i] = ps2.PS2data[i]; // copy packet over
#endif
#ifdef ENABLE_JOYSTICK_CORRECTION
          correct_stick();
#endif
          break;
        case 0x03:
          strcpy((char*)&trx_buf[3], "PS2TXRX 0.01 itsmevjnk/RIAN");
          trx_len = 4 + strlen((char*)&trx_buf[3]); // 3 byte header + payload (incl. null termination)
          break;
        default:
          trx_buf[2] = 0xF0;
          break;
      }
  
      for(i = 0; i < 3; i++) {
        if(radio.sendPacket(trx_len, trx_buf) == false) {
          digitalWrite(LED_ERR2, HIGH);
        } else {
          digitalWrite(LED_ERR2, LOW);
          break;
        }
      }
      
      if(i == 3) {
        while(1) {
          Serial.println(F("RADIO FAILURE"));
          digitalWrite(LED_ERR2, HIGH);
          delay(250);
          digitalWrite(LED_ERR2, LOW);
          delay(250);
        }
      }
    }
    
    radio.startListening();
    digitalWrite(LED_ACTY, LOW);
    t_last_pkt = millis();
  }
#if PKT_TIMEOUT != 0
  else if(millis() - t_last_pkt >= PKT_TIMEOUT) {
    digitalWrite(LED_ERR2, HIGH);
    Serial.println(F("PKT TIMEOUT"));
    radio.hardReset();
    radio.startListening();
    t_last_pkt = millis();
  }
#endif
}
