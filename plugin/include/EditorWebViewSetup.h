#pragma once
#include <juce_gui_extra/juce_gui_extra.h>

class TONE3000Editor;

namespace EditorWebViewSetup {

juce::WebBrowserComponent::Options buildMainWebViewOptions(TONE3000Editor* editor);

/**
 * Delete the webview's tone3000.com session state (cookies, site storage).
 *
 * Logout in the UI clears the tokens it holds, but the OAuth flows ride on
 * the site session inside the webview; with the cookie still present the
 * next authorize redirect silently re-issues a code without ever showing a
 * login screen. Platform-specific implementations (WebViewCookies.mm / .cpp).
 */
void clearAuthCookies();

#if JUCE_MAC
/**
 * Force acceptsMouseMovedEvents on the NSWindow that hosts the editor.
 *
 * WKWebView's hover states and cursor changes ride on mouseMoved: NSEvents,
 * which AppKit only delivers when the window opts in. JUCE's own windows do
 * (Standalone works out of the box), but most DAW plugin windows don't,
 * killing :hover and cursor feedback in the web UI while clicks keep working.
 * Takes the editor's NSView* (peer native handle); implemented in
 * WindowMouseEvents.mm.
 */
void enableHostWindowMouseMovedEvents(void* nsViewPtr);

/**
 * Keep webview hover alive while the plugin window is not key.
 *
 * AppKit delivers responder-chain mouseMoved: events only to the key window,
 * so hover feedback dies the moment the user clicks a DAW control and only
 * comes back after a click inside the plugin. Installs an always-active
 * NSTrackingArea on the WKWebView that makes the plugin window key when the
 * cursor enters and forwards mouseMoved: to the webview until that sticks.
 * Idempotent per webview; implemented in WindowMouseEvents.mm.
 */
void installHoverMouseForwarding(void* nsViewPtr);

/**
 * Kill the grey pre-load flash: stop the WKWebView drawing its own (system
 * grey) background before the page's first paint, and paint the hosting
 * NSWindow black behind it. Takes the editor's NSView* (peer native handle);
 * implemented in WindowMouseEvents.mm. Idempotent; safe to call on every
 * reparent, before or after the WKWebView exists in the hierarchy.
 */
void applyBlackWebViewBackground(void* nsViewPtr);

/**
 * Enable/disable the WKWebView Web Inspector (right-click -> Inspect
 * Element) at runtime: sets `inspectable` (macOS 13.3+) and WebKit's
 * developerExtrasEnabled preference on the live webview, so release builds
 * can expose the inspector behind the Settings -> Diagnostics toggle without
 * a JUCE patch. Also gates the stock WKWebView context menu (Reload, etc.):
 * those items only appear while the inspector is on. Takes the editor's
 * NSView* (peer native handle); implemented in WindowMouseEvents.mm. WebKit
 * reads the flags live, so this takes effect on the next right-click with
 * no reload. Always call this on editor attach (even with enabled=false)
 * so the menu guard is installed.
 */
void setWebInspectorEnabled(void* nsViewPtr, bool enabled);
#endif

/** Transport key the UI hands back to the host (see forwardKeyToHost). */
enum class HostKey { space, enter };

/**
 * Re-dispatch a transport keypress to the host DAW.
 *
 * Backs the `forwardKeyToHost` native function: the UI swallows Space and
 * Enter presses it has no use for and hands them here so the DAW's play/stop
 * and return-to-start shortcuts keep working while the plugin has keyboard
 * focus. Hands keyboard focus back to the host, then delivers synthesized
 * key events to it. Takes the editor's peer native handle. Best effort per
 * host; implemented in WindowKeyEvents.mm (macOS) and WindowKeyEvents.cpp
 * (Windows/Linux).
 */
void forwardKeyToHost(void* nativeHandle, HostKey key);

/**
 * Whether the primary mouse button is physically down right now, asked of
 * the OS directly. The web UI's resize grip needs this: the press lands in
 * the native WebView, so JUCE never sees the mouse-down and its own
 * ModifierKeys::getCurrentModifiersRealtime() reports the button as up.
 * Same files as forwardKeyToHost.
 */
bool isPrimaryMouseButtonDown();

/**
 * Main-UI WebView with a navigation allowlist.
 *
 * Native integration (loadTone, presets, clipboard, auth token, ...) is
 * injected into every page this view loads, so navigation is restricted to
 * origins we trust: the embedded resource provider, the Vite dev server, and
 * tone3000.com (the OAuth Select flow navigates the view there by design).
 * Anything else (a stray link, a dropped file, a window.open) is blocked
 * in-view and handed to the system browser instead.
 */
class GuardedWebView : public juce::WebBrowserComponent {
public:
  using juce::WebBrowserComponent::WebBrowserComponent;

  /**
   * URL of the plugin UI itself (embedded resources, or the Vite server in
   * dev). Failed navigations recover here: the OAuth flows redirect this
   * view to tone3000.com, and if that navigation dies (offline / site down)
   * the user would otherwise be stranded on a dead page with no way back.
   */
  void setRecoveryUrl(const juce::String& url) { recoveryUrl = url; }

  bool pageAboutToLoad(const juce::String& newUrl) override;
  void newWindowAttemptingToLoad(const juce::String& newUrl) override;
  bool pageLoadHadNetworkError(const juce::String& errorInfo) override;
  void pageFinishedLoading(const juce::String& url) override;

private:
  static bool isAllowedUrl(const juce::String& url);

  juce::String recoveryUrl;
  // True while a recovery load is in flight; stops the failure handler from
  // looping if the recovery URL itself fails (dev server down).
  bool recoveryInFlight = false;
};

}  // namespace EditorWebViewSetup
