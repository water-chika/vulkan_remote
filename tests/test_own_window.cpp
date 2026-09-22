// Tests for the server-owned window's compositor callbacks.
//
// These two callbacks are the whole of what the compositor can ever tell the
// server about a window it owns, and both shipped broken: toplevel_configure
// stored the new width but not the new height (the store sat on the right of
// a short-circuiting ||, so it never ran once the width had changed), and
// toplevel_close was an empty function, so closing the window did nothing at
// all. Neither could be caught by anything we had - every other test in this
// repo stops at the wire layer - and both needed a live Win32 client against
// a real compositor to notice.
//
// They do not need one. The callbacks are static, take their instance
// through void* data the way libwayland passes it, and touch nothing but
// plain members, so they can be called directly with no display connection,
// no compositor and no GPU. That is what this file does.

#include "own_window.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Dragging a corner changes width and height in one configure. This is the
// case that regressed: asserting on the height is the entire point, because
// the width was always right.
void configure_records_both_axes() {
    OwnWindow window;
    OwnWindow::toplevel_configure(&window, nullptr, 800, 600, nullptr);
    check(window.width() == 800, "configure records width");
    check(window.height() == 600, "configure records height");
    check(window.take_resized(), "first configure reports a resize");

    OwnWindow::toplevel_configure(&window, nullptr, 1024, 768, nullptr);
    check(window.width() == 1024, "second configure updates width");
    check(window.height() == 768, "second configure updates height");
    check(window.take_resized(), "second configure reports a resize");
}

// A change in one axis alone still has to be recorded, and still counts.
void configure_records_a_single_axis() {
    OwnWindow window;
    OwnWindow::toplevel_configure(&window, nullptr, 800, 600, nullptr);
    window.take_resized();

    OwnWindow::toplevel_configure(&window, nullptr, 800, 601, nullptr);
    check(window.height() == 601, "height-only change is recorded");
    check(window.take_resized(), "height-only change reports a resize");

    OwnWindow::toplevel_configure(&window, nullptr, 801, 601, nullptr);
    check(window.width() == 801, "width-only change is recorded");
    check(window.take_resized(), "width-only change reports a resize");
}

// take_resized() is a take: the flag has to clear, or every later
// vkAcquireNextImageKHR would report VK_SUBOPTIMAL_KHR forever and the
// client would recreate its swapchain on every single frame.
void resize_flag_clears_on_read() {
    OwnWindow window;
    OwnWindow::toplevel_configure(&window, nullptr, 640, 480, nullptr);
    check(window.take_resized(), "resize is reported once");
    check(!window.take_resized(), "resize does not report twice");

    OwnWindow::toplevel_configure(&window, nullptr, 640, 480, nullptr);
    check(!window.take_resized(), "an unchanged size is not a resize");
}

// Zero means "you choose", not a size, and the compositor sends exactly that
// for the initial configure. Mistaking it for a resize would hand the client
// a 0x0 extent.
void configure_ignores_a_zero_size() {
    OwnWindow window;
    OwnWindow::toplevel_configure(&window, nullptr, 0, 0, nullptr);
    check(window.width() == 0 && window.height() == 0, "zero configure stores nothing");
    check(!window.take_resized(), "zero configure is not a resize");

    OwnWindow::toplevel_configure(&window, nullptr, 500, 400, nullptr);
    OwnWindow::toplevel_configure(&window, nullptr, 500, 0, nullptr);
    check(window.height() == 400, "a zero axis does not overwrite a real one");
}

// The one that did nothing whatsoever: without this the window cannot be
// closed from the compositor at all.
void close_is_recorded_and_sticks() {
    OwnWindow window;
    check(!window.closed(), "a new window is not closed");

    OwnWindow::toplevel_close(&window, nullptr);
    check(window.closed(), "close is recorded");
    // Unlike the resize flag this must not clear on read: every swapchain
    // that asks afterwards needs the same answer, not just the first.
    check(window.closed(), "close still reads as closed");
}

}  // namespace

int main() {
    configure_records_both_axes();
    configure_records_a_single_axis();
    resize_flag_clears_on_read();
    configure_ignores_a_zero_size();
    close_is_recorded_and_sticks();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("own_window: all checks passed\n");
    return 0;
}
