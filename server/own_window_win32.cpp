// Win32 implementation of OwnWindow (see own_window.hpp). own_window.cpp
// holds the Wayland implementation instead; CMakeLists.txt picks whichever
// of the two the server target is built with, never both.

// windows.h's default (non-lean) mode drags in the legacy winsock.h, which
// conflicts with wire.hpp's winsock2.h if windows.h is reached first in this
// translation unit; defining this before the very first include keeps that
// from happening regardless of what pulls windows.h in first (see
// client/wsi.cpp lines 20-31).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <future>
#include <mutex>

#include "own_window.hpp"

namespace {

const wchar_t kWindowClassName[] = L"vulkan_remoting_own_window";

// RegisterClassExW fails if called twice with the same class name, and more
// than one OwnWindow can exist at once (see session.hpp's create_own_window),
// each spawning its own pump thread - so this runs at most once per process
// regardless of how many OwnWindow instances ask for it.
void register_window_class_once(HINSTANCE hinstance) {
    static std::once_flag once;
    std::call_once(once, [hinstance] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT {
            // WM_DESTROY -> PostQuitMessage is what lets the pump thread's
            // GetMessage loop (and therefore the thread itself) exit once
            // the destructor below asks the window to close.
            if (msg == WM_DESTROY) {
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        };
        wc.hInstance = hinstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClassName;
        RegisterClassExW(&wc);
    });
}

}  // namespace

OwnWindow::~OwnWindow() {
    // PostMessage, not SendMessage: this runs on whichever thread is serving
    // the connection that owns this OwnWindow (see session.hpp's
    // create_own_window), never on the pump thread itself, so this must not
    // block waiting for the pump thread to process it.
    if (hwnd_) PostMessage(hwnd_, WM_CLOSE, 0, 0);
    // Nothing to join if create() never got as far as starting the thread,
    // or if it already exited after a failed CreateWindowExW - joinable()
    // covers both without this hanging either way.
    if (pump_thread_.joinable()) pump_thread_.join();
}

bool OwnWindow::create() {
    hinstance_ = GetModuleHandleW(nullptr);

    // The pump thread signals through this once the HWND exists (or once
    // creation has failed) - create() must not return before then, since the
    // handlers_wsi.cpp caller immediately hands hwnd()/hinstance() to
    // vkCreateWin32SurfaceKHR.
    std::promise<bool> ready;
    std::future<bool> ready_future = ready.get_future();

    pump_thread_ = std::thread([this, &ready] {
        register_window_class_once(hinstance_);

        hwnd_ = CreateWindowExW(0, kWindowClassName, L"vulkan_remoting", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 nullptr, nullptr, hinstance_, nullptr);
        if (!hwnd_) {
            fprintf(stderr, "server: own_window: CreateWindowExW failed (%lu)\n",
                    static_cast<unsigned long>(GetLastError()));
            ready.set_value(false);
            return;
        }
        ShowWindow(hwnd_, SW_SHOW);
        ready.set_value(true);

        // Without this pump running, Windows marks the window "not
        // responding" even while it is presenting fine - a window's message
        // queue has to be serviced from the thread that created it, which is
        // exactly this thread and none of the session threads that are busy
        // serving client connections instead.
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });

    const bool ok = ready_future.get();
    if (!ok && pump_thread_.joinable()) pump_thread_.join();
    return ok;
}

// The Win32 window records its size from WM_SIZE inside the pump thread's
// own message loop, so there is nothing for a session thread to dispatch
// here - unlike the Wayland path, where the configure would otherwise sit
// unread (see own_window.hpp). Kept so both platforms present the same API.
void OwnWindow::pump() {}
