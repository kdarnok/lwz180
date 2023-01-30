/*
  Board: Arduino/Genuino Micro
*/

// Include Arduino Wire library for I2C
#include <Wire.h>
#include <KnxTpUart.h>

// some addresses for the two buses we service
#define I2C_SLAVE_ADDR 42
#define KNX_HW_ADDR "1.0.99"
#define KNX_GA_LEVEL_SET "5/7/11"
#define KNX_GA_LEVEL_STAT "5/7/12"
#define KNX_GA_POWER_SET "5/7/13"
#define KNX_GA_POWER_STAT "5/7/14"
#define KNX_GA_SCHEDULED_SET "5/7/15"
#define KNX_GA_SCHEDULED_STAT "5/7/16"

#define MEASUREMENT_INTERVAL 60000  // interval between bursts of KNX measurements updates (milliseconds)
#define BUTTON_DELAY 700  // delay between successive button commands for a sequence of level up/down commands
#define WHEEL_DELAY 50  // time between successive i2c packets for a wheel button command

#define CHANGED_POWER_VENT 1
#define CHANGED_SCHEDULED 2
#define CHANGED_LEVEL 4

#define CRC_POLY 0x49  // generator of the 8-bit crc

// button codes
#define BUTTON_DOWN 0x01
#define BUTTON_UP 0x02
#define BUTTON_POWER_VENT 0x0c

// #define PRINT_SERIAL 1  // enable debug printout via uart/usb

byte ledVal = 0;  // current LED state for blinking when there's i2c activity
uint32_t last = 0;  // timestamp of last KNX update
uint32_t last_button = 0;  // timestamp of last i2c button packet we sent

// pre-defined buffer for the button packets. the last two bytes
// (button code and checksum) are filled in as needed
byte button_packet[] = {0xe3, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00};

// a measurement record -- glueing a register number from the i2c packet to the corresponding
// KNX group address and the scaling factor
struct Measurement {
  byte reg;  // register number
  String ga;  // group address
  byte scale;  // divisor for scaling the fixed-point decimal value
};

const byte MAX_REG = 0x14 + 1;  // number of registers in 0..MAX_REG-1
const byte N_MEAS = 4;  // number of measurerements to observe

byte changed = 0;  // bit-mask of changed measurements

Measurement measurements[N_MEAS] = {
  {0x06, "5/7/3", 10}, // outside temperature
  {0x0f, "5/7/2", 10}, // exhaust humidity
  {0x10, "5/7/4", 1},  // inlet flow, calculated
  {0x14, "5/7/5", 10}, // power inlet fan
};
byte registers[MAX_REG];  // easy lookup of records in `measurements` from a register number
int16_t values[N_MEAS] = {};  // the actual values of the measurements above

// status determined from the display state those are essentially bools,
// but allowing for `-1` to indicate unknown/uninitialised states
struct Status {
  int8_t power_vent;  // if power-venting is enabled (level 3)
  int8_t scheduled;  // if the device runs on a schedule (timer symbol)
  int8_t level;  // currently active ventilation level
};

byte stat_changed = 0;
Status stat = {-1, -1, -1};  // initialising as `-1` (unknown)

const byte DIGIT_0 = 0x3f; // 7-segment encoded `0`
const byte DIGIT_1 = 0x06; // 7-segment encoded `1`
const byte DIGIT_2 = 0x5b; // 7-segment encoded `2`
const byte DIGIT_3 = 0x4f; // 7-segment encoded `3`

// requested status changes
int8_t target_level = -1;
bool set_scheduled = false;

KnxTpUart knx(&Serial1, KNX_HW_ADDR);

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  #ifdef PRINT_SERIAL
  Serial.begin(115200);
  Serial.println("starting ...");
  #endif

  Serial1.begin(19200, SERIAL_8E1);
  knx.uartReset();
  knx.addListenGroupAddress(KNX_GA_LEVEL_SET);
  knx.addListenGroupAddress(KNX_GA_POWER_SET);
  knx.addListenGroupAddress(KNX_GA_SCHEDULED_SET);

  for (byte i = 0; i < MAX_REG; i ++) {
    registers[i] = 0x00;
  }
  for (byte i = 0; i < N_MEAS; i ++) {
    byte idx = measurements[i].reg;
    registers[idx] = i + 1;
  }

  Wire.begin(I2C_SLAVE_ADDR);
  TWAR = (I2C_SLAVE_ADDR << 1) | 1;  // listen for broadcasts -- all i2c messages are sent using "general calls"
  Wire.onReceive(receiveEvent);
}

// i2c handler
void receiveEvent (int howMany) {
  byte data[28];
  
  Wire.readBytes(data, howMany);
  if (howMany == 7 and data[0] == 0xe3 and data[1] == 0x20) {
    byte reg = data[3];
    if (reg < MAX_REG) {
      byte idx = registers[reg];
      if (idx) {
        idx --;
        values[idx] = (data[4] << 8) + data[5];
        changed |= (1 << idx);
      }
    }
  }

  if (howMany == 28) {
    int8_t power_vent = (bool)(data[26] & 0x01);
    int8_t scheduled = (bool)(data[15] & 0x01);
    if (power_vent != stat.power_vent) {
      stat.power_vent = power_vent;
      stat_changed |= CHANGED_POWER_VENT;
    }
    if (scheduled != stat.scheduled) {
      stat.scheduled = scheduled;
      stat_changed |= CHANGED_SCHEDULED;
    }

    // only read fan level when the "home screen" is shown
    if ((data[20] & 0x04) or (bool)(data[19] & 0x10)) {
      byte digit = data[8];
      int8_t level;
      if (digit == DIGIT_1) {level = 1;}
      else if (digit == DIGIT_2) {level = 2;}
      else if (digit == DIGIT_3) {level = 3;}
      else {level = 0;}
      if (level != stat.level) {
        stat.level = level;
        stat_changed |= CHANGED_LEVEL;
      }
    }
  }
}

byte crc8(byte crc, byte *data, int len) {
  while (len--) {crc = crc8_push_byte(crc, *data++);}
  return crc;
}

byte crc8_push_byte(byte crc, byte data) {
  crc ^= data;
  for (int i = 0; i < 8; i++) {
    if ((crc & 0x80) != 0) {
      crc = (byte)((crc << 1) ^ CRC_POLY);
    } else {
      crc <<= 1;
    }
  }
  return crc;
}

void send_button(byte code, byte count) {
  digitalWrite (LED_BUILTIN, ledVal ^= 1);
  button_packet[5] = code;
  button_packet[6] = crc8_push_byte(0, code);
  for (byte i = 0; i < count; i ++) {
    if (i != 0) {delay(WHEEL_DELAY);}
    send_cmd(button_packet);
  }
  digitalWrite (LED_BUILTIN, ledVal ^= 1);
}

void send_cmd(byte * cmd) {
  Wire.beginTransmission(0);  // start i2c broadcast (general call)
  Wire.write(cmd, 7);
  Wire.endTransmission();
}

void loop() {
  uint32_t now = millis();

  // send changed measurements to KNX
  if (now - last >= MEASUREMENT_INTERVAL) {
    last = now;
    long mask = changed;
    changed = 0;
    for (byte i = 0; i < N_MEAS; i ++) {
      if (mask & 1) {
        Measurement mmnt = measurements[i];
        String ga = mmnt.ga;
        #ifdef PRINT_SERIAL
        Serial.print(ga);
        Serial.print(" ");
        Serial.println((float) values[i] / mmnt.scale);
        #endif
        knx.groupWrite2ByteFloat(ga, (float) values[i] / mmnt.scale);
      }
      mask >>= 1;
    }
  }

  int8_t stat_mask = stat_changed;
  stat_changed = 0;
  if (stat_mask & CHANGED_POWER_VENT) {  // power_vent
    #ifdef PRINT_SERIAL
    Serial.print("POWER VENT ");
    Serial.println(stat.power_vent);
    #endif
    knx.groupWriteBool(KNX_GA_POWER_STAT, stat.power_vent);
  }
  if (stat_mask & CHANGED_SCHEDULED) {  // scheduled
    #ifdef PRINT_SERIAL
    Serial.print("SCHEDULED ");
    Serial.println(stat.scheduled);
    #endif
    knx.groupWriteBool(KNX_GA_SCHEDULED_STAT, stat.scheduled);
  }
  if (stat_mask & CHANGED_LEVEL) {  // level
    #ifdef PRINT_SERIAL
    Serial.print("LEVEL ");
    Serial.println(stat.level);
    #endif
    knx.groupWrite1ByteInt(KNX_GA_LEVEL_STAT, stat.level);
  }

  // read KNX telegrams and adjust targets
  if (Serial1.available() > 0) {
    KnxTpUartSerialEventType eType = knx.serialEvent();
    if (eType == KNX_TELEGRAM) {
      KnxTelegram* telegram = knx.getReceivedTelegram();

      String target =
        String(0 + telegram->getTargetMainGroup()) + "/" +
        String(0 + telegram->getTargetMiddleGroup()) + "/" +
        String(0 + telegram->getTargetSubGroup());

      if (telegram->getCommand() == KNX_COMMAND_WRITE) {
        if (target == KNX_GA_LEVEL_SET) {
          int8_t value = telegram->get1ByteIntValue();
          if ((value >= 0) and (value <= 2)) {target_level = value;}
        } else if (target == KNX_GA_POWER_SET) {
          if (telegram->getBool() != stat.power_vent) {send_button(BUTTON_POWER_VENT, 1);}
        } else if (target == KNX_GA_SCHEDULED_SET) {
          bool value = telegram->getBool();
          if ((bool)value != (bool)stat.scheduled) {
            if (value) {set_scheduled = true;}
            else {
              set_scheduled = false;
              target_level = stat.level;
            }
          }
        }
      }
    }
  }

  // to get to the requested targets, send one level change at a time and wait BUTTON_DELAY ms before sending the next
  if (now - last_button >= BUTTON_DELAY) {
    if ((!stat.scheduled and (target_level == stat.level)) or (set_scheduled and stat.scheduled)) {
      target_level = -1;
      set_scheduled = false;
    } else if ((target_level > -1) or set_scheduled) {
      byte code;

      if (set_scheduled) {
        code = BUTTON_UP;
      } else if ((stat.scheduled == 1) or (target_level < stat.level)) {
        code = BUTTON_DOWN;
      } else if (target_level > stat.level) {
        code = BUTTON_UP;
      }
      send_button(code, 2);
    }
    last_button = millis();
  }
}
