// macOS window helpers (Objective-C++). Kept tiny: the rest of the host is plain C++.
#import <AppKit/AppKit.h>

// True when any part of the window is visible on screen. False when it is fully covered,
// minimized, on another Space, or the display is asleep / the screen is locked. Call on the
// main thread.
bool thps_window_visible(void* nswindow) {
    if (nswindow == nullptr) return true;
    NSWindow* w = (__bridge NSWindow*)nswindow;
    return ([w occlusionState] & NSWindowOcclusionStateVisible) != 0;
}
