#ifdef TRIAC

void pilotTriacPeriod(float p) {
  Triac::setPeriod(p);
}

void pilotSetup() {
  Triac::setup(ZC_EI_PIN, TRIAC_PIN);
}

#else

void pilotSetup() {
}
#endif

const int MIN_POWER = 300;

void pilotLoop() {

  const int PUMP_POWER = 40;
  const byte MONITORING_UNTIL_SOC = 85; // %
  const byte BYPASS_BUFFER_SOC = 95; // %
  const int CONSUMPTION_POWER_LIMIT = 3900; // W
  const int BATTERY_MAX_CHARGING_POWER = 4000; // W
  const byte TOP_OSCILLATION_SOC = 97; // %
  const int TOP_OSCILLATION_DISCHARGE_LIMIT = -600; // W
  const int TOP_OSCILLATION_COUNTERMEASURE = -20; // W
  const int MIN_START_POWER = 500; // W
  const int BYPASS_MIN_START_POWER = BYPASS_POWER + 100;
  const int HEATING_PRIORITY_START_POWER = BYPASS_POWER + 250;
  const byte WAIT_FOR_IT_COUNT = 3;

  static byte waitForItCounter = 0; // to not react on short spikes

  switch (state) {
    case RegulatorState::MONITORING:
      break;
    case RegulatorState::REGULATING:
      if (!mainRelayOn) {
        state = RegulatorState::MONITORING;
      }
      break;
    default:
      return;
  }

  // handle heating strategy plan
  if (hourNow >= 12 && (powerPilotPlan == HEATING_PRIORITY_AM || powerPilotPlan == HEATING_DISABLED_AM)) {
    powerPilotPlan = BATTERY_PRIORITY;
  }
  if (powerPilotPlan == HEATING_DISABLED || powerPilotPlan == HEATING_DISABLED_AM) {
    if (heatingPower > 0) {
      heatingPower = 0;
      powerPilotRaw = 0;
    }
    return;
  }

  int pvChP = (pvChargingPower > 0) ? 0 : pvChargingPower; // as default take only negative charge (discharge)
  if (!pvBattCalib) { // at battery calibration. battery takes what is needed. surplus power can occur

  // check state of charge
  // * the Symo regulates the distribution of energy while the battery is charging.
  // usually we don't want to interfere with that (unless heating has priority),
  // but we must enter the regulation algorithm if heating is on.
  // * if the production is larger then the battery can use for charging, surplus can occur
  if (powerPilotPlan != HEATING_PRIORITY_AM && pvSOC < MONITORING_UNTIL_SOC
      && heatingPower == 0 && pvChargingPower < BATTERY_MAX_CHARGING_POWER)
    return;

  // in bypass mode, we can use battery as buffer to ignore short PV shadows or small concurrent consumption
  if (bypassRelayOn && pvSOC > BYPASS_BUFFER_SOC && !pvBattCalib && inverterAC < CONSUMPTION_POWER_LIMIT)
    return;

  // battery charge/discharge control
  if ((powerPilotPlan == HEATING_PRIORITY_AM || pvSOC > BYPASS_BUFFER_SOC)
      && (heatingPower + pvChargingPower) > HEATING_PRIORITY_START_POWER
      && (inverterAC - heatingPower + HEATING_PRIORITY_START_POWER) < CONSUMPTION_POWER_LIMIT) {
    pvChP = pvChargingPower; // take this big charging as available (not everything will be used and/or the battery can charge later)
  } else if (pvSOC < BYPASS_BUFFER_SOC && pvChargingPower < (BATTERY_MAX_CHARGING_POWER - TOP_OSCILLATION_DISCHARGE_LIMIT)) {
    heatingPower = 0; // stop heating, which started by taking the big charging
    powerPilotRaw = 0;
    waitForItCounter = 0;
    return;
  } else if (pvSOC > TOP_OSCILLATION_SOC && pvChP < TOP_OSCILLATION_COUNTERMEASURE
      && (pvChP + meterPower) > TOP_OSCILLATION_DISCHARGE_LIMIT) { // tolerated discharge
    pvChP = TOP_OSCILLATION_COUNTERMEASURE; // almost ignore discharge. full battery can do a little discharging
  }

  } // close if (!pvBattCalib) {

  // sum available power
  short availablePower = heatingPower + meterPower + pvChP - (mainRelayOn ? 0 : PUMP_POWER);
  if (heatingPower == 0 && availablePower < MIN_START_POWER) {
    waitForItCounter = 0;
    return;
  }
  if (bypassRelayOn && availablePower > BYPASS_POWER)
    return;
  if (availablePower < MIN_POWER) {
    heatingPower = 0;
    powerPilotRaw = 0;
    waitForItCounter = 0;
    return;
  }

  // bypass the power regulator module for max power
  boolean bypass = availablePower > (bypassRelayOn ? BYPASS_POWER : BYPASS_MIN_START_POWER);

  // not switch relays for short spikes
  if ((!mainRelayOn || (!bypassRelayOn && bypass)) && waitForItCounter < WAIT_FOR_IT_COUNT) {
    waitForItCounter++;
    return;
  }
  waitForItCounter = 0;

  if (!turnMainRelayOn())
    return;
  state = RegulatorState::REGULATING;

  // set heating power
#ifdef TRIAC
  float r = bypass ? 0 : power2TriacPeriod(availablePower);
  pilotTriacPeriod(r);
  powerPilotRaw = r * 1000; // to log
#else
  powerPilotRaw = bypass ? 0 : power2pwm(availablePower);
  analogWrite(PWM_PIN, powerPilotRaw);
#endif

  if (bypass != bypassRelayOn) {
    waitZeroCrossing();
    digitalWrite(BYPASS_RELAY_PIN, bypass);
    bypassRelayOn = bypass;
  }
  heatingPower = bypass ? BYPASS_POWER : (availablePower > MAX_POWER) ? MAX_POWER : availablePower;
}

void powerPilotSetPlan(byte plan) {
  if (powerPilotPlan == plan)
    return;
  eventsWrite(POWERPILOT_PLAN_EVENT, plan, powerPilotPlan);
  powerPilotPlan = plan;
}

void powerPilotStop() {
#ifdef TRIAC
  pilotTriacPeriod(0);
#else
  analogWrite(PWM_PIN, 0);
#endif
  powerPilotRaw = 0;
}

float power2TriacPeriod(int power) {
  const float POWER2PERIOD_SHIFT = 0.08;
  const float POWER2PERIOD_KOEF = 0.5;

  if (power < MIN_POWER)
    return 0.0;
  if (power >= MAX_POWER)
    return 0.95;
  float ratio = (float) power / MAX_POWER;
  return POWER2PERIOD_SHIFT + POWER2PERIOD_KOEF * asin(sqrt(ratio));
}

unsigned short power2pwm(int power) {

  const float MAX_CURRENT = 8.8;
  const float POWER2PWM_KOEF = 30.0;
  const float PF_ANGLE_SHIFT = 0.08 * PI;
  const float PF_ANGLE_INTERVAL = 0.33 * PI;
  const unsigned short MIN_PWM = 190;

  if (power < MIN_POWER)
    return 0;
  unsigned short res;
  if (power >= MAX_POWER) {
    res = 1023;
  } else {
    float current = (float) power / voltage;
    float ratio = current / MAX_CURRENT;
    res = MIN_PWM + POWER2PWM_KOEF * current / cos(PF_ANGLE_SHIFT + (ratio * PF_ANGLE_INTERVAL));
  }
#ifdef __AVR__
  res /= 4;
#endif
  return res;
}
