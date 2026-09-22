#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <climits>
#include <initializer_list>

namespace RLA {

// Windows can continually generate legacy cursor motion while raw input is
// active. Read each motion type once per frame, then leave further motion
// queued for Windows to combine. Buttons, wheels, keys and window messages
// remain responsive. The dedicated input thread owns all WM_INPUT reports.
template<class Peek, class Dispatch>
bool PumpWindowMessages(MSG& message, Peek peek, Dispatch dispatch) {
    auto process = [&]() {
        if (message.message == WM_QUIT) return false;
        dispatch(message);
        return true;
    };
    for (UINT motion : {UINT(WM_MOUSEMOVE), UINT(WM_NCMOUSEMOVE)}) {
        if (peek(message, motion, motion) && !process()) return false;
    }
    for (unsigned remaining = 256; remaining > 0 &&
          (peek(message, 0, WM_NCMOUSEMOVE - 1) ||
           peek(message, WM_NCMOUSEMOVE + 1, WM_MOUSEMOVE - 1) ||
           peek(message, WM_MOUSEMOVE + 1, UINT_MAX)); --remaining) {
        if (!process()) return false;
    }
    return true;
}

} // namespace RLA
