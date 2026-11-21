# ADR-044 · Extension screen switcher

**Status:** Accepted interaction contract for the Pilot Panel. Physical qualification is pending.

The Pilot Panel is one installed extension with multiple screens and a shared Setup. A swipe up starting in the bottom navigation strip opens its screen switcher. Tapping the visible handle opens the same switcher. Selecting an instrument, the extension start screen or Setup keeps the package active. An explicit **Return to KVMpad** action in the switcher leaves the package. The swipe itself never exits. The working screens have no direct system Home button.

The first declared screen is the package start screen. Closing the switcher restores the current page. Moving between screens preserves extension state. Leaving a radio frequency editor never submits a draft; only the explicit commit control may submit. Card removal still restores the built-in interface. Existing single-screen packages keep their installed navigation behavior.

The bottom 48 logical pixels are reserved for the switcher handle. A swipe begins there and finishes at least 48 pixels higher with greater vertical than horizontal travel. A drag starting in the content belongs to the content for its entire duration, including when it crosses the strip. A reversed, diagonal or canceled drag must not open a screen or activate an underlying control. Coordinates refer to the upright view after rotation. The first version uses an immediate screen transition.

Local charts may implement pan and zoom, and lists may scroll. Edge-back, global page swipes and an application switcher are outside this interaction contract. The single GT911 reader and existing touch ownership remain in charge. HID control, USB personality and simulator permissions remain independent of the switcher.

## Acceptance

- From radio, Setup and a nested instrument, swipe up. Verify that only the switcher opens and the existing session stays active. Reopen the same screen and verify its state.
- Select another screen and then the extension start screen. Reach the KVMpad mouse screen only by choosing Return to KVMpad in the switcher during normal navigation.
- Repeat with the visible handle and in portrait. Cover short, diagonal, reversed and canceled movements. A swipe starting on a map, keypad or slider must not open the switcher. Verify the absence of unintended clicks and stuck HID controls.
- Remove the card with the switcher open, rotate through normal profile exit and reopen the extension. Verify recovery to the built-in shell and a clean new session.
