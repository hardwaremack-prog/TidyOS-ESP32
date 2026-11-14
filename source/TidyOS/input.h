// TidyOS input: Bluetooth LE keyboard + mouse host, key repeat, serial keyboard
#pragma once
#include <string>
#include "fabgl.h"
#include "fabui.h"

struct FoundDev {
  std::string name;
  std::string addr;
  bool isKbd;
  bool isMouse;
  bool bonded;
};

void inputBegin();                       // call before the display grabs RAM
void inputAttach(fabgl::uiApp * app, fabgl::Keyboard * kbd, fabgl::BitmappedDisplayController * dc);
void inputStartPairScan(int seconds);    // look for new keyboards / mice
bool inputScanning();
int  inputFoundCount();
bool inputGetFound(int i, FoundDev & out);
void inputPair(int i);                   // connect + pair found device i (async)
void inputForgetAll();
int  inputKeyboards();
int  inputMice();
std::string inputStatus();               // short human readable status
// start-up switches window (before the desktop exists): browser clicks and keys go here
void inputBootGrab(bool on);
bool inputBootClick(int & x, int & y);   // true once per click
int  inputBootKey();                     // HID usage of a pressed key, or -1
