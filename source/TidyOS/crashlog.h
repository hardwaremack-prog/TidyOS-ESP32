// TidyOS crash recorder (see crashlog.cpp)
#pragma once
#include <string>
#include <stdint.h>
void        crashBegin();                         // first thing in setup()
std::string crashText();                          // "" if the last run didn't crash
void        crashSave(const std::string & path);  // append it to a file
extern volatile uint32_t gHeapSnap, gMinHeapSnap;
