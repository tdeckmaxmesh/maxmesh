#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <TDeckMaxBoard.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"
#ifdef DISPLAY_CLASS
  #include <TDeckMaxDisplay.h>
  #include <MaxKeyboard.h>
  #include <MaxTouch.h>
#endif

extern TDeckMaxBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  extern MaxKeyboard keyboard;
  extern MaxTouch touch;
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
