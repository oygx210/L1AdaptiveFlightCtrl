#include "motors.h"

#include <string.h>

#include "eeprom.h"
#include "i2c.h"
#include "uart.h"


// =============================================================================
// Private data:

#define MOTORS_MAX (8)
#define MOTORS_BASE_ADDRESS (0x52)

enum BLCStatusCode
{
  BLC_STATUS_UNKNOWN = 0,
  BLC_STATUS_MISMATCH = 1,  // Arbitrary
  BLC_STATUS_STARTING = 40,
  BLC_STATUS_V3_FAST_READY = 248,
  BLC_STATUS_V3_READY = 249,
  BLC_STATUS_V2_READY = 250,
  BLC_STATUS_RUNNING_REDUNDANT = 254,
  BLC_STATUS_RUNNING = 255,  // V1 always gives this before motors are started
};

enum BLCFeatureBitfield
{
  BLC_FEATURE_EXTENDED_STATUS = 1<<0,
  BLC_FEATURE_V3 = 1<<1,
  BLC_FEATURE_20KHz = 1<<2,
};

enum BLCConfigBitfield
{
  BLC_BITFIELD_REVERSE_ROTATION = 1<<0,
  BLC_BITFIELD_START_PWM_1 = 1<<1,
  BLC_BITFIELD_START_PWM_2 = 1<<2,
  BLC_BITFIELD_START_PWM_3 = 1<<3,
};

struct MotorSetpoint
{
  uint8_t bits_11_to_3;
  uint8_t bits_2_to_0;
} __attribute__((packed)) setpoints_[MOTORS_MAX];

struct BLCConfig
{
  uint8_t revision;  // BLC configuration revision
  uint8_t mask;  // Settings mask
  uint8_t pwm_scaling;  // PWM saturation
  uint8_t current_limit;  // Current limit in A
  uint8_t temperature_limit;  // °C
  uint8_t current_scaling;  // Scale factor for current measurement
  uint8_t bitfield;
  uint8_t checksum;
} __attribute__((packed));

struct BLCStatus
{
  uint8_t current;  // x 0.1 A
  enum BLCStatusCode status_code;  // Also command limit when running?
  uint8_t temperature;  // °C (for V2 or greater, 0xFF otherwise)
  uint8_t rpm;  // TODO: verify units
  uint8_t extra;  // V3: Voltage, V2: mAh, V1: N/A
  uint8_t voltage;  // x 0.1V (V3 is limited to 255, V2 is only low-byte)
  uint8_t i2c_errors;  // V2 or greater
  uint8_t version_major;  // V2 or greater
  uint8_t version_minor;  // V2 or greater
} __attribute__((packed));

static volatile struct BLCStatus blc_status_[MOTORS_MAX] = { { 0 } };

static uint8_t blc_error_bitfield_ = 0x00;
static uint8_t blc_feature_bitfield_ = 0x00;
static uint8_t n_motors_ = 0;
static uint8_t setpoint_length_ = sizeof(uint8_t);
static uint8_t comms_in_progress_;  // Address to which communication is ongoing


// =============================================================================
// Private function declarations:

static void TxMotorSetpoint(uint8_t address);


// =============================================================================
// Accessors

uint8_t BLCErrorBitfield(void)
{
  return blc_error_bitfield_;
}


// =============================================================================
// Public functions:

// This function pings all of the possible brushless motor controller addresses
// by sending a 0 command. A response indicates that a controller (and hopefully
// also a motor) is present. The contents of the response indicate the type and
// features of the controller.
void DetectMotors(void)
{
  // TODO: if (motors_on) return;

  // Send a 0 command to each brushless controller address and record any
  // responses.
  uint8_t motors = 0;  // Bitfield representing motors present.
  uint8_t setpoint = 0;  // Do not command the motors to move
  enum BLCStatusCode blc_status_code = BLC_STATUS_UNKNOWN;
  for (uint8_t i = 0; i < MOTORS_MAX; i++)
  {
    I2CTxThenRx(MOTORS_BASE_ADDRESS + (i << 1), &setpoint, sizeof(setpoint),
      (volatile uint8_t *)&blc_status_[i], sizeof(struct BLCStatus));
    I2CWaitUntilCompletion();
    // I2C will give an error if there is no response.
    if (!I2CError())
    {
      motors |= (1 << i);  // Mark this motor as present

      // Check that all controllers are the same type.
      if (blc_status_code == BLC_STATUS_UNKNOWN)
        blc_status_code = blc_status_[i].status_code;
      else if (blc_status_[i].status_code != blc_status_code)
        blc_error_bitfield_ |= BLC_ERROR_INCONSISTENT_SETTINGS;
    }
  }

  // Identify additional features of the brushless controllers.
  switch (blc_status_code)
  {
    case BLC_STATUS_V3_FAST_READY:
      blc_feature_bitfield_ |= BLC_FEATURE_20KHz;
    case BLC_STATUS_V3_READY:
      blc_feature_bitfield_ |= BLC_FEATURE_V3;
    case BLC_STATUS_V2_READY:
      blc_feature_bitfield_ |= BLC_FEATURE_EXTENDED_STATUS;
      setpoint_length_ = sizeof(uint16_t);
      break;
    default:
      break;
  }

  // Check for missing or extra motors. Assumes that present motors have
  // contiguous addresses beginning with 0.
  n_motors_ = eeprom_read_byte(&eeprom.n_motors);
  if (((1 << n_motors_) - 1) & !motors)
    blc_error_bitfield_ |= BLC_ERROR_MISSING_MOTOR;
  if (motors & !((1 << n_motors_) - 1))
    blc_error_bitfield_ |= BLC_ERROR_EXTRA_MOTOR;
}

// -----------------------------------------------------------------------------
void SetMotorSetpoint(uint8_t address, uint16_t setpoint)
{
  if (address >= MOTORS_MAX) return;
  setpoints_[address].bits_2_to_0 = (uint8_t)setpoint & 0x7;
  setpoints_[address].bits_11_to_3 = (uint8_t)(setpoint >> 3);
}

// -----------------------------------------------------------------------------
void TxMotorSetpoints(void)
{
  comms_in_progress_ = n_motors_ - 1;
  TxMotorSetpoint(comms_in_progress_);
}


// =============================================================================
// Private functions:

static void TxNextMotorSetpoint(void)
{
  if (comms_in_progress_--) TxMotorSetpoint(comms_in_progress_);
}

// -----------------------------------------------------------------------------
static void TxMotorSetpoint(uint8_t address)
{
  I2CTxThenRxThenCallback(MOTORS_BASE_ADDRESS + (address << 1),
    (uint8_t *)&setpoints_[address], setpoint_length_,
    (volatile uint8_t *)&blc_status_[address], sizeof(struct BLCStatus),
    TxNextMotorSetpoint);
}
