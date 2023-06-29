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
#define EEP_ADDR_PALVL          6

#define LED_ACTY                5
#define LED_ERROR               4

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
//#define RADIO_PALVL_OVR   RF24_PA_MIN

uint8_t radio_channel = 0, radio_rate = 0, radio_palvl = 0;
uint8_t radio_addr_robot[5], radio_addr_trx[5];

bool radio_pvar = false; // set if nRF24L01+ is detected

void radio_post_init() {
  radio.disableAckPayload();
  radio.setAutoAck(false);
  radio.setPALevel(radio_palvl);
  radio.setChannel(radio_channel);
  radio.setDataRate((rf24_datarate_e) radio_rate);
  radio.openWritingPipe(radio_addr_trx);
  radio.openReadingPipe(1, radio_addr_robot);
  radio.startListening();
}

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
uint8_t disp_mode = 0; // 0 = menu, 1 = radio config, 2 = copy to EEPROM, 3 = copy from EEPROM, 4 = spectrum analyzer
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
    printf_P(PSTR("  Radio config\n  Export config\n  Import config\n  Spectrum analyzer\n  Benchmark (WIP)\n  Statistics"));
    menu_redraw = false; disp_update = true;
  }

  if((buttons & (1 << 1)) && !(buttons & (1 << 5)) && menu_sel > 1) menu_sel--;
  if((buttons & (1 << 2)) && !(buttons & (1 << 6)) && menu_sel < 6) menu_sel++;

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
uint8_t config_sel = 0; // 0 = radio channel, 1 = data rate, 2 = address prefix, 3 = PA level
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
    case 3: // PA level
      if(sub != 255) oled.text_invert = true;
      oled.tty_x = SSD1306_TEXT_WIDTH - 1; oled.tty_y = 4;
      printf_P(PSTR("%d"), radio_palvl);
      if(sub != 255) oled.text_invert = false;
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
    printf_P(PSTR("  Channel:\n  Data rate:\n  Addr. pre.:\n  PA level:"));
    config_print(0, 255);
    config_print(1, 255);
    config_print(2, 255);    
    config_print(3, 255);
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
    if(config_sel_sub == 255) {
      /* save config */
      switch(config_sel) {
        case 0: EEPROM.update(EEP_ADDR_CHANNEL, radio_channel); break;
        case 1: EEPROM.update(EEP_ADDR_DRATE, radio_rate); break;
        case 2: for(i = 0; i < 4; i++) EEPROM.update(EEP_ADDR_RAPRE + i, radio_addr_robot[i + 1]); break;
        case 3: EEPROM.update(EEP_ADDR_PALVL, radio_palvl); break;
      }
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
        case 3:
          if(radio_palvl < 3) {
            radio_palvl++;
            reprint = true;
            radio.setPALevel(radio_palvl);
          }
          break;
      }
    }
  }

  if((buttons & (1 << 2)) && !(buttons & (1 << 6))) {
    /* DOWN */
    if(config_sel_sub == 255) {
      if(config_sel < 3) config_sel++;
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
        case 3:
          if(radio_palvl > 0) {
            radio_palvl--;
            reprint = true;
            radio.setPALevel(radio_palvl);
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
    printf_P(PSTR("Exporting to EEPROM:\n  %02X %02X %02X %02X %02X %02X %02X\nPlug EEPROM and press OK"), radio_channel, radio_rate, radio_addr_robot[1], radio_addr_robot[2], radio_addr_robot[3], radio_addr_robot[4], radio_palvl);
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
    Wire.write(radio_palvl);
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
    Wire.requestFrom(EEP_ADDR, 7, true);
    uint8_t channel = Wire.read();
    uint8_t rate = Wire.read();
    uint8_t addr[4]; for(uint8_t i = 0; i < 4; i++) addr[i] = Wire.read();
    uint8_t palvl = Wire.read();
    oled.tty_x = 0; oled.tty_y = 7;
    if(channel != radio_channel || rate != radio_rate || memcmp(addr, &radio_addr_robot[1], 4) != 0 || palvl != radio_palvl) printf_P(PSTR("EEPROM verify failed"));
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
uint8_t import_channel, import_rate, import_addr[4], import_palvl;

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
        Wire.requestFrom(EEP_ADDR, 7, true);
        import_channel = Wire.read();
        import_rate = Wire.read();
        for(uint8_t i = 0; i < 4; i++) import_addr[i] = Wire.read();
        import_palvl = Wire.read();
        for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
        oled.tty_x = 0; oled.tty_y = 1;
        printf_P(PSTR("Importing to MCU:\n  %02X %02X %02X %02X %02X %02X %02X\nUP   - Import\nDOWN - Cancel"), import_channel, import_rate, import_addr[1], import_addr[2], import_addr[3], import_addr[4], import_palvl);
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
        for(uint8_t i = 0; i < 4; i++) EEPROM.update(EEP_ADDR_RAPRE + i, import_addr[i]);
        radio.stopListening();
        radio.openWritingPipe(radio_addr_trx);
        radio.closeReadingPipe(1);
        radio.openReadingPipe(1, radio_addr_robot);
        radio.startListening();
      }
      if(radio_palvl != import_palvl) {
        radio_palvl = import_palvl;
        EEPROM.update(EEP_ADDR_PALVL, radio_palvl);
        radio.setPALevel(radio_palvl);
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

/* spectrum analyzer */
#define SPEC_GLITCH_STREAK        8 // minimum streak length to be considered a bug

bool trx_halt = false; // set to halt normal TRX operations

uint8_t spec_channel = 0, spec_strength = 0;
uint32_t spec_n = 0;
uint8_t spec_mode = 0; // 0 = accumulative (ACC), 1 = sliding window (SLW), 2 = waterfall (WAT), 3 = current strength only (CUR)
uint8_t spec_channel_scan = 0;

uint8_t sig_current[16]; // current signal spectrum
uint8_t sig_streak_n = 0, sig_streak_start = 0; // signal streak length and start channel (for radio glitch detection)

void spec_loop() {
  uint8_t last_channel = (menu_switch) ? 255 : spec_channel;
  uint8_t last_strength = (menu_switch) ? 255 : spec_strength;
  uint8_t last_mode = (menu_switch) ? 255 : spec_mode;
  
  if(menu_switch) {
    trx_halt = true;
    spec_n = 0;
    for(uint8_t i = 1; i < 8; i++) oled.fill_page(i);
    oled.tty_y = 7; oled.tty_x = 0; printf_P(PSTR("U/D:Move ptr OK:SA mode"));
    oled.tty_y = 6; oled.tty_x = 0; 
    printf_P(PSTR("0000 "));
    if(radio_pvar) printf_P(PSTR("RPD"));
    else printf_P(PSTR("CAR"));
    printf_P(PSTR(" Ch."));
    menu_switch = false;

    spec_channel_scan = 0;
  }

  if((buttons & (1 << 3)) && !(buttons & (1 << 7))) {
    /* OK */
    spec_mode = (spec_mode + 1) % 4;
  }

  if((buttons & (1 << 1)) && !(buttons & (1 << 5))) {
    /* UP */
    spec_channel = (spec_channel + 1) % 126;
  }

  if((buttons & (1 << 2)) && !(buttons & (1 << 6))) {
    /* DOWN */
    spec_channel = (spec_channel == 0) ? 125 : (spec_channel - 1);
  }

  /* scan cycle */
  if(spec_channel_scan == 0) {
    for(uint8_t i = 0; i < 16; i++) sig_current[i] = 0;
  }
  
  if(spec_channel_scan < 126) {
    radio.setChannel(spec_channel_scan);
    radio.startListening();
    delayMicroseconds(200); // allow for some time to receive
    bool sig = (radio_pvar) ? radio.testRPD() : radio.testCarrier(); // check for any >-64dBm signal (on + variant) or carrier only (non-+ variant)
    radio.stopListening();
    if(sig) {
      acty_on();
      sig_current[spec_channel_scan / 8] |= (1 << (spec_channel_scan % 8));
      if(sig_streak_n == 0) sig_streak_start = spec_channel_scan;
      sig_streak_n++;
    } else {
      acty_off();
      sig_streak_n = 0;
    }
    spec_channel_scan++;
  }

  if(spec_channel_scan == 126) {
    disp_update = true;
    if(sig_streak_n >= SPEC_GLITCH_STREAK) {
      uint8_t mask[8] = {0b00000000, 0b00000001, 0b00000011, 0b00000111, 0b00001111, 0b00011111, 0b00111111, 0b01111111};
      uint8_t idx = (sig_streak_start + 1) / 8, off = (sig_streak_start + 1) % 8;
      sig_current[idx] &= mask[off];
      for(uint8_t i = idx + 1; i < 16; i++) sig_current[i] = 0;
      radio.begin();
    }

    spec_n++;
    Serial.print(spec_n, DEC);
    Serial.print(':');
    for(uint8_t i = 0; i < 16; i++) {
      Serial.print(sig_current[i], HEX);
      if(i != 15) Serial.print(',');
      else Serial.println();
    }
    oled.tty_y = 6; oled.tty_x = 0; printf_P(PSTR("%04X"), spec_n);
    
    if(spec_mode == 3) {
      /* current strength only */
      for(uint8_t ch = 0; ch < 126; ch++) {
        bool sig = (sig_current[ch / 8] & (1 << (ch % 8)));
        for(uint8_t y = (SSD1306_HEIGHT - 20); y > (SSD1306_HEIGHT - 20) - 32; y--) oled.draw_pixel(ch + 1, y, sig);
      }
    } else if(spec_mode == 2) {
      /* waterfall */
      for(uint8_t y = (SSD1306_HEIGHT - 20) - 31; y < (SSD1306_HEIGHT - 20); y++) {
        for(uint8_t x = 1; x < 127; x++) oled.draw_pixel(x, y, oled.get_pixel(x, y + 1));
      }
      for(uint8_t ch = 0; ch < 126; ch++) {
        bool sig = (sig_current[ch / 8] & (1 << (ch % 8)));
        oled.draw_pixel(ch + 1, (SSD1306_HEIGHT - 20), sig);
      }
    } else {
      /* accumulative/sliding window (almost the same thing) */
      for(uint8_t ch = 0; ch < 126; ch++) {
        bool sig = (sig_current[ch / 8] & (1 << (ch % 8)));
        if(sig) {
          /* add a pixel */
          for(uint8_t y = (SSD1306_HEIGHT - 20); y > (SSD1306_HEIGHT - 20) - 32; y--) {
            if(!oled.get_pixel(ch + 1, y)) {
              oled.draw_pixel(ch + 1, y);
              break;
            }
          }
        } else if(spec_mode == 1) {
          /* remove pixel (only in sliding window) */
          for(uint8_t y = (SSD1306_HEIGHT - 20) - 31; y <= (SSD1306_HEIGHT - 20); y++) {
            if(oled.get_pixel(ch + 1, y)) {
              oled.draw_pixel(ch + 1, y, false);
              break;
            }
          }
        }
      }
    }

    spec_strength = 0;
    for(uint8_t y = (SSD1306_HEIGHT - 20) - 31; y <= (SSD1306_HEIGHT - 20); y++) {
      if(oled.get_pixel(spec_channel + 1, y)) spec_strength++;
    }
    
    spec_channel_scan = 0;
  }
  
  if(last_mode != spec_mode) {
    disp_update = true;
    oled.tty_y = 6; oled.tty_x = SSD1306_TEXT_WIDTH - 3;
    switch(spec_mode) {
      case 0: printf_P(PSTR("ACC")); break;
      case 1: printf_P(PSTR("SLW")); break;
      case 2: printf_P(PSTR("WAT")); break;
      case 3: printf_P(PSTR("CUR")); break;
    }
    for(uint8_t x = 1; x < 127; x++) {
      for(uint8_t y = (SSD1306_HEIGHT - 20); y > (SSD1306_HEIGHT - 20) - 32; y--) oled.draw_pixel(x, y, false);
    }
  }

  if(last_channel != spec_channel) {
    disp_update = true;
    oled.tty_y = 6; oled.tty_x = 12;
    printf_P(PSTR("%3d"), spec_channel);
    if(last_channel != 255) oled.draw_pixel(last_channel + 1, (SSD1306_HEIGHT - 20) + 2, false);
    oled.draw_pixel(spec_channel + 1, (SSD1306_HEIGHT - 20) + 2);
  }

  if(last_strength != spec_strength) {
    disp_update = true;
    oled.tty_y = 6; oled.tty_x = SSD1306_TEXT_WIDTH - 6;
    printf_P(PSTR("%2d"), spec_strength);
  }
}

/* benchmark */
// TODO

/* statistics */
uint32_t stats_recv = 0, stats_fail_miss = 0,stats_fail_dup = 0, stats_fail_radio = 0;
uint32_t stats_pkt_tproc_min = 0xFFFFFFFF, stats_pkt_tproc = 0, stats_pkt_tproc_max = 0; // packet processing time
uint32_t stats_pkt_tpp_min = 0xFFFFFFFF, stats_pkt_tpp = 0, stats_pkt_tpp_max = 0; // time between packets
uint16_t stats_ps2 = 0, stats_fail_ps2 = 0;
uint8_t stats_page = 0; // 0 = packet info, 1 = packet timing, 2 = PS2 controller info
bool stats_page_change = false;

void stats_loop() {
  bool update = ((buttons & (1 << 3)) && !(buttons & (1 << 7)));
  
  if(menu_switch) {
    stats_page_change = true;
    oled.fill_page(7);
    oled.tty_x = 0; oled.tty_y = 7;
    printf_P(PSTR("U/D:Page OK:Update"));
    oled.tty_x = SSD1306_TEXT_WIDTH - 3; oled.tty_y = 7;
    putchar('/'); putchar('3');
    menu_switch = false;
    update = true;
  }
  
  if((buttons & (1 << 1)) && !(buttons & (1 << 5))) {
    /* UP */
    if(stats_page > 0) {
      stats_page--;
      stats_page_change = true;
    }
  }
  
  if((buttons & (1 << 2)) && !(buttons & (1 << 6))) {
    /* DOWN */
    if(stats_page < 2) {
      stats_page++;
      stats_page_change = true;
    }
  }
  
  
  if(stats_page_change) {
    oled.tty_x = SSD1306_TEXT_WIDTH - 4; oled.tty_y = 7; printf_P(PSTR("%d"), stats_page+1);
    oled.tty_x = 0; oled.tty_y = 1;
    for(uint8_t i = 1; i < 7; i++) oled.fill_page(i);
    switch(stats_page) {
      case 0:
        printf_P(PSTR("Recv'd packets:\nMissed pkts:\nPkt fail rate:\nDuplicate pkts:\nRadio fails:"));
        oled.tty_x = SSD1306_TEXT_WIDTH - 1; oled.tty_y = 3; putchar('%');
        break;
      case 1:
        printf_P(PSTR("Time between pkts:     ms  Min:    ms Max:    ms\nPkt proc. time:        ms  Min:    ms Max:    ms"));
        break;
      case 2:
        printf_P(PSTR("PS2 ctrlr polls:\nPS2 ctrlr fails:"));
        break;
    }
    stats_page_change = false; update = true;
  }
    
  if(update) {
    float t;
    switch(stats_page) {
      case 0: 
        oled.tty_x = SSD1306_TEXT_WIDTH - 5; oled.tty_y = 1; printf_P(PSTR("%5u"), stats_recv);
        oled.tty_x = SSD1306_TEXT_WIDTH - 5; oled.tty_y = 2; printf_P(PSTR("%5u"), stats_fail_miss);
        t = 100 * (float)((float)(stats_fail_miss + stats_fail_dup) / (float)(stats_recv + stats_fail_miss));
        oled.tty_x = SSD1306_TEXT_WIDTH - 7; oled.tty_y = 3; printf_P(PSTR("%3u.%02u"), (uint8_t)t, (uint8_t)((t - (uint8_t)t)*100));
        oled.tty_x = SSD1306_TEXT_WIDTH - 5; oled.tty_y = 4; printf_P(PSTR("%5u"), stats_fail_dup);
        oled.tty_x = SSD1306_TEXT_WIDTH - 5; oled.tty_y = 5; printf_P(PSTR("%5u"), stats_fail_radio);
        break;
      case 1:
        oled.tty_x = 19; oled.tty_y = 1; printf_P(PSTR("%4u"), stats_pkt_tpp);
        if(stats_pkt_tpp_min != 0xFFFFFFFF) {
          oled.tty_x = 6; oled.tty_y = 2; printf_P(PSTR("%4u"), stats_pkt_tpp_min);
        }
        oled.tty_x = 17; oled.tty_y = 2; printf_P(PSTR("%4u"), stats_pkt_tpp_max);
        oled.tty_x = 19; oled.tty_y = 3; printf_P(PSTR("%4u"), stats_pkt_tproc);
        if(stats_pkt_tproc_min != 0xFFFFFFFF) {
          oled.tty_x = 6; oled.tty_y = 4; printf_P(PSTR("%4u"), stats_pkt_tproc_min);
        }
        oled.tty_x = 17; oled.tty_y = 4; printf_P(PSTR("%4u"), stats_pkt_tproc_max);
        break;
      case 2:
        oled.tty_x = SSD1306_TEXT_WIDTH - 5; oled.tty_y = 1; printf_P(PSTR("%5u"), stats_ps2);
        oled.tty_x = SSD1306_TEXT_WIDTH - 5; oled.tty_y = 2; printf_P(PSTR("%5u"), stats_fail_ps2);
        break;
    }
        
    disp_update = true;
  }
}

void disp_loop() {
  oled.text_invert = false;
   
  buttons <<= 4;
  buttons |= ((digitalRead(BTN_MODE) == LOW) ? (1 << 0) : 0) | ((digitalRead(BTN_UP) == LOW) ? (1 << 1) : 0) | ((digitalRead(BTN_DOWN) == LOW) ? (1 << 2) : 0) | ((digitalRead(BTN_OK) == LOW) ? (1 << 3) : 0);
#ifdef BUTTONS_DEBUG
  oled.tty_x = SSD1306_TEXT_WIDTH - 2; oled.tty_y = 0;
  printf_P(PSTR("%02X"), buttons);
  oled.update();
#endif

  if((buttons & (1 << 0)) && !(buttons & (1 << 4))) {
    if(disp_mode == 4) {
      // exiting from spectrum analyzer
      trx_halt = false;
      radio.begin(); // reinitialize
      radio_post_init();
    }
    menu_redraw = true;
    menu_sel = 1;
    disp_mode = 0;
  }

  switch(disp_mode) {
    case 0: menu_loop(); break;
    case 1: config_loop(); break;
    case 2: export_loop(); break;
    case 3: import_loop(); break;
    case 4: spec_loop(); break;
    case 6: stats_loop(); break;
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

#ifdef LED_ERROR
  pinMode(LED_ERROR, OUTPUT); digitalWrite(LED_ERROR, LOW);
#endif

  stdout = fdevopen(&stdout_write, NULL);

  Wire.begin();
  Wire.setClock(400000);
  oled.init(); oled.fill(); oled.update();


#ifndef RADIO_CHANNEL_OVR
  radio_channel = EEPROM.read(EEP_ADDR_CHANNEL);
  if(radio_channel > 125) {
    radio_channel = 76;
    EEPROM.update(EEP_ADDR_CHANNEL, radio_channel);
  }
#else
  radio_channel = RADIO_CHANNEL_OVR;
#endif
#ifndef RADIO_RATE_OVR
  radio_rate = EEPROM.read(EEP_ADDR_DRATE);
  if(radio_rate > 2) {
    radio_rate = 1;
    EEPROM.update(EEP_ADDR_DRATE, radio_rate);
  }
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
#ifndef RADIO_PALVL_OVR
  radio_palvl = EEPROM.read(EEP_ADDR_PALVL);
  if(radio_palvl > 3) {
    radio_rate = 0;
    EEPROM.update(EEP_ADDR_PALVL, radio_palvl);
  }
#else
  radio_palvl = RADIO_PALVL_OVR;
#endif

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
      //radio.enableDynamicPayloads();
      radio_post_init();
      chklist_ok();
      break;
    }
  }
  
#ifdef LED_ACTY
  digitalWrite(LED_ACTY, LOW);
#endif

  oled.tty_x = 0; oled.tty_y = 0; oled.fill_page(0);
  printf_P(PSTR("PS2 TRANSCEIVER"));
  radio_pvar = radio.isPVariant();
  if(radio_pvar) printf_P(PSTR(" (+)"));
  oled.update();
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

// #define PAYLOAD_DEBUG

bool send_fail = false;

// #define ERROR_OLED

/* duration for failure modes */
#define FAIL_DUR_AVAILABLE        50 // data always available
#define FAIL_DUR_CONFIG           1000 // config verification check

uint32_t config_timer = 0;
uint32_t t_last_pkt = 0;

#define PKT_MISS_MAX         100 // max number of missed packets to be counted as missed and not packet ID reset

void loop() {
  if(t_last_pkt == 0) t_last_pkt = millis();
  if(radio.failureDetected) {
    stats_fail_radio++;
    
#ifdef LED_ERROR
    digitalWrite(LED_ERROR, HIGH);
#endif
    radio.begin();
    radio_post_init();
    radio.failureDetected = false;
#ifdef LED_ERROR
    digitalWrite(LED_ERROR, LOW);
#endif
  }
  
  // put your main code here, to run repeatedly:

  disp_loop();

  if(trx_halt) return;
  
  if(millis() - config_timer > FAIL_DUR_CONFIG) {
    if(radio.getDataRate() != radio_rate || radio.getChannel() != radio_channel) {
      Serial.println(F("FAIL_DUR_CONFIG"));
      radio.failureDetected = true;
      return;
    }
  }
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
    uint32_t fail_timer = millis();
    while(radio.available()) {
      if(millis() - fail_timer > FAIL_DUR_AVAILABLE) {
        Serial.println(F("FAIL_DUR_AVAILABLE"));
        radio.failureDetected = true;
        return;
      }
      trx_len = radio.getPayloadSize();
      radio.read(trx_buf, trx_len);
    }

    stats_recv++;
    acty_on();
    
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
      uint32_t tpp = millis() - t_last_pkt;
      stats_pkt_tpp = ((stats_pkt_tpp * (stats_recv - stats_fail_dup - 1)) + tpp) / (stats_recv - stats_fail_dup);
      if(stats_pkt_tpp_min > tpp) stats_pkt_tpp_min = tpp;
      if(stats_pkt_tpp_max < tpp) stats_pkt_tpp_max = tpp;
      uint32_t tproc = millis();
      if(!(last_pktid & (1 << 15)) && !(pktid & (1 << 15)) && (uint16_t)(pktid - last_pktid) > 1 && (uint16_t)(pktid - last_pktid) <= PKT_MISS_MAX && !(last_pktid == 0x7FFF && pktid == 0x0000)) stats_fail_miss += (uint16_t)(pktid - last_pktid);
      last_pktid = pktid;
      int i;
      // sprintf(str_buf, "Packet length: %d, header: %02X %02X %02X\n", trx_len, trx_buf[0], trx_buf[1], trx_buf[2]); Serial.print((char*)&str_buf);
      switch(trx_buf[2]) {
        case 0x00: // loopback test
          break;
        case 0x02:
          if(millis() - ps2.t_last_att >= CTRL_PACKET_DELAY) {
            // only poll PS2 controller if enough time has passed
            stats_ps2++;
            if(ps2.read_gamepad((trx_buf[3] != 0), trx_buf[4]) == false) {
              stats_fail_ps2++;
              trx_buf[2] = 0xF2;
              trx_len = 3;
              break;
            }
          }
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

      //radio.setPayloadSize(trx_len);
      radio.write(trx_buf, trx_len); // fail condition only exists if auto ack is enabled
      tproc = millis() - tproc;
      stats_pkt_tproc = ((stats_pkt_tproc * (stats_recv - stats_fail_dup - 1)) + tproc) / (stats_recv - stats_fail_dup);
      if(stats_pkt_tproc_min > tproc) stats_pkt_tproc_min = tproc;
      if(stats_pkt_tproc_max < tproc) stats_pkt_tproc_max = tproc;
      
      t_last_pkt = millis();
/*
      if(radio.write(trx_buf, trx_len) == false) {
        stats_fail_resp++;
        send_fail = true;
#ifdef ERROR_OLED
        oled.tty_y = 7; oled.text_invert = true;
        if(!send_fail) {
          oled.tty_x = 0;
          printf_P(PSTR("Resp. failure (ID %04X)"), pktid);
        } else {
          oled.tty_x = 18;
          printf_P(PSTR("%04X"), pktid);
        }
        oled.text_invert = false;
        oled.update();
#endif
#ifdef LED_ERROR
        digitalWrite(LED_ERROR, HIGH);
#endif
      } else if(send_fail == true) {
        send_fail = false;
#ifdef ERROR_OLED
        oled.fill_page(7);
        oled.update();
#endif
#ifdef LED_ERROR
        digitalWrite(LED_ERROR, LOW);
#endif
      }
*/
    } else stats_fail_dup++;
    
    radio.startListening();
    acty_off();
  }
}
