#pragma once

#include <windows.h>

namespace TaskbarCenter {

inline constexpr UINT_PTR LayoutTimerId = 0x54444354;
inline constexpr UINT LayoutMessage = WM_APP + 0x544;

enum class State : LRESULT { Off = 0, Waiting = 1, Centered = 2, NoSpace = 3, Unavailable = 4 };

bool Start(HWND taskList);
void OnPositionChanging(HWND taskList, WINDOWPOS& position);
void OnPositionChanged(HWND taskList, const WINDOWPOS& position);
void OnButtonRemoval(HWND taskList);
void RequestLayout(double now, bool notified);
bool NeedsFrame(double now);
bool HasDeferredLayout();
bool Advance(HWND taskList, double now, bool canMove);
void Stop(HWND taskList);
State Status();
bool IsUpdating();

}  // namespace TaskbarCenter
