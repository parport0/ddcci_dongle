#include <Wire.h>
// Should be a divisor of 128, or fix the edid reading code
#define SPI_BUFFER_SIZE 32

// Shared constants

// Inputs, Brightness.
#define NUM_MENU_ITEMS 2

// Max number of display inputs we read out from the monitor.
#define MAX_INPUTS 10

// MCCS 2.2:
// 0x60: Input Source Select
// 0x10: Luminance (= brightness)
const byte vcp_codes[NUM_MENU_ITEMS] = { 0x60, 0x10 };

const char default_monitor_name[16] = "Unknown monitor";

// The below are shared buffers between cores 0 and 1

// Monitor name is written by core1 after it sends FIFO_UPD_MONITOR_LOST,
// and read by core0 when it gets FIFO_UPD_MONITOR_EDID_FOUND.
auto_init_mutex(shared_monitor_name_mutex);
volatile char shared_monitor_name[16] = { 0 };

// Inputs are written by core1 after it sends FIFO_UPD_MONITOR_EDID_FOUND,
// and read by core0 when it gets FIFO_UPD_NEW_VCP_VALUES for the first time after.
auto_init_mutex(shared_monitor_inputs_mutex);
volatile byte shared_monitor_inputs_ids[MAX_INPUTS];
volatile int shared_monitor_inputs_num_detected = 0;

// Data is written by core1 periodically in the main loop,
// and read by core0 when it gets FIFO_UPD_NEW_VCP_VALUES.
auto_init_mutex(shared_vcp_values_mutex);
volatile uint16_t shared_vcp_values_current[NUM_MENU_ITEMS] = { 0x0, 50 };
volatile uint16_t shared_vcp_values_max[NUM_MENU_ITEMS] = { 0, 100 };

// Core 1 is doing I2C with the monitor.

#define MONITOR_ADDR 0x37
#define EDID_ADDR 0x50
#define I2C_INTERVAL_SHORT 100
#define I2C_INTERVAL_LONG 1000

#define MONITOR_SDA 2
#define MONITOR_SCL 3

void setup1() {
  Wire1.setSDA(MONITOR_SDA);
  Wire1.setSCL(MONITOR_SCL);
  Wire1.begin();
}

// Sending stuff to the monitor
int ddcci_send_packet(byte* payload, size_t payload_length) {
  byte checksum = (MONITOR_ADDR << 1);
  Wire1.beginTransmission(MONITOR_ADDR);
  for (int i = 0; i < payload_length; i++) {
    checksum ^= payload[i];
    Wire1.write(payload[i]);
  }
  Wire1.write(checksum);
  byte error = Wire1.endTransmission();
  return error;
}

// Try three times in case some other stuff is going on on the I2C bus?
int ddcci_send_packet_try_loop(byte* payload, size_t payload_length) {
  int error;
  for (int i = 0; i < 3; i++) {
    if ((error = ddcci_send_packet(payload, payload_length)) == 0) {
      return 0;
    }
  }
  return error;
}

int ddcci_send_vcp_value(byte feature, uint16_t value) {
  byte payload[] = {
    0x51,
    0x84,
    0x03,
    feature,
    highByte(value),
    lowByte(value)
  };

  int error = ddcci_send_packet_try_loop(payload, sizeof(payload));
  // DDC/CI v1.1, section 4.4:
  // "The host should wait at least 50ms to ensure next message is received by the display."
  delay(50);
  return error;
}

// Reading stuff from the monitor

// These are compared with shared_vcp_values_*, and if updated, FIFO_UPD_NEW_VCP_VALUES is sent.
// These are set by ddcci_get_vcp_values and read by loop1.
uint16_t core1_read_values_current[NUM_MENU_ITEMS] = { 0x0, 50 };
uint16_t core1_read_values_max[NUM_MENU_ITEMS] = { 0, 100 };

// Writes to core1_read_values_*.
// Returns non-zero if I2C communication did not go well, or if garbage was received.
int ddcci_get_vcp_values(uint8_t valueIdx) {
  byte payload[] = {
    0x51,
    0x82,
    0x01,
    vcp_codes[valueIdx]
  };

  int error;
  if ((error = ddcci_send_packet_try_loop(payload, sizeof(payload))) != 0) {
    return error;
  }

  // DDC/CI v1.1, section 4.4:
  // "The host should wait at least 40ms in order to enable the decoding and preparation
  // of the reply message by the display."
  delay(40);

  // 11 is the proper packet size for a response
  // (6Fa-6Ea-88a-02a-RCa-CPa-TPa-MHa-MLa-SHa-SLa-CHK’n-P)
  if (Wire1.requestFrom((int) MONITOR_ADDR, 11) != 11) {
    return -1;
  }

  if (Wire1.read() != 0x6E) return -1;  // Destination address
  Wire1.read();  // length?
  if (Wire1.read() != 0x02) return -1;  // VCP feature reply
  if (Wire1.read() != 0x00) return -1;  // result code: 00 - NoError
  if (Wire1.read() != vcp_codes[valueIdx]) return -1;  // VCP opcode from Feature request message
  Wire1.read();  // VCP type code: 00 - set parameter, 01 - momentary (??)

  core1_read_values_max[valueIdx] = ((byte) Wire1.read()) << 8;
  core1_read_values_max[valueIdx] |= (byte) Wire1.read();

  if (valueIdx != 0) {
    core1_read_values_current[valueIdx] = ((byte) Wire1.read()) << 8;
    core1_read_values_current[valueIdx] |= (byte) Wire1.read();
  } else {
     // DELL IS WEIRD
     // It returns "0x1111" when it wants to say, "0x11, that is, HDMI-1!"
     // Same for the "max" value, if anyone's interested
     Wire1.read();
     core1_read_values_current[valueIdx] = Wire1.read();
  }

  // not flushing any Wire buffers, see
  // https://github.com/Koepel/How-to-use-the-Arduino-Wire-library/wiki/Common-mistakes#4

  return 0;
}

// Should be holding shared_monitor_name_mutex when calling this
// because this writes to shared_monitor_name.
// If the monitor name is not set by the monitor, returns 0 but writes "Unknown" to the string.
// Returns non-zero if I2C communication did not go well.
int edid_read_monitor_name_or_set_unknown() {
  // All this reading the monitor name business is just for the fancy points
  // (to be able to draw the name at the top of the screen)

  memcpy((void *) shared_monitor_name, default_monitor_name, sizeof(shared_monitor_name));

  Wire1.beginTransmission(EDID_ADDR);
  // EDID lives at address 0
  Wire1.write(0x00);

  int error;
  if ((error = Wire1.endTransmission()) != 0) {
    return error;  // The monitor disappeared?
  }

  // how much to wait here? Can't find in the documents
  delay(50);

  // Arduino I2C buffer is SPI_BUFFER_SIZE bytes
  // EDID is fixed size of 128 bytes
  byte edid[128];
  for (int i = 0; i < 128; i += SPI_BUFFER_SIZE) {
    if (Wire1.requestFrom((int) EDID_ADDR, SPI_BUFFER_SIZE) != SPI_BUFFER_SIZE) {
      return 1;  // Not enough data returned, something strange is going on
    }
    for (int j = 0; j < SPI_BUFFER_SIZE; j++) {
      edid[i + j] = Wire1.read();
    }
  }

  // The monitor name hides in one of the "18 Byte Descriptors"
  // (previously known as "Detailed Timing Blocks"), located at:
  // 36h -> 47h, 48h -> 59h, 5Ah -> 6Bh, 6Ch -> 7Dh
  // VESA E-EDID 1.4 section 3.10:
  // "Each of the four data blocks shall contain a detailed timing descriptor,
  // a display descriptor or a dummy descriptor (Tag 10h) (...)"
  // "The first 18 byte descriptor field shall be used to indicate the display's preferred timing mode. (...)"
  // "A Display Product Name Descriptor (required in EDID data structure version 1, revision 3)
  // is optional (but recommended) in EDID data structure version 1, revision 4."
  // "The first three bytes of the block shall be ‘000000h’, when the 18 byte descriptor contains a
  // display descriptor (not a detailed timing). The fourth byte shall contain the display descriptor
  // tag number (from Table 3.23) and the fifth byte shall be ‘00h’. (...)"
  // "FEh Alphanumeric Data String (ASCII): Up to 13 alphanumeric characters of a data string may be stored."
  // "If there are less than 13 characters in the string, then terminate the alphanumeric data string
  // with ASCII code ‘0Ah’ (line feed) and pad the unused bytes in the field with ASCII code ‘20h’ (space)."

  for (int i = 0; i < 3; i++) {
    int idx = 72 + i * 18;

    if (edid[idx] == 0x00 && edid[idx + 1] == 0x00 && edid[idx + 2] == 0x00 && edid[idx + 3] == 0xFC) {
      memset((void *) shared_monitor_name, 0, sizeof(shared_monitor_name));
      for (int j = 0; j < 13; j++) {
        if (edid[idx + 5 + j] == 0x0A) {
          break;
        }
        shared_monitor_name[j] = edid[idx + 5 + j];
      }
      break;
    }
  }

  return 0;
}

// Should be holding shared_monitor_inputs_mutex when calling this
// because this writes to shared_monitor_inputs_*.
// This does a Capabilities Request (several, because the string can be long) to read out the vcp() string.
// It parses this string on the fly to find the part about capability 60 (inputs).
// This could also check if brightness (10h), contrast (12h), or inputs (60h) are even supported capabilities
// of a given monitor, but... later.
// Returns non-zero if I2C communication did not go well, or if garbage was received
int scan_input_sources() {
  shared_monitor_inputs_num_detected = 0;

  int match_step = 0;  // 0: looking for "6", 1: looking for "0", 2: looking for "(", 3: reading the bytes until ")"
  int offset = 0;  // reading the vcp string in chunks, this tracks the offset to request
  bool done = false;

  char token[3];  // string containing a hex code of a byte corresponsing to one supported input value
  int token_idx = 0;
  memset(token, 0, sizeof(token));

  while (!done) {
    byte payload[] = {
      0x51,
      0x83,
      0xF3,
      highByte(offset),
      lowByte(offset)
    };

    int error;
    if ((error = ddcci_send_packet_try_loop(payload, sizeof(payload))) != 0) {
      return error;
    }

    // DDC/CI v1.1, section 4.6:
    // "The host should wait at least 50ms before sending the next message to the display."
    // I guess I'll just wait before reading the response
    delay(50);

    // "If the display has reached end-of-string, it shall send a fragment with the next offset but zero data bytes."
    // -- nb: different monitor makers interpret the "zero data bytes" as "32 bytes of zeroes"
    // S-6Fa-6Ea-XXa-E3a-OFS(H)a-OFS(L)a-Data(0)a-Data(1)a-Data(2)a------Data(n)a-CHK’n-P
    if (Wire1.requestFrom((int) MONITOR_ADDR, SPI_BUFFER_SIZE) < 6) {
      return -1;
    }

    char c;
    c = Wire1.read();
    if (c != 0x6E && c != 0x6F) {  // The document says 6F, IRL it is 6E, silly mistake
      return -1;
      }

    // This one's the length, the document says it is between 3 and 35;
    // I interpret it as it includes (the bytes following it) E3, OFS(H), OFS(L), and does not include CHK'n.
    // But just in case length actually includes CHK'n for some monitor... Worst case, we'll be skipping some trailing ")"?
    c = Wire1.read();
    if (c <= 4) {
      done = true;
      break;
    }
    unsigned length = c - 4;

    c = Wire1.read();
      if (c != 0xE3) {
        return -1;
      }

    Wire1.read();  // OFS(H)
    Wire1.read();  // OFS(L)

    // We requested SPI_BUFFER_SIZE bytes, of them [0]-[4] are the header,
    // bytes [5]-[30] are the actual data, byte [31] could be the CHK'n.
    // SOME MONITORS RETURN FULL BYTES OF ZEROES when the actual data is done.
    // So take the minimum of (data left in the buffer - 1, length we saved earlier which is also safety-reduced - 1).
    // A lot of safety/liberties here... As per MCCS, in the end of the string, there should be an "mccs_ver" chunk.
    // So the 60() part is guaranteed not to be there.
    length = min(length, Wire1.available() - 1);

    for (int i = 0; i < length; i++) {
      c = Wire1.read();

      if (match_step < 3) {  // searching for the "60("
        if (match_step == 0 && c == '6') {
          match_step = 1;
        } else if (match_step == 1 && c == '0') {
          match_step = 2;
        } else if (match_step == 2 && c == '(') {
          match_step = 3;
        } else {
          // reset the search
          if (c == '6') {
            match_step = 1;
          } else {
            match_step = 0;
          }
        }

      } else if (match_step == 3) {
        if (c == ')') {  // token list done
          process_input_source_token(token);
          done = true;
          break;
        } else if (!isHexadecimalDigit(c)) {  // token done
          process_input_source_token(token);
          token_idx = 0;
          memset(token, 0, sizeof(token));
        } else {
          if (token_idx < 2) {  // tokens are two characters only since it's a byte
            token[token_idx++] = c;
          }  // just skip the following bytes if the monitor's got weird understanding of what a byte is
        }
      }
    }

    offset += length;

    if (offset > 2048) {
      // this vcp is too long, bail out
      done = true;
    }
  }

  return 0;
}

// Should be holding shared_monitor_inputs_mutex when calling this
// Should only be called from scan_input_sources
void process_input_source_token(char *token) {
  if (shared_monitor_inputs_num_detected >= MAX_INPUTS) {
    return;
  }

  long code = strtol(token, NULL, 16);
  if (code != 0) {  // 0 is not a valid input source as per MCCS, and is returned by strtol when it fails.
    shared_monitor_inputs_ids[shared_monitor_inputs_num_detected] = code;
    shared_monitor_inputs_num_detected++;
  }
}

// Pinging the monitor is done when waiting for the monitor to come up
// or checking if it is lost
int ping_edid_addr() {
  Wire1.beginTransmission(EDID_ADDR);
  return Wire1.endTransmission();
}

int ping_ddcci_addr() {
  Wire1.beginTransmission(MONITOR_ADDR);
  return Wire1.endTransmission();
}

#define FIFO_CMD_ACK_MONITOR_LOST 0x1
#define FIFO_CMD_SEND_A_VCP_TO_MONITOR 0x2
#define FIFO_CMD_STOP_PINGING 0x3
#define FIFO_CMD_START_PINGING 0x4

#define FIFO_UPD_MONITOR_LOST 0x1
#define FIFO_UPD_MONITOR_EDID_FOUND 0x2
#define FIFO_UPD_NEW_VCP_VALUES 0x3

void update_other_core_about_lost_monitor() {
  rp2040.fifo.push(FIFO_UPD_MONITOR_LOST);
  // Flush the fifo until we get an ack from core0.
  // In case of a lost monitor, the fifo needs to be flushed so that
  // vcp set commands for the previous monitor don't get sent to the new one.
  while (rp2040.fifo.pop() != FIFO_CMD_ACK_MONITOR_LOST) {};
}

void loop1() {
  static bool should_be_pinging = true;
  static bool monitor_exists = false;
  static unsigned long time_of_last_readout = 0;

  if (!monitor_exists) {
    byte error;
    do {
      error = ping_edid_addr();
      delay(500);
    } while (error != 0);

    mutex_enter_blocking(&shared_monitor_name_mutex);
    if (edid_read_monitor_name_or_set_unknown()) {  // I2C error -> the monitor disappeared
      mutex_exit(&shared_monitor_name_mutex);
      return;  // re-enter loop1() with monitor_exists set to false
    }
    mutex_exit(&shared_monitor_name_mutex);

    rp2040.fifo.push(FIFO_UPD_MONITOR_EDID_FOUND);

    mutex_enter_blocking(&shared_monitor_inputs_mutex);
    while (scan_input_sources()) {
        // lost monitor or just no ddc/ci?
      if (ping_edid_addr()) {
        mutex_exit(&shared_monitor_inputs_mutex);
        monitor_exists = false;  // I2C error -> the monitor disappeared
        update_other_core_about_lost_monitor();
        return;  // re-enter loop1() with monitor_exists set to false
      }
    }
    mutex_exit(&shared_monitor_inputs_mutex);
  }

  unsigned long time_now = millis();
  if (should_be_pinging || !monitor_exists) {
    if (!monitor_exists || abs((long long) time_now - time_of_last_readout) > I2C_INTERVAL_LONG) {
      for (int i = 0; i < NUM_MENU_ITEMS; i++) {
        if (ddcci_get_vcp_values(i)) {
          monitor_exists = false;  // I2C error -> the monitor disappeared
          update_other_core_about_lost_monitor();
          return;  // re-enter loop1() with monitor_exists set to false
        }
      }
      time_of_last_readout = time_now;
    }

    for (int i = 0; i < NUM_MENU_ITEMS; i++) {
      if ((!monitor_exists) ||
          (core1_read_values_current[i] != shared_vcp_values_current[i]) ||
          (core1_read_values_max[i] != shared_vcp_values_max[i]))
      {
        mutex_enter_blocking(&shared_vcp_values_mutex);
        memcpy((void *) shared_vcp_values_current, core1_read_values_current, sizeof(shared_vcp_values_current));
        memcpy((void *) shared_vcp_values_max, core1_read_values_max, sizeof(shared_vcp_values_max));
        mutex_exit(&shared_vcp_values_mutex);

        rp2040.fifo.push(FIFO_UPD_NEW_VCP_VALUES);

        break;
      }
    }

    monitor_exists = true;
  }

  int32_t values_to_update[NUM_MENU_ITEMS] = { -1, -1 };

  while (rp2040.fifo.available()) {
    uint32_t data = rp2040.fifo.pop();
    uint8_t cmd = data & 0xFF;

    if (cmd == FIFO_CMD_SEND_A_VCP_TO_MONITOR) {
      // Cross-check with: ddcci_send_choices()
      unsigned idx = (data >> 24) & 0xFF;
      values_to_update[idx] = (uint32_t) ((data >> 8) & 0xFF);
      values_to_update[idx] |= (uint32_t) ((data >> 16) & 0xFF) << 8;

    } else if (cmd == FIFO_CMD_STOP_PINGING) {
      should_be_pinging = false;

    } else if (cmd == FIFO_CMD_START_PINGING) {
      should_be_pinging = true;

    } else {
      // FIFO_CMD_ACK_MONITOR_LOST. wat
    }
  }

  for (int i = 0; i < NUM_MENU_ITEMS; i++) {
    if (values_to_update[i] != -1) {
      if (ddcci_send_vcp_value(vcp_codes[i], values_to_update[i])) {
        monitor_exists = false;  // I2C error -> the monitor disappeared
        update_other_core_about_lost_monitor();
        return;  // re-enter loop1() with monitor_exists set to false
      }
      // Update the shared values so there is no extra FIFO_UPD_NEW_VCP_VALUES later.
      // Both cores know the values should be these values_to_update now.
      mutex_enter_blocking(&shared_vcp_values_mutex);
      shared_vcp_values_current[i] = (uint16_t) values_to_update[i];
      mutex_exit(&shared_vcp_values_mutex);

      // Postpone the next readout, right now we know what the values should be
      time_of_last_readout = time_now;
      core1_read_values_current[i] = (uint16_t) values_to_update[i];
    }
  }
}

// Core 0 is doing the UI.

#include "SparkFun_Qwiic_Twist_Arduino_Library.h"
TWIST twist;

#define COLOR_BG_PRIMARY 0xF7BE
#define COLOR_BG_SECONDARY 0xE73C
#define COLOR_BG_TERTIARY 0xBDF7
#define COLOR_FG_PRIMARY 0x2124
#define COLOR_FG_SECONDARY 0x6b2d

uint16_t menu_colors[NUM_MENU_ITEMS] = { 0xF816, 0xFD20 };

// These store copies of the shared arrays
volatile uint16_t core0_monitors_vcp_values[NUM_MENU_ITEMS] = { 0 };
volatile uint16_t core0_max_vcp_values[NUM_MENU_ITEMS] = { 0 };

volatile char core0_monitor_name[16] = { 0 };

volatile byte core0_monitor_inputs_ids[MAX_INPUTS];
volatile int core0_monitor_inputs_num_detected = 0;

// This is based on the shared arrays, updated only when the first readout for the monitor happens
char core0_monitor_inputs_names[MAX_INPUTS][16];

// This is the values seen in the UI. Usually newer than core0_monitors_vcp_values,
// but updated from core0_monitors_vcp_values when going to the menu
uint16_t core0_menu_vcp_values[NUM_MENU_ITEMS] = { 0 };


void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 10000UL);
  Serial.println("Hello there");

  if (twist.begin() == false)
  {
    Serial.println("Can't find the twist.");
    while (1)
      ;
  }
}

void ddcci_send_choices(int current_sub_menu) {
  if (core0_monitors_vcp_values[current_sub_menu] != core0_menu_vcp_values[current_sub_menu]) {
    // Cross-check with: loop1()
    uint32_t pkt = FIFO_CMD_SEND_A_VCP_TO_MONITOR;
    pkt |= (lowByte(core0_menu_vcp_values[current_sub_menu])) << 8;
    pkt |= (highByte(core0_menu_vcp_values[current_sub_menu])) << 16;
    pkt |= ((uint8_t) current_sub_menu) << 24;

    rp2040.fifo.push(pkt);
    core0_monitors_vcp_values[current_sub_menu] = core0_menu_vcp_values[current_sub_menu];
  }
}

unsigned positive_modulo(int value, unsigned m) {
    int mod = value % (int )m;
    if (mod < 0) {
        mod += m;
    }
    return mod;
}

enum State {
  STATE_SEARCHING,
  STATE_READING_OUT,
  STATE_ONLINE
};

State current_state = STATE_SEARCHING;

void loop() {
  static int current_input = 0;
  static int current_rotary = 0;
  static int fade = 255;
  static unsigned long time_fade = millis() + 255 * 5;

  if (current_state == STATE_SEARCHING) {
    draw_searching_screen();

    while (rp2040.fifo.pop() != FIFO_UPD_MONITOR_EDID_FOUND) {};

    mutex_enter_blocking(&shared_monitor_name_mutex);
    memcpy((void *) core0_monitor_name, (void *) shared_monitor_name, sizeof(core0_monitor_name));
    mutex_exit(&shared_monitor_name_mutex);

    current_state = STATE_READING_OUT;
  }

  if (current_state == STATE_READING_OUT) {
    draw_reading_out_screen();

    while (current_state == STATE_READING_OUT) {
      uint32_t message = rp2040.fifo.pop();

      if (message == FIFO_UPD_MONITOR_LOST) {
        rp2040.fifo.push(FIFO_CMD_ACK_MONITOR_LOST);
        current_state = STATE_SEARCHING;
        return;
      }
      if (message == FIFO_UPD_NEW_VCP_VALUES) {
        mutex_enter_blocking(&shared_monitor_inputs_mutex);
        memcpy((void *) core0_monitor_inputs_ids, (void *) shared_monitor_inputs_ids, sizeof(core0_monitor_inputs_ids));
        core0_monitor_inputs_num_detected = shared_monitor_inputs_num_detected;
        mutex_exit(&shared_monitor_inputs_mutex);

        mutex_enter_blocking(&shared_vcp_values_mutex);
        memcpy((void *) core0_max_vcp_values, (void *) shared_vcp_values_max, sizeof(core0_max_vcp_values));
        memcpy((void *) core0_monitors_vcp_values, (void *) shared_vcp_values_current, sizeof(core0_monitors_vcp_values));
        mutex_exit(&shared_vcp_values_mutex);
        memcpy(core0_menu_vcp_values, (void *) core0_monitors_vcp_values, sizeof(core0_menu_vcp_values));

        for (int i = 0; i < core0_monitor_inputs_num_detected; i++) {
          if (core0_monitor_inputs_ids[i] == core0_menu_vcp_values[0]) {
            current_input = i;
          }
        }
        twist.setLimit(core0_max_vcp_values[1]);
        twist.setCount(core0_monitors_vcp_values[1]);

        current_state = STATE_ONLINE;
        twist.setColor(0, 0, 0);
        return;
      }
    }
  }

  if (current_state == STATE_ONLINE) {
    unsigned long fade = millis();
    fade = (time_fade > fade ? time_fade - fade : 0);
    fade = fade / 5;
    twist.setColor(fade, fade, fade);

    if (twist.isPressed()) {
      current_input = positive_modulo(current_input + 1, core0_monitor_inputs_num_detected);
      core0_menu_vcp_values[0] = core0_monitor_inputs_ids[current_input];

      time_fade = millis() + 255 * 5;
      ddcci_send_choices(0);
      return;
    }

    int twist_diff = twist.getDiff();
    if (twist_diff != 0) {
      core0_menu_vcp_values[1] = core0_menu_vcp_values[1] + twist_diff;
      time_fade = millis() + 255 * 5; 
      ddcci_send_choices(1);
      return;
    }
  }

  bool should_get_new_values = false;
  while (rp2040.fifo.available()) {
    uint32_t message = rp2040.fifo.pop();

    if (message == FIFO_UPD_MONITOR_LOST) {
      rp2040.fifo.push(FIFO_CMD_ACK_MONITOR_LOST);
      current_state = STATE_SEARCHING;
      return;
    }

    if (message == FIFO_UPD_NEW_VCP_VALUES) {
      should_get_new_values = true;
    }
  }

  if (should_get_new_values) {
    mutex_enter_blocking(&shared_vcp_values_mutex);
    memcpy((void *) core0_monitors_vcp_values, (void *) shared_vcp_values_current, sizeof(core0_monitors_vcp_values));
    mutex_exit(&shared_vcp_values_mutex);
    memcpy(core0_menu_vcp_values, (void *) core0_monitors_vcp_values, sizeof(core0_menu_vcp_values));

    twist.setLimit(core0_max_vcp_values[1]);
    twist.setCount(core0_monitors_vcp_values[1]);
    time_fade = millis() + 255 * 5;
  }
}

void draw_searching_screen() {
  twist.setColor(255, 0, 180);
}

void draw_reading_out_screen() {
  twist.setColor(255, 166, 0);
}

void draw_full_main_menu(int current_sub_menu) {
  twist.setColor(0, 0, 0);
}