# ADR-045 — Top-edge touchpad and mirrored keyboard workspaces

Status: ACCEPTED 2026-11-20

The owner requested a touchpad on either side of the keyboard and a larger top touchpad reaching the screen edge. In the top-edge workspace the shell lends its full height to the keyboard. The operator chooses the left or right control rail when selecting the workspace. The rail keeps Home, Shortcuts, Host, Client, Settings, Extensions when available, and both mouse buttons within reach. Home opens the ordinary Mouse view. All other primary workspaces retain the standard top bar.

Workspace changes use the local keyboard picker. The router withdraws the old touch rectangle while the picker is open, releases held input, changes the layout, and publishes the new rectangle. A return to the keyboard during the session restores its workspace. The key field keeps the selected HOST language layout and its ordinary HID behavior. These controls require no Windows application.

The keyboard and shell test suites check both side layouts, both top-edge arrangements, the navigation targets and restoration of the top bar. Simulator fixtures record the picker and each layout at the 7B panel resolution.

The rail opens as a side drawer. Its icons follow the top bar and its connection labels use the same status descriptors. A tap on the handle or an outward horizontal swipe closes it. A tap or inward swipe opens it. Only the handle recognizes this gesture. The gesture engine releases held input and receives no pad rectangle while the drawer moves. After it settles the open touchpad is 780 × 213 pixels and the collapsed touchpad is 960 × 213 pixels.
