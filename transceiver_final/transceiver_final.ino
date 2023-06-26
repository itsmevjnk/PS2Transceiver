/* Arduino Pro Mini - ATmega328, 8MHz, 3.3V */

#include <SPI.h>
#include <Wire.h>
#include <ssd1306_mf.h>
#include <stdio.h> // for printf - best invention ever

#include <EEPROM.h>

#include <PS2X_lib.h>
#include <RF24.h>

#include <avr/pgmspace.h>

#define PS2_DAT                 A3
#define PS2_CMD                 A2
#define PS2_ATT                 A1
#define PS2_CLK                 A0

#define RAD_CSN                 10

#define BTN_UP                  7
#define BTN_DOWN                6
#define BTN_MODE                9
#define BTN_OK                  8

#define EEP_ADDR                0x50
#define EEP_WR_DELAY            10

#define DEBOUNCE                25

#define EEP_ADDR_CHANNEL        0
#define EEP_ADDR_DRATE          1
#define EEP_ADDR_RAPRE          2

#define LED_ACTY                5

SSD1306 oled;

PS2X ps2;
RF24 radio(RAD_CSN, RAD_CSN);

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

/* radio settings override */
//#define RADIO_CHANNEL_OVR 40
//#define RADIO_RATE_OVR    1
//#define RADIO_ADDR_OVR    '\0', 'R', 'B', '0', '1'

uint8_t radio_channel = 0, radio_rate = 0;
uint8_t radio_addr_robot[5], radio_addr_trx[5];

/*
void prog_print_nibble(uint8_t i_byte, bool i_nib) {
  oled.tty_x = (i_byte % 8) * 3 + ((i_nib) ? 1 : 0);
  oled.tty_y = 1 + i_byte / 8;
  printf_P(PSTR("%X"), (!i_nib) ? (radio_config[i_byte] >> 4) : (radio_config[i_byte] & 0x0F));
  oled.update();
}
*/

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
#if defined(ACTY_OLED)
  oled.print_char(SSD1306_TEXT_WIDTH - 1, 0, 'A', true, true);
  oled.update();
#endif
#if defined(LED_ACTY)
  digitalWrite(LED_ACTY, HIGH);
#endif
}

void acty_off() {
#if defined(ACTY_OLED)
  oled.print_char(SSD1306_TEXT_WIDTH - 1, 0, 'A', false, true);
  oled.update();
#endif
#if defined(LED_ACTY)
  digitalWrite(LED_ACTY, LOW);
#endif
}

/* GUI loop */
uint8_t disp_mode = 0; // 0 = menu, 1 = radio config, 2 = copy to EEPROM, 3 = copy from EEPROM
bool disp_update = false; // set to update display by the end of disp_loop()
uint8_t buttons = 0; // lower 4 bits = current buttons (menu, up, down, ok), upper 4 bits = last

// #define BUTTONS_DEBUG

/* menu */
bool menu_redraw = true;
uint8_t menu_sel = 1; // 1-3 corresponding to disp_mode
bool menu_switch = false; // set when a mode has been selected

void menu_loop() {
  uint8_t last_sel = (menu_redraw) ? 255 : menu_sel;
  
  if(menu_redraw) {
    for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
    oled.tty_x = 0; oled.tty_y = 1;
    printf_P(PSTR("  Radio config\n  Export config\n  Import config"));
    menu_redraw = false; disp_update = true;
  }

  if((buttons & (1 << 1)) && !(buttons & (1 << 5)) && menu_sel > 1) menu_sel--;
  if((buttons & (1 << 2)) && !(buttons & (1 << 6)) && menu_sel < 3) menu_sel++;

  if((buttons & (1 << 3)) && !(buttons & (1 << 7))) {
    disp_mode = menu_sel;
    menu_switch = true;
  }
  else if(last_sel != menu_sel) {
    if(last_sel != 255) {
      oled.tty_x = 0; oled.tty_y = last_sel; putchar(' ');
    }
    oled.tty_x = 0; oled.tty_y = menu_sel; putchar('>');
    disp_update = true;
  }
}

/* radio config */
uint8_t config_sel = 0; // 0 = radio channel, 1 = data rate, 2 = address prefix
uint8_t config_sel_sub = 255; // 255 = scrolling through selections, 0-254 = config sub-selection

char config_radio_rates[3][5] = {"  1M", "  2M", "250k"};

void config_print(uint8_t sel, uint8_t sub) {
  uint8_t i;
  switch(sel) {
    case 0: // channel
      if(sub != 255) oled.text_invert = true;
      oled.tty_x = SSD1306_TEXT_WIDTH - 3; oled.tty_y = 1;
      printf_P(PSTR("%3d"), radio_channel);
      if(sub != 255) oled.text_invert = false;
      break;
    case 1: // data rate
      if(sub != 255) oled.text_invert = true;
      oled.tty_x = SSD1306_TEXT_WIDTH - 4; oled.tty_y = 2;
      printf(config_radio_rates[radio_rate]);
      if(sub != 255) oled.text_invert = false;
      break;
    case 2: // address prefix
      oled.tty_x = SSD1306_TEXT_WIDTH - 8; oled.tty_y = 3;
      printf_P(PSTR("%02X%02X%02X%02X"), radio_addr_robot[1], radio_addr_robot[2], radio_addr_robot[3], radio_addr_robot[4]);
      if(sub != 255) {
        oled.tty_x = SSD1306_TEXT_WIDTH - 8 + sub; oled.tty_y = 3;
        oled.text_invert = true;
        printf_P(PSTR("%X"), (sub % 2 == 0) ? (radio_addr_robot[sub / 2 + 1] >> 4) : (radio_addr_robot[sub / 2 + 1] & 0x0F));
        oled.text_invert = false;
      }
      break;
  }
}

void config_loop() {
  uint8_t last_sel = (menu_switch) ? 255 : config_sel;

//  Serial.print(radio_addr_robot[0], HEX); Serial.print(' ');
//  Serial.print(radio_addr_robot[1], HEX); Serial.print(' ');
//  Serial.print(radio_addr_robot[2], HEX); Serial.print(' ');
//  Serial.print(radio_addr_robot[3], HEX); Serial.print(' ');
//  Serial.println(radio_addr_robot[4], HEX);
  
  if(menu_switch) {
    config_sel = 0; config_sel_sub = 255;
    for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
    oled.tty_x = 0; oled.tty_y = 1;
    printf_P(PSTR("  Channel:\n  Data rate:\n  Addr. pre.:"));
    config_print(0, 255);
    config_print(1, 255);
    config_print(2, 255);    
    disp_update = true; menu_switch = false;
//    radio_channel_old = radio_channel;
//    radio_rate_old = radio_rate;
//    for(uint8_t i = 1; i < 5; i++) radio_addr_old[i - 1] = radio_addr_robot[i];
  }

  bool reprint = false;

  if((buttons & (1 << 3)) && !(buttons & (1 << 7))) {
    /* OK */
    if(config_sel == 2) {
      switch(config_sel_sub) {
        case 7: config_sel_sub = 255; break;
        case 255: config_sel_sub = 0; break;
        default: config_sel_sub++; break;
      }
    } else config_sel_sub = (config_sel_sub == 255) ? 0 : 255;
    uint8_t i;
    switch(config_sel) {
      case 0: EEPROM.update(EEP_ADDR_CHANNEL, radio_channel); break;
      case 1: EEPROM.update(EEP_ADDR_DRATE, radio_rate); break;
      case 2: for(i = 0; i < 4; i++) EEPROM.update(EEP_ADDR_RAPRE + i, radio_addr_robot[i + 1]); break;
    }
    reprint = true;
  }
  
  if((buttons & (1 << 1)) && !(buttons & (1 << 5))) {
    /* UP */
    if(config_sel_sub == 255) {
      if(config_sel > 0) config_sel--;
    } else {
      uint8_t t;
      switch(config_sel) {
        case 0: 
          if(radio_channel < 125) {
            radio_channel++;
            reprint = true;
            radio.setChannel(radio_channel);
          }
          break;
        case 1:
          if(radio_rate < 2) {
            radio_rate++;
            reprint = true;
            radio.setDataRate((rf24_datarate_e) radio_rate);
          }
          break;
        case 2:
          t = (config_sel_sub % 2 == 0) ? (radio_addr_robot[1 + config_sel_sub / 2] >> 4) : (radio_addr_robot[1 + config_sel_sub / 2] & 0x0F);
          if(t < 0x0F) {
            t++;
            if(config_sel_sub % 2) radio_addr_robot[1 + config_sel_sub / 2] = (radio_addr_robot[1 + config_sel_sub / 2] & 0xF0) | t;
            else radio_addr_robot[1 + config_sel_sub / 2] = (radio_addr_robot[1 + config_sel_sub / 2] & 0x0F) | (t << 4);
            reprint = true;
          }
          break;
      }
    }
  }

  if((buttons & (1 << 2)) && !(buttons & (1 << 6))) {
    /* DOWN */
    if(config_sel_sub == 255) {
      if(config_sel < 2) config_sel++;
    } else {
      uint8_t t;
      switch(config_sel) {
        case 0: 
          if(radio_channel > 0) {
            radio_channel--;
            reprint = true;
            radio.setChannel(radio_channel);
          }
          break;
        case 1:
          if(radio_rate > 0) {
            radio_rate--;
            reprint = true;
            radio.setDataRate((rf24_datarate_e) radio_rate);
          }
          break;
        case 2:
          t = (config_sel_sub % 2 == 0) ? (radio_addr_robot[1 + config_sel_sub / 2] >> 4) : (radio_addr_robot[1 + config_sel_sub / 2] & 0x0F);
          if(t > 0) {
            t--;
            if(config_sel_sub % 2) radio_addr_robot[1 + config_sel_sub / 2] = (radio_addr_robot[1 + config_sel_sub / 2] & 0xF0) | t;
            else radio_addr_robot[1 + config_sel_sub / 2] = (radio_addr_robot[1 + config_sel_sub / 2] & 0x0F) | (t << 4);
            reprint = true;
            memcpy(&radio_addr_trx[1], &radio_addr_robot[1], 4);
            radio.stopListening();
            radio.openWritingPipe(radio_addr_trx);
            radio.closeReadingPipe(1);
            radio.openReadingPipe(1, radio_addr_robot);
            radio.startListening();
          }
          break;
      }
    }
  }

  if(reprint) {
    config_print(config_sel, config_sel_sub);
    disp_update = true;
  }
  
  if(last_sel != config_sel) {
    if(last_sel != 255) {
      oled.tty_x = 0; oled.tty_y = 1 + last_sel; putchar(' ');
    }
    oled.tty_x = 0; oled.tty_y = 1 + config_sel; putchar('>');
    disp_update = true;
  }
}

/* EEPROM export */
uint8_t export_attempts = 0, export_ok = 0;
uint32_t export_t = 0;

void export_loop() {
  bool export_update = false;
  
  if(menu_switch) {
    export_attempts = 0; export_ok = 0;
    for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
    oled.tty_x = 0; oled.tty_y = 1;
    printf_P(PSTR("Exporting to EEPROM:\n    %02X %02X %02X %02X %02X %02X\nPlug EEPROM and press OK"), radio_channel, radio_rate, radio_addr_robot[1], radio_addr_robot[2], radio_addr_robot[3], radio_addr_robot[4]);
    oled.tty_x = 0; oled.tty_y = 6;
    printf_P(PSTR("Prg'd:         PrgOk:"));
    menu_switch = false; disp_update = true; export_update = true;
  }

  if((buttons & (1 << 3)) && !(buttons & (1 << 7))) {
    /* OK */
    oled.fill_page(7);
    Wire.beginTransmission(EEP_ADDR);
    Wire.write(0);
    Wire.write(radio_channel);
    Wire.write(radio_rate);
    for(uint8_t i = 0; i < 4; i++) Wire.write(radio_addr_robot[i+1]);
    export_t = 0;
    if(Wire.endTransmission() != 0) {
      oled.tty_x = 0; oled.tty_y = 7;
      printf_P(PSTR("No EEPROM detected"));
      disp_update = true;
    } else {
      export_t = millis();
      export_attempts++;
    }
  }

  if(export_t != 0 && millis() - export_t >= EEP_WR_DELAY) {
    /* programming done, now verify */
    export_t = 0;
    Wire.beginTransmission(EEP_ADDR);
    Wire.write(0);
    Wire.endTransmission();
    Wire.requestFrom(EEP_ADDR, 6, true);
    uint8_t channel = Wire.read();
    uint8_t rate = Wire.read();
    uint8_t addr[4]; for(uint8_t i = 0; i < 4; i++) addr[i] = Wire.read();
    oled.tty_x = 0; oled.tty_y = 7;
    if(channel != radio_channel || rate != radio_rate || memcmp(addr, &radio_addr_robot[1], 4) != 0) printf_P(PSTR("EEPROM verify failed"));
    else {
      export_ok++;
      printf_P(PSTR("EEPROM programmed"));
    }
    export_update = true;
  }
  
  if(export_update) {
    oled.tty_x = 7; oled.tty_y = 6; printf_P(PSTR("%3d"), export_attempts);
    oled.tty_x = 22; printf_P(PSTR("%3d"), export_ok);
    disp_update = true;
  }
}

/* EEPROM import */
bool import_read = false, import_done = false;
uint8_t import_channel, import_rate, import_addr[4];

void import_loop() {
  if(menu_switch) {
    import_read = false; import_done = false;
    for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
    oled.tty_x = 0; oled.tty_y = 1;
    printf_P(PSTR("Plug EEPROM and press OK"));
    disp_update = true;
    menu_switch = false;
  }

  if(!import_read) {
    if((buttons & (1 << 3)) && !(buttons & (1 << 7))) {
      /* OK */
      Wire.beginTransmission(EEP_ADDR);
      Wire.write(0);
      if(Wire.endTransmission() != 0) {
        oled.tty_x = 0; oled.tty_y = 7;
        oled.fill_page(7);
        printf_P(PSTR("EEPROM not detected"));
      } else {
        Wire.requestFrom(EEP_ADDR, 6, true);
        import_channel = Wire.read();
        import_rate = Wire.read();
        for(uint8_t i = 0; i < 4; i++) import_addr[i] = Wire.read();
        for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
        oled.tty_x = 0; oled.tty_y = 1;
        printf_P(PSTR("Importing to MCU:\n    %02X %02X %02X %02X %02X %02X\nUP   - Import\nDOWN - Cancel"), import_channel, import_rate, import_addr[1], import_addr[2], import_addr[3], import_addr[4]);
        import_read = true;
      }
      disp_update = true;
    }
  } else if(!import_done) {    
    if((buttons & (1 << 1)) && !(buttons & (1 << 5))) {
      /* UP */
      if(radio_channel != import_channel) {
        radio_channel = import_channel;
        EEPROM.update(EEP_ADDR_CHANNEL, radio_channel);
        radio.setChannel(radio_channel);
      }
      if(radio_rate != import_rate) {
        radio_rate = import_rate;
        EEPROM.update(EEP_ADDR_DRATE, radio_rate);
        radio.setDataRate((rf24_datarate_e) radio_rate);
      }
      if(memcmp(import_addr, &radio_addr_robot[1], 4) != 0) {
        memcpy(&radio_addr_robot[1], import_addr, 4);
        memcpy(&radio_addr_trx[1], import_addr, 4);
        radio.stopListening();
        radio.openWritingPipe(radio_addr_trx);
        radio.closeReadingPipe(1);
        radio.openReadingPipe(1, radio_addr_robot);
        radio.startListening();
      }
      for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
      oled.tty_x = 0; oled.tty_y = 1;
      printf_P(PSTR("Config has been imported\n"));
      
      import_done = true;
    }

    if((buttons & (1 << 2)) && !(buttons & (1 << 6))) {
      /* DOWN */
      for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
      oled.tty_x = 0; oled.tty_y = 1;
      printf_P(PSTR("Cancelled config import\n"));
      
      import_done = true;
    }

    if(import_done) {
      printf_P(PSTR("Press MODE to exit"));
      disp_update = true;
    }
  }
}

void disp_loop() {
  buttons <<= 4;
  buttons |= ((digitalRead(BTN_MODE) == LOW) ? (1 << 0) : 0) | ((digitalRead(BTN_UP) == LOW) ? (1 << 1) : 0) | ((digitalRead(BTN_DOWN) == LOW) ? (1 << 2) : 0) | ((digitalRead(BTN_OK) == LOW) ? (1 << 3) : 0);
#ifdef BUTTONS_DEBUG
  oled.tty_x = SSD1306_TEXT_WIDTH - 2; oled.tty_y = 0;
  printf_P(PSTR("%02X"), buttons);
  oled.update();
#endif

  if((buttons & (1 << 0)) && !(buttons & (1 << 4))) {
    menu_redraw = true;
    menu_sel = 1;
    disp_mode = 0;
  }

  switch(disp_mode) {
    case 0: menu_loop(); break;
    case 1: config_loop(); break;
    case 2: export_loop(); break;
    case 3: import_loop(); break;
    default:
      disp_mode = 0;
      menu_redraw = true;
      menu_sel = 1;
      menu_loop();
      break;
  }
  
  if(disp_update) {
    oled.update();
    disp_update = false;
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);

#ifdef LED_ACTY
  pinMode(LED_ACTY, OUTPUT); digitalWrite(LED_ACTY, HIGH);
#endif

  stdout = fdevopen(&stdout_write, NULL);

  Wire.begin();
  Wire.setClock(400000);
  oled.init(); oled.fill(); oled.update();

#ifndef RADIO_CHANNEL_OVR
  radio_channel = EEPROM.read(EEP_ADDR_CHANNEL);
#else
  radio_channel = RADIO_CHANNEL_OVR;
#endif
#ifndef RADIO_RATE_OVR
  radio_rate = EEPROM.read(EEP_ADDR_DRATE);
#else
  radio_rate = RADIO_RATE_OVR;
#endif
#ifndef RADIO_ADDR_OVR
  uint8_t radio_addr[5];
  for(uint8_t i = 0; i < 4; i++) radio_addr[i + 1] = EEPROM.read(EEP_ADDR_RAPRE + i);
#else
  uint8_t radio_addr[5] = {RADIO_ADDR_OVR};
#endif
  for(uint8_t i = 1; i < 5; i++) {
    radio_addr_robot[i] = radio_addr[i];
    radio_addr_trx[i] = radio_addr[i];
  }
  radio_addr_robot[0] = 'R'; radio_addr_trx[0] = 'T';
  
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

  printf_P(PSTR("Radio (ch.%d)"), radio_channel);
  while(1) {
    chklist_wait();
    if(radio.begin()) {
      radio.setChannel(radio_channel);
      radio.setDataRate((rf24_datarate_e) radio_rate);
      radio.openWritingPipe(radio_addr_trx);
      radio.openReadingPipe(1, radio_addr_robot);
      radio.startListening();
      chklist_ok();
      break;
    }
  }
  
#ifdef LED_ACTY
  digitalWrite(LED_ACTY, LOW);
#endif

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

#define PAYLOAD_DEBUG

bool send_fail = false;

void loop() {
  // put your main code here, to run repeatedly:

  disp_loop();
  
  /*
  if(digitalRead(BTN_MODE) == LOW) {
    oled.tty_x = 0; oled.tty_y = SSD1306_TEXT_HEIGHT - 1;
    oled.fill_page(7);
    Wire.beginTransmission(EEP_ADDR);
    if(Wire.endTransmission() != 0) {
      printf_P(PSTR("No I2C EEPROM detected"));
    } else {
      Wire.beginTransmission(EEP_ADDR);
      Wire.write(0);
      Wire.write(radio_channel);
      Wire.write(radio_rate);
      for(uint8_t i = 0; i < 4; i++) Wire.write(radio_addr_robot[i]);
      Wire.endTransmission();
      delay(EEP_WR_DELAY);
      printf_P(PSTR("I2C EEPROM programmed"));
    }
    oled.update();
    while(digitalRead(BTN_MODE) == LOW);
  }
  */
  uint8_t pipe;
  if(radio.available(&pipe)) {
    acty_on();
    trx_len = radio.getPayloadSize();
    radio.read(trx_buf, trx_len);
    
#ifdef PAYLOAD_DEBUG
    Serial.print(trx_len, DEC);
    Serial.print(" HOST: ");
    for(uint8_t i = 0; i < trx_len; i++) {
      Serial.print(trx_buf[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
#endif

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
      // delay(10);
      radio.stopListening();
#ifdef PAYLOAD_DEBUG
      Serial.print(trx_len, DEC);
      Serial.print(" RESP: ");
      for(uint8_t i = 0; i < trx_len; i++) {
        Serial.print(trx_buf[i], HEX);
        Serial.print(' ');
      }
      Serial.println();
#endif

      if(radio.write(trx_buf, trx_len) == false) {
        send_fail = true;
        oled.tty_x = 0; oled.tty_y = 7; oled.text_invert = true;
        printf_P(PSTR("Resp. failure (ID %04X)"), pktid);
        oled.update();
      } else if(send_fail == true) {
        send_fail = false;
        oled.fill_page(7);
        oled.update();
      }
    }
    
    radio.startListening();
    acty_off();
  }
}
