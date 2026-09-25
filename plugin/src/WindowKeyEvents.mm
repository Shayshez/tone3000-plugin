#include "EditorWebViewSetup.h"

#if JUCE_IOS

namespace EditorWebViewSetup {

// iOS has no host DAW to hand a transport keypress back to: the Standalone app
// IS the host, and there is no AppKit event queue or NSResponder chain to post
// a synthesized Space/Enter into. The UI still calls this (it suppresses the
// key itself so nothing beeps or scrolls), so keep the symbol and make it a
// no-op rather than teaching the UI a second platform check.
void forwardKeyToHost(void*, HostKey, bool) {}

bool isPrimaryMouseButtonDown() {
  return false;  // no resize grip on iOS (the window is the screen)
}

}  // namespace EditorWebViewSetup

#else

#import <AppKit/AppKit.h>

namespace EditorWebViewSetup {

// The WKWebView is first responder once the user has clicked the plugin UI,
// so every keypress lands in web content and the host DAW never sees it,
// most painfully the transport keys Space (play/stop) and Enter (return to
// start). The UI swallows both itself (preventDefault, so no caret scroll or
// system beep) and calls this to hand the press to the host instead.
void forwardKeyToHost(void* nsViewPtr, HostKey key, bool keepFocus) {
  NSView* view = (__bridge NSView*)nsViewPtr;
  NSWindow* window = [view window];
  if (window == nil)
    return;
  // The WebView's own responder, to hand focus back to (keepFocus).
  NSResponder* pluginResponder = [window firstResponder];

  // Focus the host's own content view (our peer view is a subview of it):
  // the same state as a click on the host's plugin-window chrome, where
  // the transport keys work natively. Follow-up presses and key repeats
  // then reach the host without us. Don't target [NSApp mainWindow]
  // instead: hosts can make the plugin window main (LUNA does), which
  // would send the key right back into the chain we just emptied.
  NSView* hostView = [window contentView];
  if (hostView == nil || ![window makeFirstResponder:hostView])
    [window makeFirstResponder:nil];

  NSString* characters = key == HostKey::enter ? @"\r" : @" ";
  const unsigned short keyCode = key == HostKey::enter ? 36 : 49;  // kVK_Return : kVK_Space
  NSEvent* (^keyEvent)(NSEventType) = ^(NSEventType type) {
    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:[[NSProcessInfo processInfo] systemUptime]
                        windowNumber:[window windowNumber]
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:characters
                           isARepeat:NO
                             keyCode:keyCode];
  };
  // postEvent, not sendEvent: only queued events flow through the app's
  // run-loop dispatch, where local event monitors live, and hosts commonly
  // hang their transport shortcut off one.
  [NSApp postEvent:keyEvent(NSEventTypeKeyDown) atStart:NO];
  [NSApp postEvent:keyEvent(NSEventTypeKeyUp) atStart:NO];

  // Hand focus back once the host has dispatched the posted pair (they are
  // queued behind whatever is pending, hence the short delay), unless the
  // user clicked elsewhere in the meantime.
  if (keepFocus && pluginResponder != nil && pluginResponder != [window contentView]) {
    NSView* hostView = [window contentView];
    const NSInteger windowNumber = [window windowNumber];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      NSWindow* w = [NSApp windowWithWindowNumber:windowNumber];
      if (w == nil)
        return;
      NSResponder* current = [w firstResponder];
      if ((current == hostView || current == w || current == nil) &&
          [pluginResponder isKindOfClass:[NSView class]] &&
          [(NSView*)pluginResponder window] == w)
        [w makeFirstResponder:pluginResponder];
    });
  }
}

bool isPrimaryMouseButtonDown() {
  return ([NSEvent pressedMouseButtons] & 1) != 0;
}

}  // namespace EditorWebViewSetup

#endif  // JUCE_IOS
