/* Arduino Pro Mini - ATmega328, 8MHz, 3.3V */

#include <SPI.h>
#include <Wire.h>
#include <ssd1306_mf.h>
#include <stdio.h> // for printf - best invention ever

#include <EEPROM.h>

#include <PS2X_lib.h>
#include <si4432.h>

#define PS2_DAT                 A0
#define PS2_CMD                 A1
#define PS2_ATT                 A2
#define PS2_CLK                 A3

#define RAD_NSEL                10
#define RAD_SDN                 7
#define RAD_NIRQ                2

#define BTN_UP                  3
#define BTN_DOWN                4
#define BTN_MODE                5
#define BTN_OK                  6

#define EEP_ADDR                0x50
#define EEP_WR_DELAY            10

#define DEBOUNCE                25

SSD1306 oled;

PS2X ps2;
Si4432 radio(RAD_NSEL, RAD_SDN, RAD_NIRQ);

int stdout_write(char c, FILE* stream) {
  oled.write(c);
}

void chklist_ok() {
  oled.tty_x = SSD1306_TEXT_WIDTH - 2;
  oled.text_invert = true; printf_P(PSTR("OK")); oled.text_invert = false;
  oled.update();
}

char chklist_wait_char[] = {'-', '\\', '|', '/'};
uint8_t chklist_wait_n = 0;
void chklist_wait() {
  oled.print_char(oled.tty_x, oled.tty_y, chklist_wait_char[chklist_wait_n++]);
  //uint32_t t = millis();
  oled.update();
  //Serial.println(millis() - t, DEC);
  if(chklist_wait_n >= 4) chklist_wait_n = 0;
}

uint8_t radio_config[43];

void prog_print_nibble(uint8_t i_byte, bool i_nib) {
  oled.tty_x = (i_byte % 8) * 3 + ((i_nib) ? 1 : 0);
  oled.tty_y = 1 + i_byte / 8;
  printf_P(PSTR("%X"), (!i_nib) ? (radio_config[i_byte] >> 4) : (radio_config[i_byte] & 0x0F));
  oled.update();
}

#define ENABLE_PRESSURE_EMULATION // enable if you absolutely need pressure readings
#define ENABLE_JOYSTICK_CORRECTION // enable to correct joystick drift, needed for most wired controllers

#ifdef ENABLE_PRESSURE_EMULATION
bool ps2_emulate_pressure = false;
#endif

/* joystick center config */
#ifdef ENABLE_JOYSTICK_CORRECTION
#define STICK_RX_CENTER       0x7B
#define STICK_RY_CENTER       0x7B
#define STICK_LX_CENTER       0x7B
#define STICK_LY_CENTER       0x7B
#endif

// #define ACTY_OLED

void acty_on() {
#ifdef ACTY_OLED
  oled.print_char(SSD1306_TEXT_WIDTH - 1, 0, 'A', true, true);
  oled.update();
#endif
}

void acty_off() {
#ifdef ACTY_OLED
  oled.print_char(SSD1306_TEXT_WIDTH - 1, 0, 'A', false, true);
  oled.update();
#endif
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);

  stdout = fdevopen(&stdout_write, NULL);

  Wire.begin();
  Wire.setClock(400000);
  oled.init(); oled.fill(); oled.update();

  /* MODE on boot = EEPROM programming mode */
  EEPROM.get(0, radio_config);
  if(digitalRead(BTN_MODE) == LOW) {
    printf_P(PSTR("EEPROM PROGRAMMING\nUP   - auto prog.\nDOWN - manual prog.\n")); oled.update();
    while(digitalRead(BTN_UP) == HIGH && digitalRead(BTN_DOWN) == HIGH);
    oled.fill_page(1); oled.fill_page(2); oled.tty_y = 1;
    if(digitalRead(BTN_UP) == LOW) {
      /* auto programming via serial */
      printf_P(PSTR("Follow the instructions outputted via serial monitor."));
      while(1);
    } else {
      /* manual programming */
      delay(DEBOUNCE); while(digitalRead(BTN_DOWN) == LOW);
      for(uint8_t i = 0; i < 43; i++) {
        if(i != 0 && i % 8 == 0) oled.println();
        printf_P(PSTR("%02X "), radio_config[i]);
      }
      printf_P(PSTR("\nMODE:Exit OK:Next"));
      oled.update();
      uint8_t i_byte = 0; bool i_nib = false;
      while(digitalRead(BTN_MODE) == HIGH) {
        oled.text_invert = true;
        prog_print_nibble(i_byte, i_nib);
        while(digitalRead(BTN_UP) == HIGH && digitalRead(BTN_DOWN) == HIGH && digitalRead(BTN_MODE) == HIGH && digitalRead(BTN_OK) == HIGH);
        uint8_t nib = (!i_nib) ? (radio_config[i_byte] >> 4) : (radio_config[i_byte] & 0x0F);
        bool nib_changed = false;
        if(digitalRead(BTN_UP) == LOW) {
          delay(DEBOUNCE); while(digitalRead(BTN_UP) == LOW);
          if(nib == 0xF) nib = 0;
          else nib++;
          nib_changed = true;
        }
        if(digitalRead(BTN_DOWN) == LOW) {
          delay(DEBOUNCE); while(digitalRead(BTN_DOWN) == LOW);
          if(nib == 0x0) nib = 0xF;
          else nib--;
          nib_changed = true;
        }
        if(nib_changed) {
        if(i_nib) radio_config[i_byte] = (radio_config[i_byte] & 0xF0) | nib;
        else radio_config[i_byte] = (radio_config[i_byte] & 0x0F) | (nib << 4);
          Serial.print(i_byte, DEC); Serial.print(':'); Serial.println(radio_config[i_byte], HEX);
          prog_print_nibble(i_byte, i_nib);
        }
        if(digitalRead(BTN_OK) == LOW) {
          delay(DEBOUNCE); while(digitalRead(BTN_OK) == LOW);
          oled.text_invert = false;
          prog_print_nibble(i_byte, i_nib);
          i_nib = !i_nib;
          if(i_nib == false) {
            i_byte++;
            if(i_byte >= 43) i_byte = 0;
          }
        }
      }
    }

    EEPROM.put(0, radio_config);
    EEPROM.get(0, radio_config); // read back
    
    oled.fill();
    oled.text_invert = false;
    printf_P(PSTR("EEPROM PROGRAMMED\n"));
    for(uint8_t i = 0; i < 43; i++) {
      if(i != 0 && i % 8 == 0) oled.println();
      printf_P(PSTR("%02X "), radio_config[i]);
    }
    printf_P(PSTR("\nPress OK to continue"));
    oled.update();
    while(digitalRead(BTN_OK) == HIGH);
    oled.fill();
  }
  printf_P(PSTR("INIT CHECKLIST\n")); oled.update();

  printf_P(PSTR("PS2 controller"));
  while(1) {
    chklist_wait();
    byte ret = ps2.config_gamepad(PS2_CLK, PS2_CMD, PS2_ATT, PS2_DAT, true, true);
    uint8_t tx = oled.tty_x;
    oled.tty_x += 2;
    printf_P(PSTR("(%d)"), ret);
    oled.tty_x = tx;
    if(ret == 0 || ret == 3) {
#ifdef ENABLE_PRESSURE_EMULATION
      if(ret == 3) ps2_emulate_pressure = true;
#endif
      chklist_ok();
      break;
    }
  }

  printf_P(PSTR("Radio (ch.%02X)"), radio_config[41]);
  while(1) {
    chklist_wait();
    if(radio.init(&SPI, radio_config)) {
      radio.startListening();
      chklist_ok();
      break;
    }
  }

  oled.tty_x = 0; oled.tty_y = 0; oled.fill_page(0);
  printf_P(PSTR("PS2 TRANSCEIVER")); oled.update();
  acty_off();
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
uint8_t trx_buf[64], trx_len = 0;

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

void loop() {
  // put your main code here, to run repeatedly:
  if(digitalRead(BTN_MODE) == LOW) {
    oled.tty_x = 0; oled.tty_y = SSD1306_TEXT_HEIGHT - 1;
    oled.fill_page(7);
    Wire.beginTransmission(EEP_ADDR);
    if(Wire.endTransmission() != 0) {
      printf_P(PSTR("No I2C EEPROM detected"));
    } else {
      Wire.beginTransmission(EEP_ADDR);
      Wire.write(0);
      uint8_t n = 1;
      for(uint8_t i = 0; i < sizeof(radio_config); i++) {
        if(n == 32 || (i != 0 && i % 8 == 0)) {
          Wire.endTransmission();
          delay(EEP_WR_DELAY);
          Wire.beginTransmission(EEP_ADDR);
          Wire.write(i);
          n = 1;
        }
        Wire.write(radio_config[i]);
        n++;
      }
      Wire.endTransmission();
      delay(EEP_WR_DELAY);
      printf_P(PSTR("I2C EEPROM programmed"));
    }
    oled.update();
    while(digitalRead(BTN_MODE) == LOW);
  }
  if(radio.isPacketReceived()) {
    acty_on();
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
            trx_buf[2] = 0xF2;
            trx_len = 3;
          } else {
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
        if(radio.sendPacket(trx_len, trx_buf) == true) break;
      }
      
      if(i == 3) {
        oled.tty_x = 0; oled.tty_y = 0; oled.text_invert = true;
        printf_P(PSTR("RADIO FAILURE"));
        while(1);
      }
    }
    
    radio.startListening();
    acty_off();
  }
}
