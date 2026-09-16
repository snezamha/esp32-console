#include "board.h"

#include <Wire.h>

#include "../../config.h"
#include "../ui/ui.h"

void Board::Initialize() {
  InitializePower();
  InitializeDisplay();
  InitializeTouch();

  boot_button_ = new Button(BOOT_BUTTON_GPIO);
  boot_button_->SetLongPressTime(3000);
  boot_button_->OnClick([this]() {
    if (boot_click_handler_) boot_click_handler_();
  });
  boot_button_->OnLongPress([this]() {
    if (factory_reset_handler_) factory_reset_handler_();
  });
}

void Board::InitializePower() {
  power_ok_ = power_.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA_PIN, IIC_SCL_PIN);
  if (!power_ok_) return;

  power_.enableBattDetection();
  power_.enableBattVoltageMeasure();
  power_.enableVbusVoltageMeasure();
  power_.enableSystemVoltageMeasure();
  power_.disableTSPinMeasure();

  // DC1 powers the board logic, ALDO1/ALDO2 power the panel and touch controller. XPowersLib's
  // begin() only opens the I2C link; without this the rails stay at whatever the chip happened to
  // power up with (often off), and the display never lights up.
  power_.setDC1Voltage(3300);
  power_.enableDC1();
  power_.setALDO1Voltage(3300);
  power_.enableALDO1();
  power_.setALDO2Voltage(3300);
  power_.enableALDO2();

  // Hardware cutoff at 10s so the console's "poweroff" command / boot-button long-press (handled
  // in software) always gets a chance to run its own shutdown sequence first.
  power_.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
  power_.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
  power_.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  power_.clearIrqStatus();
}

void Board::InitializeDisplay() {
  static Arduino_DataBus* bus =
      new Arduino_ESP32QSPI(DISPLAY_CS_PIN, DISPLAY_CLK_PIN, DISPLAY_D0_PIN, DISPLAY_D1_PIN, DISPLAY_D2_PIN, DISPLAY_D3_PIN);
  display_ = new Arduino_CO5300(bus, DISPLAY_RST_PIN, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 22, 0, 0, 0);
  display_->begin();
  display_->fillScreen(RGB565_BLACK);
}

void Board::InitializeTouch() {
  iic_bus_ = std::make_shared<Arduino_HWIIC>(IIC_SDA_PIN, IIC_SCL_PIN, &Wire);
  touch_.reset(new Arduino_FT3x68(iic_bus_, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TOUCH_INT_PIN, nullptr));
  touch_->begin();
}

void Board::Loop() { boot_button_->Tick(); }

bool Board::GetBatteryLevel(int& level, bool& charging, bool& discharging) {
  if (!power_ok_ || !power_.isBatteryConnect()) {
    level = -1;
    charging = false;
    discharging = false;
    return false;
  }
  level = power_.getBatteryPercent();
  charging = power_.isCharging();
  discharging = !power_.isVbusIn();
  return true;
}

void Board::PowerOff() {
  if (power_ok_) power_.shutdown();
}

void Board::ShowNotification(const std::string& text, int duration_ms) { ui_notify(text, duration_ms); }
