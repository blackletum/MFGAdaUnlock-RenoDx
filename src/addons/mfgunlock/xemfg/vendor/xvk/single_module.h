// SPDX-License-Identifier: MIT
#pragma once
// 1.7 (plan F): the only part of MFG Unlocker's single_module the xvk layer
// uses. The XeMFG build implements it on its own log (native_xvk.cpp).
#include <Windows.h>

namespace single_module
{
void Log(const wchar_t* text) noexcept;
}
