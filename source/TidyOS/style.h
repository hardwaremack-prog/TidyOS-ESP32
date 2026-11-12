// TidyOS: apply the theme to FabGL widgets.  Usage:  auto b = S(new uiButton(...));
#pragma once
#include "core.h"

void applyStyle(fabgl::uiObject * o);

template <class W> inline W * S(W * w) { applyStyle(w); return w; }
