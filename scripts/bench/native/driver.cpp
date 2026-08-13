#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static int ignoreXError(Display *, XErrorEvent *) {
    return 0;
}

static std::string title(Display *display, Window window) {
    const Atom property = XInternAtom(display, "_NET_WM_NAME", True);
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", True);
    if (property != None && utf8 != None) {
        Atom actualType = None;
        int actualFormat = 0;
        unsigned long count = 0;
        unsigned long remaining = 0;
        unsigned char *value = nullptr;
        if (XGetWindowProperty(display, window, property, 0, 4096, False, utf8, &actualType, &actualFormat, &count, &remaining, &value) ==
                Success &&
            value && actualFormat == 8) {
            const std::string result(reinterpret_cast<char *>(value), count);
            XFree(value);
            return result;
        }
        if (value) {
            XFree(value);
        }
    }
    char *value = nullptr;
    if (!XFetchName(display, window, &value) || !value) {
        return {};
    }
    const std::string result(value);
    XFree(value);
    return result;
}

static bool isEden(Display *display, Window window) {
    XClassHint hint{};
    if (XGetClassHint(display, window, &hint)) {
        const bool found = (hint.res_name && std::strcmp(hint.res_name, "eden-browser") == 0) ||
                           (hint.res_class && std::strcmp(hint.res_class, "Eden") == 0);
        if (hint.res_name) {
            XFree(hint.res_name);
        }
        if (hint.res_class) {
            XFree(hint.res_class);
        }
        return found;
    }
    return false;
}

static unsigned long processId(Display *display, Window window) {
    const Atom property = XInternAtom(display, "_NET_WM_PID", True);
    if (property == None) {
        return 0;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char *value = nullptr;
    if (XGetWindowProperty(display, window, property, 0, 1, False, XA_CARDINAL, &actualType, &actualFormat, &count, &remaining, &value) !=
            Success ||
        !value || actualFormat != 32 || count != 1) {
        if (value) {
            XFree(value);
        }
        return 0;
    }
    const unsigned long result = *reinterpret_cast<unsigned long *>(value);
    XFree(value);
    return result;
}

static bool isViewable(Display *display, Window window) {
    XWindowAttributes attributes{};
    return XGetWindowAttributes(display, window, &attributes) && attributes.map_state == IsViewable;
}

static Window findTopLevel(Display *display, unsigned long requestedProcess) {
    const Window displayRoot = DefaultRootWindow(display);
    Window root = 0;
    Window parent = 0;
    Window *children = nullptr;
    unsigned int count = 0;
    if (!XQueryTree(display, displayRoot, &root, &parent, &children, &count)) {
        return 0;
    }
    Window result = 0;
    if (requestedProcess) {
        for (unsigned int index = 0; index < count; ++index) {
            if (processId(display, children[index]) == requestedProcess && isViewable(display, children[index])) {
                result = children[index];
                break;
            }
        }
    }
    if (requestedProcess && !result) {
        if (children) {
            XFree(children);
        }
        return 0;
    }
    for (unsigned int index = 0; index < count; ++index) {
        if (!result && isEden(display, children[index]) && isViewable(display, children[index])) {
            result = children[index];
            break;
        }
    }
    for (unsigned int index = 0; index < count && !result; ++index) {
        const std::string windowTitle = title(display, children[index]);
        if (isViewable(display, children[index]) &&
            (windowTitle.find("bench:") != std::string::npos || windowTitle.find("video:") != std::string::npos)) {
            result = children[index];
        }
    }
    if (children) {
        XFree(children);
    }
    return result;
}

static bool waitTitle(Display *display, Window &window, unsigned long requestedProcess, const char *needle,
                      std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (!window || !isViewable(display, window)) {
            window = findTopLevel(display, requestedProcess);
        }
        if (title(display, window).find(needle) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static Window shell(Display *display, unsigned long requestedProcess) {
    Window window = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (!window && Clock::now() < deadline) {
        window = findTopLevel(display, requestedProcess);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return window;
}

static void key(Display *display, KeySym symbol) {
    const KeyCode code = XKeysymToKeycode(display, symbol);
    XTestFakeKeyEvent(display, code, True, CurrentTime);
    XTestFakeKeyEvent(display, code, False, CurrentTime);
}

static void shortcut(Display *display, KeySym symbol, bool shift = false) {
    const KeyCode control = XKeysymToKeycode(display, XK_Control_L);
    const KeyCode shiftCode = XKeysymToKeycode(display, XK_Shift_L);
    XTestFakeKeyEvent(display, control, True, CurrentTime);
    if (shift) {
        XTestFakeKeyEvent(display, shiftCode, True, CurrentTime);
    }
    key(display, symbol);
    if (shift) {
        XTestFakeKeyEvent(display, shiftCode, False, CurrentTime);
    }
    XTestFakeKeyEvent(display, control, False, CurrentTime);
}

static int frameCount(const std::string &value) {
    const std::string prefix = "video:frames:";
    const std::size_t offset = value.find(prefix);
    return offset == std::string::npos ? -1 : std::atoi(value.c_str() + offset + prefix.size());
}

int main(int argc, char **argv) {
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        return 1;
    }
    XSetErrorHandler(ignoreXError);
    const unsigned long requestedProcess = argc >= 3 ? std::strtoul(argv[2], nullptr, 10) : 0;
    Window window = shell(display, requestedProcess);
    if (!window || argc < 2) {
        XCloseDisplay(display);
        return 2;
    }
    if (std::strcmp(argv[1], "quit") == 0) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        shortcut(display, XK_q);
        XFlush(display);
        XCloseDisplay(display);
        return 0;
    }
    if (std::strcmp(argv[1], "video") == 0) {
        if (!waitTitle(display, window, requestedProcess, "video:frames:", std::chrono::seconds(20))) {
            std::fprintf(stderr, "window=%lu title=%s\n", window, title(display, window).c_str());
            XCloseDisplay(display);
            return 3;
        }
        const int before = frameCount(title(display, window));
        const auto started = Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        const int after = frameCount(title(display, window));
        std::printf("video_fps=%.3f frames=%d\n", static_cast<double>(after - before) / elapsed, after - before);
        XCloseDisplay(display);
        return after > before ? 0 : 4;
    }
    if (!waitTitle(display, window, requestedProcess, "bench:ready", std::chrono::seconds(20))) {
        std::fprintf(stderr, "window=%lu title=%s\n", window, title(display, window).c_str());
        XCloseDisplay(display);
        return 5;
    }
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XSync(display, False);
    if (std::strcmp(argv[1], "tabswitch") == 0) {
        for (int sample = 0; sample < 21; ++sample) {
            shortcut(display, XK_Tab, sample % 2 == 1);
            XFlush(display);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        XCloseDisplay(display);
        return 0;
    }
    if (std::strcmp(argv[1], "input") == 0) {
        shortcut(display, XK_l);
        XFlush(display);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int sample = 0; sample < 21; ++sample) {
            key(display, XK_a + sample % 26);
            XFlush(display);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        XCloseDisplay(display);
        return 0;
    }
    if (std::strcmp(argv[1], "scroll") == 0) {
        XWarpPointer(display, None, window, 0, 0, 0, 0, 700, 500);
        XTestFakeButtonEvent(display, 1, True, CurrentTime);
        XTestFakeButtonEvent(display, 1, False, CurrentTime);
        XSync(display, False);
        for (int sample = 0; sample < 20; ++sample) {
            const std::string before = title(display, window);
            const auto started = Clock::now();
            XTestFakeButtonEvent(display, 5, True, CurrentTime);
            XTestFakeButtonEvent(display, 5, False, CurrentTime);
            XFlush(display);
            const auto deadline = started + std::chrono::seconds(2);
            std::string after = title(display, window);
            while (Clock::now() < deadline && (after == before || after.find("bench:") == std::string::npos)) {
                if (!isViewable(display, window)) {
                    window = findTopLevel(display, requestedProcess);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(250));
                after = title(display, window);
            }
            const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (after == before || after.find("bench:") == std::string::npos) {
                XCloseDisplay(display);
                return 7;
            }
            std::printf("scroll=%.6f\n", milliseconds);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        XCloseDisplay(display);
        return 0;
    }
    if (std::strcmp(argv[1], "frames") == 0) {
        for (int sample = 0; sample < 120; ++sample) {
            shortcut(display, XK_Tab, sample % 2 == 1);
            XFlush(display);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        XCloseDisplay(display);
        return 0;
    }
    XCloseDisplay(display);
    return 6;
}
