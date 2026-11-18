#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "TaskbarCenter.h"

#include <oleacc.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <utility>

namespace TaskbarCenter {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kLayoutRetryInterval = 125;
constexpr UINT kIdleLayoutInterval = 750;
constexpr double kSlideDuration = 215.0;  // Milliseconds.
constexpr double kWatchAfterNotification = 550.0;
constexpr double kWatchAfterResize = 140.0;
constexpr double kFallbackDelay = 375.0;
constexpr double kBoundaryReleaseDelay = 120.0;
constexpr UINT kMoveFlags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER;

struct Placement {
    HWND window = nullptr;
    HWND parent = nullptr;
    int nativeX = 0;
    int appliedX = 0;
};

struct Slide {
    double from = 0;
    double velocity = 0;
    double started = 0;
    int target = 0;
    int minX = 0;
    int maxX = 0;
    bool active = false;
};

struct ButtonSpan {
    int left;
    int right;
    int target;
    int centeredX;
    int maxX;
    bool valid;
};

struct ReflowPosition {
    int nativeX;
    int adjustedX;
    bool pending;
};

struct PendingFallback {
    State state = State::Off;
    double since = 0;
};

struct LayoutBounds {
    HWND band = nullptr;
    RECT list{};
    RECT screen{};
    POINT bandOrigin{};
    int minLeft = 0;
    int maxRight = 0;
};

Placement g_placement{};
Slide g_slide{};
ButtonSpan g_lastSpan{};
ReflowPosition g_reflow{};
PendingFallback g_fallback{};
double g_watchUntil = 0;
double g_releaseAt = 0;
bool g_measureRequested = false;
bool g_layoutDeferred = false;
HWINEVENTHOOK g_layoutHook = nullptr;
HWINEVENTHOOK g_boundsHook = nullptr;
HWND g_taskList = nullptr;
bool g_measurePending = false;
State g_state = State::Off;
bool g_updating = false;
UINT g_layoutInterval = 0;

bool SetLayoutInterval(HWND taskList, UINT interval) {
    if (g_layoutInterval == interval) return true;
    if (!SetTimer(taskList, LayoutTimerId, interval, nullptr)) return false;
    g_layoutInterval = interval;
    return true;
}

bool ReadBounds(HWND window, RECT& bounds) {
    return window && GetWindowRect(window, &bounds) && !IsRectEmpty(&bounds);
}

bool ReadPosition(POINT& position) {
    RECT rect{};
    POINT origin{};
    if (!ReadBounds(g_placement.window, rect) || GetAncestor(g_placement.window, GA_PARENT) != g_placement.parent ||
        !ClientToScreen(g_placement.parent, &origin)) return false;
    position = {rect.left - origin.x, rect.top - origin.y};
    return true;
}

bool MoveList(POINT current, int x) {
    if (current.x != x &&
        !SetWindowPos(g_placement.window, nullptr, x, current.y, 0, 0, kMoveFlags)) return false;
    g_placement.appliedX = x;
    return true;
}

bool RestorePosition() {
    POINT current{};
    return g_placement.appliedX != g_placement.nativeX && ReadPosition(current) &&
           current.x == g_placement.appliedX && MoveList(current, g_placement.nativeX);
}

// A Hermite segment ends at rest. Carry its current speed into a new segment if
// Explorer changes the target while the app buttons are moving.
struct SlideSample {
    double x;
    double speed;
};

SlideSample SampleSlide(double now) {
    const double t = std::clamp((now - g_slide.started) / kSlideDuration, 0.0, 1.0);
    const double remaining = 1.0 - t;
    const double distance = g_slide.target - g_slide.from;
    const double x = g_slide.from + distance * t * t * (3 - 2 * t) +
                     g_slide.velocity * kSlideDuration * t * remaining * remaining;
    const double speed = distance / kSlideDuration * 6 * t * remaining +
                         g_slide.velocity * remaining * (1 - 3 * t);
    return {x, speed};
}

void SetSlideTarget(int current, int target, int minX, int maxX, double now) {
    if (g_slide.active && g_slide.target == target) {
        g_slide.minX = minX;
        g_slide.maxX = maxX;
        return;
    }
    if (current == target) {
        g_slide = {};
        return;
    }
    const double from = std::clamp(current, minX, maxX);
    const double distance = target - from;
    const double speedLimit = 2.5 * std::abs(distance) / kSlideDuration;
    const double velocity = g_slide.active
        ? std::clamp(SampleSlide(now).speed, -speedLimit, speedLimit)
        : 0.65 * distance / kSlideDuration;
    g_slide = {from, velocity, now, target, minX, maxX, true};
}

// Explorer can briefly report incomplete bounds while rebuilding its layout. Hold
// the last position until the same fallback is still necessary on a later read.
State ConfirmFallback(State state, double now, bool& moved) {
    if (g_fallback.state != state) g_fallback = {state, now};
    if (g_placement.appliedX != g_placement.nativeX && now - g_fallback.since < kFallbackDelay) {
        return State::Waiting;
    }
    g_slide = {};
    g_lastSpan = {};
    g_releaseAt = 0;
    moved = RestorePosition();
    return state;
}

bool ReadButtonSpan(HWND taskList, const RECT& listBounds, RECT& span, bool& overflow) {
    ComPtr<IAccessible> accessible;
    if (FAILED(AccessibleObjectFromWindow(taskList, OBJID_CLIENT, IID_PPV_ARGS(&accessible))) || !accessible) {
        return false;
    }

    long childCount = 0;
    if (FAILED(accessible->get_accChildCount(&childCount)) || childCount < 0) return false;
    for (long first = 0; first < childCount;) {
        std::array<VARIANT, 32> children{};
        const long count = std::min(childCount - first, static_cast<long>(children.size()));
        long read = 0;
        const HRESULT result = AccessibleChildren(accessible.Get(), first, count, children.data(), &read);
        bool complete = SUCCEEDED(result) && read == count;
        for (long index = 0; index < read; ++index) {
            VARIANT id = children[index];
            ComPtr<IAccessible> item;
            if (id.vt == VT_DISPATCH && id.pdispVal) {
                id.pdispVal->QueryInterface(IID_PPV_ARGS(&item));
                id = {};
                id.vt = VT_I4;
                id.lVal = CHILDID_SELF;
            } else if (id.vt == VT_I4) {
                item = accessible;
            }

            VARIANT role{};
            if (!item || FAILED(item->get_accRole(id, &role)) || role.vt == VT_EMPTY) {
                complete = false;
            } else if (role.vt == VT_I4 &&
                       (role.lVal == ROLE_SYSTEM_PUSHBUTTON || role.lVal == ROLE_SYSTEM_BUTTONMENU)) {
                VARIANT state{};
                if (FAILED(item->get_accState(id, &state)) || state.vt != VT_I4) {
                    complete = false;
                } else if (!(state.lVal & STATE_SYSTEM_INVISIBLE)) {
                    if (state.lVal & STATE_SYSTEM_OFFSCREEN) {
                        overflow = true;
                    } else {
                        long left, top, width, height;
                        if (FAILED(item->accLocation(&left, &top, &width, &height, id))) {
                            complete = false;
                        } else if (width > 0 && height > 0 && top < listBounds.bottom && top + height > listBounds.top) {
                            RECT bounds{left, top, left + width, top + height};
                            UnionRect(&span, &span, &bounds);
                        }
                    }
                }
                VariantClear(&state);
            }
            VariantClear(&role);
            VariantClear(&children[index]);
        }
        if (!complete) return false;
        first += read;
    }
    return true;
}

void PostLayout() {
    if (g_taskList && !g_measurePending) {
        g_measurePending = PostMessageW(g_taskList, LayoutMessage, 0, 0) != FALSE;
    }
}

bool IsLayoutBoundsWindow(HWND window) {
    const HWND tray = GetAncestor(g_taskList, GA_ROOT);
    if (!tray || !window) return false;
    if (window == tray || window == GetAncestor(g_taskList, GA_PARENT)) return true;
    if (GetAncestor(window, GA_PARENT) != tray) return false;

    wchar_t name[32];
    return GetClassNameW(window, name, ARRAYSIZE(name)) &&
           (std::wcscmp(name, L"TrayNotifyWnd") == 0 || std::wcscmp(name, L"DynamicContent2") == 0);
}

void CALLBACK OnLayoutEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (event != EVENT_OBJECT_LOCATIONCHANGE && window == g_taskList &&
        (object == OBJID_CLIENT || object == OBJID_WINDOW)) {
        PostLayout();
    } else if (object == OBJID_WINDOW && child == CHILDID_SELF && IsLayoutBoundsWindow(window)) {
        PostLayout();
    } else if (g_layoutDeferred && object == OBJID_WINDOW && child == CHILDID_SELF &&
               (event == EVENT_OBJECT_DESTROY || (event == EVENT_OBJECT_HIDE && GetAncestor(window, GA_ROOT) == window))) {
        // A preview on the taskbar thread can have just released the layout.
        PostLayout();
    }
}

State ReadLayout(HWND taskList, LayoutBounds& layout) {
    const HWND tray = GetAncestor(taskList, GA_ROOT);
    layout.band = GetAncestor(taskList, GA_PARENT);
    if (!tray || !layout.band) return State::Unavailable;

    const HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    MONITORINFO monitor{sizeof(monitor)};
    RECT trayBounds{}, bandBounds{}, notifyBounds{};
    if (!ReadBounds(tray, trayBounds) || !ReadBounds(layout.band, bandBounds) ||
        !ReadBounds(taskList, layout.list) || !ReadBounds(notify, notifyBounds) ||
        !ClientToScreen(layout.band, &layout.bandOrigin) ||
        !GetMonitorInfoW(MonitorFromWindow(tray, MONITOR_DEFAULTTONULL), &monitor)) {
        return State::Unavailable;
    }

    layout.screen = monitor.rcMonitor;
    if (trayBounds.right - trayBounds.left <= trayBounds.bottom - trayBounds.top ||
        trayBounds.top < (layout.screen.top + layout.screen.bottom) / 2 ||
        (GetWindowLongPtrW(tray, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)) {
        return State::Unavailable;
    }
    if (trayBounds.top >= layout.screen.bottom - 2 || !IsWindowVisible(taskList)) return State::Waiting;

    layout.minLeft = std::max({layout.screen.left, trayBounds.left, bandBounds.left});
    layout.maxRight = std::min({layout.screen.right, notifyBounds.left, bandBounds.right});
    const HWND news = FindWindowExW(tray, nullptr, L"DynamicContent2", nullptr);
    RECT newsBounds{};
    if (IsWindowVisible(news) && ReadBounds(news, newsBounds) && newsBounds.left > layout.minLeft) {
        layout.maxRight = std::min(layout.maxRight, static_cast<int>(newsBounds.left));
    }
    return State::Centered;
}

bool ConstrainReflowX(HWND taskList, const WINDOWPOS& position, int& x) {
    if (taskList != g_placement.window || !g_lastSpan.valid ||
        x == g_placement.appliedX || g_placement.appliedX == g_placement.nativeX) {
        return false;
    }

    LayoutBounds layout{};
    if (ReadLayout(taskList, layout) != State::Centered || layout.band != g_placement.parent) return false;
    const int width = position.flags & SWP_NOSIZE ? layout.list.right - layout.list.left : position.cx;
    const int minX = layout.minLeft - layout.bandOrigin.x - g_lastSpan.left;
    const int maxX = layout.maxRight - layout.bandOrigin.x - g_lastSpan.right;
    if (g_lastSpan.right > width || minX > maxX) return false;

    // Keep the current X while it fits. If the shell is going to move the row
    // anyway, allow only the movement needed to fit it in the available space.
    x = std::clamp(g_placement.appliedX, minX, maxX);
    return true;
}

void RecordReflow(int nativeX, int adjustedX, int previousX) {
    g_placement.nativeX = nativeX;
    g_placement.appliedX = adjustedX;
    if (adjustedX != previousX) {
        g_slide = {};
        g_releaseAt = 0;
        // Remember a tighter boundary even if motion is paused during a drag.
        if (adjustedX < previousX) g_lastSpan.maxX = std::min(g_lastSpan.maxX, adjustedX);
    }
}

State Arrange(HWND taskList, double now, bool& moved) {
    LayoutBounds layout{};
    const State state = ReadLayout(taskList, layout);
    if (state == State::Unavailable) return ConfirmFallback(state, now, moved);
    if (state == State::Waiting) {
        g_fallback = {};
        return state;
    }

    const auto& listBounds = layout.list;
    const auto& screen = layout.screen;
    const HWND band = layout.band;
    const POINT bandOrigin = layout.bandOrigin;
    const int current = listBounds.left - bandOrigin.x;
    if (g_placement.window != taskList || g_placement.parent != band || current != g_placement.appliedX) {
        // Explorer can reposition the child when its band is rebuilt.
        g_placement = {taskList, band, current, current};
        g_fallback = {};
        g_slide = {};
        g_lastSpan = {};
        g_releaseAt = 0;
    }

    RECT buttons{};
    bool overflow = false;
    if (!ReadButtonSpan(taskList, listBounds, buttons, overflow)) {
        return ConfirmFallback(State::Unavailable, now, moved);
    }
    if (overflow) return ConfirmFallback(State::NoSpace, now, moved);
    if (IsRectEmpty(&buttons)) return ConfirmFallback(State::Centered, now, moved);
    if (buttons.left < listBounds.left || buttons.right > listBounds.right) {
        return ConfirmFallback(State::NoSpace, now, moved);
    }
    const int width = buttons.right - buttons.left;
    if (layout.maxRight - layout.minLeft < width) return ConfirmFallback(State::NoSpace, now, moved);
    g_fallback = {};

    const int buttonOffset = buttons.left - listBounds.left;
    const int origin = bandOrigin.x + buttonOffset;
    const int minX = layout.minLeft - origin;
    const int maxX = layout.maxRight - width - origin;
    const int centered = screen.left + (screen.right - screen.left - width) / 2 - origin;
    int target = std::clamp(centered, minX, maxX);
    const ButtonSpan previous = g_lastSpan;
    const int right = buttons.right - listBounds.left;
    const bool sameButtons = previous.valid && previous.left == buttonOffset && previous.right == right;

    // Delay a rightward move only when an unchanged row gets its space back.
    // Another boundary change restarts the delay until the available width settles.
    if (!sameButtons || previous.centeredX != centered || target <= current || current < minX) {
        g_releaseAt = 0;
    } else if (maxX != previous.maxX &&
               (g_releaseAt != 0 || (maxX > previous.maxX && previous.maxX < centered))) {
        g_releaseAt = now + kBoundaryReleaseDelay;
        g_watchUntil = std::max(g_watchUntil, g_releaseAt + kWatchAfterResize);
    }
    if (now < g_releaseAt) target = current;
    else g_releaseAt = 0;

    const ButtonSpan span{buttonOffset, right, target, centered, maxX, true};
    g_lastSpan = span;
    if (previous.valid && !sameButtons) g_watchUntil = std::max(g_watchUntil, now + kWatchAfterResize);

    // Explorer animates the width of the departing button itself. Follow small right-edge
    // changes directly, so each width update does not start another 215 ms transition.
    const int dx = target - previous.target;
    const int maxFollowStep = std::max(1, MulDiv(5, GetDpiForWindow(taskList), 96));
    if (previous.valid && previous.left == span.left && span.right < previous.right &&
        dx > 0 && dx <= maxFollowStep) {
        if (g_slide.active && g_slide.target == previous.target) {
            g_slide.from += dx;
            g_slide.target = target;
            g_slide.minX = minX;
            g_slide.maxX = maxX;
            return State::Centered;
        }
        if (current == previous.target) {
            moved = MoveList({current, listBounds.top - bandOrigin.y}, target);
            return moved ? State::Centered : State::Waiting;
        }
    }

    SetSlideTarget(current, target, minX, maxX, now);
    return State::Centered;
}

}  // namespace

bool Start(HWND taskList) {
    if (g_state != State::Off) return true;
    if (!SetLayoutInterval(taskList, kLayoutRetryInterval)) return false;
    g_taskList = taskList;
    g_layoutHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER, nullptr, OnLayoutEvent,
                                  GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    g_boundsHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnLayoutEvent,
                                  GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    g_state = State::Waiting;
    PostLayout();
    return true;
}

void OnPositionChanging(HWND taskList, WINDOWPOS& position) {
    if (g_updating || taskList != g_taskList) return;

    g_reflow = {};
    int x = position.x;
    if (!(position.flags & SWP_NOMOVE) && ConstrainReflowX(taskList, position, x)) {
        if (x == g_placement.appliedX) {
            g_placement.nativeX = position.x;
        } else {
            g_reflow = {position.x, x, true};
        }
        position.x = x;
    }
}

void OnPositionChanged(HWND taskList, const WINDOWPOS& position) {
    if (g_updating || taskList != g_taskList) return;

    const auto reflow = std::exchange(g_reflow, {});
    if ((position.flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE)) return;

    if (!(position.flags & SWP_NOMOVE)) {
        POINT current{};
        if (ReadPosition(current)) {
            const int previousX = g_placement.appliedX;
            if (reflow.pending && !(position.flags & SWP_NOSENDCHANGING) && current.x == reflow.adjustedX) {
                RecordReflow(reflow.nativeX, current.x, previousX);
            } else {
                // Some native moves skip WM_WINDOWPOSCHANGING. Correct those
                // synchronously so the native left-aligned X has no animation frame.
                int x = current.x;
                if (ConstrainReflowX(taskList, position, x)) {
                    g_updating = true;
                    if (MoveList(current, x)) RecordReflow(current.x, x, previousX);
                    g_updating = false;
                }
            }
        }
    }
    PostLayout();
}

void OnButtonRemoval(HWND taskList) {
    if (taskList == g_taskList) PostLayout();
}

void RequestLayout(double now, bool notified) {
    if (notified) g_measurePending = false;
    if (g_state == State::Off) return;
    if (notified) g_watchUntil = now + kWatchAfterNotification;
    g_measureRequested = true;
}

bool NeedsFrame(double now) {
    return g_state != State::Off &&
           (g_measureRequested || (!g_layoutDeferred && (now < g_watchUntil || g_slide.active)));
}

bool HasDeferredLayout() {
    return g_layoutDeferred;
}

bool Advance(HWND taskList, double now, bool canMove) {
    if (g_state == State::Off || g_updating) return false;
    const bool measure = std::exchange(g_measureRequested, false) || now < g_watchUntil || g_layoutDeferred;
    if (!canMove) {
        g_layoutDeferred = measure || g_slide.active;
        g_slide = {};
        g_fallback = {};
        SetLayoutInterval(taskList, kLayoutRetryInterval);
        return false;
    }
    if (std::exchange(g_layoutDeferred, false)) g_watchUntil = std::max(g_watchUntil, now + kWatchAfterResize);

    g_updating = true;
    bool moved = false;
    if (measure) {
        const State state = Arrange(taskList, now, moved);
        if (g_state != State::Off) {
            if (state != State::Centered) g_slide = {};
            g_state = state;
        }
    }

    if (g_slide.active && g_state != State::Off) {
        POINT current{};
        if (!ReadPosition(current) || current.x != g_placement.appliedX) {
            g_slide = {};
            PostLayout();
        } else {
            const auto sample = SampleSlide(now);
            const int x = std::clamp(static_cast<int>(std::lround(sample.x)), g_slide.minX, g_slide.maxX);
            const bool applied = MoveList(current, x);
            if (!applied || now - g_slide.started >= kSlideDuration) g_slide = {};
            if (!applied && g_state != State::Off) {
                g_state = State::Waiting;
                PostLayout();
            }
            moved |= applied && x != current.x;
        }
    }
    g_updating = false;
    if (g_state != State::Off) {
        const bool centeredWithEvents = g_state == State::Centered && g_layoutHook && g_boundsHook;
        SetLayoutInterval(taskList, centeredWithEvents ? kIdleLayoutInterval : kLayoutRetryInterval);
    }
    return moved;
}

void Stop(HWND taskList) {
    if (g_state == State::Off) return;
    g_state = State::Off;
    KillTimer(taskList, LayoutTimerId);
    g_layoutInterval = 0;
    if (g_layoutHook) UnhookWinEvent(std::exchange(g_layoutHook, nullptr));
    if (g_boundsHook) UnhookWinEvent(std::exchange(g_boundsHook, nullptr));
    g_taskList = nullptr;
    g_measurePending = false;
    g_measureRequested = false;
    g_layoutDeferred = false;
    g_watchUntil = 0;
    g_releaseAt = 0;
    g_slide = {};
    g_lastSpan = {};
    g_reflow = {};
    g_fallback = {};
    const bool wasUpdating = std::exchange(g_updating, true);
    RestorePosition();
    g_placement = {};
    g_updating = wasUpdating;
}

bool IsUpdating() {
    return g_updating;
}

State Status() {
    return g_state;
}

}  // namespace TaskbarCenter
