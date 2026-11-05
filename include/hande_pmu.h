#pragma once
// The AXP2101 power-chip settings applied right after power.begin() succeeds, so charging and battery readings
// behave the same on every boot.
// Include after XPowersLib.h (XPowersPMU must be the AXP2101 class).

__attribute__((always_inline)) inline void hande_pmu_apply_common(XPowersPMU& power) {
    // The Waveshare board has no battery thermistor on the TS pin. XPowersLib's own charge example: "It is necessary
    // to disable the detection function of the TS pin on the board without the battery temperature detection
    // function, otherwise it will cause abnormal charging."
    power.disableTSPinMeasure();
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableVbusVoltageMeasure();
    power.enableSystemVoltageMeasure();
    power.fuelGaugeControl(false, true);  // fuel gauge on (battery percentage); nothing written to the chip's ROM
    power.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);  // standard single-cell LiPo
}
