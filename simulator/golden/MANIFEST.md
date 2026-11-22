# Golden images of the desktop simulator

Generated file - do not edit by hand. Rendered by `kvm_sim` (TASK-070D) from the fixtures in
`simulator/fixtures/`, one image per fixture, and regenerated together with
`python -I -X utf8 simulator/tools/golden_manifest.py`.

* Renderer: LVGL 9.6.0 (the version pinned by `dependencies.lock`), software renderer, headless
  display, snapshot taken in RGB565 and expanded to 24-bit RGB - the pixel format of the ADR-026 panel.
* Geometry: 1024x600 (ADR-026 / TECH-087).
* Command: `kvm_sim --update-golden`
* Comparison defaults: per-channel tolerance 2, at most 64 differing pixels per image.

| Golden | Size | Bytes | SHA256 | Fixture |
|---|---|---|---|---|
| `approval_prompt.png` | 1024x600 | 19363 | `ee0c726419a3df73ce853c23d51de3688ae1d4f2593ab425f65d464a27208806` | Control approval - Allow or Deny, 30 s |
| `client_details.png` | 1024x600 | 20683 | `499dd4fc840b7f16a0d8f7596b8506f492ae0953a9ac6474f6397750cae0142d` | CLIENT details card opened from the top bar indicator (FR-003) |
| `client_lost.png` | 1024x600 | 36467 | `fcb60cacb2c08a2940d81b29b4276d187a3e8f768dd948ca8184bea292737041` | MOUSE view, CLIENT session stale (reconnecting) |
| `client_offline.png` | 1024x600 | 36060 | `a3825e142875c60fb925181a45350d719a7d73970fe12f4c5ee463d60a6786e7` | MOUSE view, no network: CLIENT offline, Wi-Fi link down |
| `diagnostics_overlay.png` | 1024x600 | 17254 | `d8814346011d47480b5313b558b7f63e17a22d1f6a099cd00139092992711929` | Mouse view with USB and touch diagnostics (FR-509) |
| `error_host_offline.png` | 1024x600 | 18449 | `0e7c205d3786c8142937725e188e0e3eae9c78fb9c65d0d05036e9ab23210224` | HOST details card with a persistent HOST_OFFLINE error |
| `error_queue_overflow.png` | 1024x600 | 18834 | `0b2cb989c3085a98507d273859e2e6b3a21be23ead95996b60bbfc72f3592e96` | HOST details card with a persistent HID queue overflow error |
| `host_details.png` | 1024x600 | 18822 | `afc66171c1b6ce83d9ea8f77181aaa5edbeac25b4a1aec0c5a75466a3ab23f07` | HOST details card opened from the top bar indicator (FR-003) |
| `host_lost.png` | 1024x600 | 15371 | `a0fdf372b4c9967de13140253c18262dd09d79cfb7186691d6168533d2deba20` | MOUSE view, HOST disconnected (FR-006: nothing is sent) |
| `host_lost_keyboard.png` | 1024x600 | 22942 | `6ce402d6ae4164c2ff150ebbf4f5ab39de72720aa8cfd7bd2af1c9cf02990d15` | KEYBOARD view, HOST disconnected: every cap disabled |
| `host_suspended.png` | 1024x600 | 15722 | `3dc38eb318f2694f5bd47dada5984e235179fffe94462f7c452a586f0ed009a5` | MOUSE view, HOST USB suspended |
| `keyboard.png` | 1024x600 | 21394 | `51bc2f0c52998afeed1c3042b544ea9db486b4b563897e5ca7799cb56c4b2b24` | KEYBOARD view, CapsLock LED off |
| `keyboard_caps_on.png` | 1024x600 | 21716 | `2abe455df997406fb7f8ffe0480a0893d7d4af63bf37e91d1f5ec6e264ad458f` | KEYBOARD view, CapsLock LED on |
| `keyboard_caps_unknown.png` | 1024x600 | 21543 | `9dda86d35d648779d6eb6dbcee837383ab75c246e1d3fbe24b81619f50d30603` | KEYBOARD view, the HOST has sent no LED report (FR-206) |
| `keyboard_fn.png` | 1024x600 | 20766 | `9b9d34b7cf9b481b64616c9959da2e736e89c923d0a680cd76288abd505833f8` | KEYBOARD view, local F1-F12 layer on (FR-207) |
| `keyboard_ja.png` | 1024x600 | 25497 | `f1f2f910da70580ce62bfbbff8ed4bd9ffbe5e408d34404ecf18ff94843638b5` | KEYBOARD view, JA layer composing "kanjihenkan": kana reading and the candidate strip (ADR-024 FR-746) |
| `keyboard_ja_flick.png` | 1024x600 | 22438 | `31beb5896ddbf0218c7285ecf360ca236cfb9f7a12298e9db67bddbd5d5d1c32` | KEYBOARD view, JA 12-key flick pad: "kanji" flicked in, reading and candidates (ADR-024 FR-746) |
| `keyboard_key_held.png` | 1024x600 | 21857 | `ea57302b41d3b5ac6dd6412ebfe0761fae6fad14badc8bd6712291207c6dd320` | KEYBOARD view, the A cap held down (FR-203 native DOWN) |
| `keyboard_ko.png` | 1024x600 | 23107 | `f170d012c510122f575aeb9a5365d65870faee78a54136cc48ae8b4ae8054ab3` | KEYBOARD view, KO layer composing 안녕: 안 committed, 녕 in the preedit strip (ADR-024 FR-746) |
| `keyboard_lang_picker.png` | 1024x600 | 24456 | `255b6a5b0b888c37e4b5ad02b892fe01640f8fbadd0b34bfd84e605bcd6ec85f` | KEYBOARD view, language picker held open on the KO layer: Korean current, the two decoder-less rows disabled with their reason (ADR-037) |
| `keyboard_layout_br.png` | 1024x600 | 23712 | `3d3530030fe0024f834fbf905f50de65ddc4d2c36999466afda80793283cf109` | KEYBOARD view, HOST layout br: ABNT2 geometry with the LSGT and Ro keys, cedilla legend (FR-742) |
| `keyboard_layout_de.png` | 1024x600 | 23593 | `9c62cb27d46aa2f3a510634ccd689517dc542b59048514516ddf22a50f93e96b` | KEYBOARD view, HOST layout de: ISO geometry with the LSGT key and the ISO Enter, umlaut legends (FR-742) |
| `keyboard_layout_fr.png` | 1024x600 | 23496 | `547ee973123afe8efdce5018b3fc48dd229dd11a0c8d67e3e939b1592f47ae6b` | KEYBOARD view, HOST layout fr: AZERTY on ISO geometry, AltGr legends in the bottom-right corner of a cap (FR-742) |
| `keyboard_layout_jp.png` | 1024x600 | 21927 | `e28afc0024637265ff83c8f7c1a7e2ae5270d862bd78e7e0957c9a2a8b4478ff` | KEYBOARD view, HOST layout jp: JIS geometry with Yen, Ro, Muhenkan, Henkan and Kana on their own usages (FR-742, FR-743) |
| `keyboard_layout_ru.png` | 1024x600 | 21163 | `a9e1af106d9999305e3901eeb1ae76ade3dde0e7c9bed1f5cc6b6deb0cee8cea` | KEYBOARD view, HOST layout ru: Cyrillic legends from the generated legend font, usages unchanged (FR-742, FR-743, TECH-078) |
| `keyboard_layout_us.png` | 1024x600 | 21394 | `51bc2f0c52998afeed1c3042b544ea9db486b4b563897e5ca7799cb56c4b2b24` | KEYBOARD view, HOST layout us: ANSI geometry, no LSGT key, ASCII legends on their HID usages (FR-742, FR-743) |
| `keyboard_mods.png` | 1024x600 | 22146 | `180b8e0c715cbc8fefcef52d8526621f234ac7c5df3381e40255e1e82962764b` | KEYBOARD view, LShift armed and LCtrl locked (FR-204, ADR-016) |
| `keyboard_pad_above.png` | 1024x600 | 23569 | `803291605c8acbbd93b0037ac7559d24ab97c2eeb9580459f7e2c7c4ecb495d9` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the right |
| `keyboard_pad_above_collapsed.png` | 1024x600 | 18671 | `4bb36404e2e75f84bc1e9546b75929087f1aeef1193104203695c8f30698378e` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the right with the navigation drawer collapsed |
| `keyboard_pad_above_high_contrast.png` | 1024x600 | 22672 | `9827e142843fe71b7b7834721b53a71147d5ed1eeabe423247cbc467646168c2` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the right (High contrast) |
| `keyboard_pad_above_left.png` | 1024x600 | 23561 | `d83da4524fb3ffb9c58b59cf6253ee88910fb6789894fd91d5070b2225f3076d` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the left |
| `keyboard_pad_above_left_collapsed.png` | 1024x600 | 18669 | `1565c4d90f12713ac66582854261efaecdee1c245e777eea71417bc8abfeb3cd` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the left with the navigation drawer collapsed |
| `keyboard_pad_above_left_high_contrast.png` | 1024x600 | 22673 | `25042a6aff5b95a112107bbeef1b289ffabc807d6d55eee8d9b42aafc4f43791` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the left (High contrast) |
| `keyboard_pad_above_left_night.png` | 1024x600 | 24809 | `b2fc293cb0056ca7b1ce27191dbe58f45e8b9f0a649efceedb724240550acd2a` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the left (Night) |
| `keyboard_pad_above_night.png` | 1024x600 | 24810 | `53bbce4e2356a10df4c6e15eb987479a3650ef56e9fbba8a92bb3179a9bc783a` | Top-edge touchpad above the US keyboard with navigation and mouse buttons on the right (Night) |
| `keyboard_pad_left.png` | 1024x600 | 26413 | `736f7b7a76025c4501be771f6740919156f2f0f2beb362315163fe8ec577bf5c` | Keyboard with the touchpad left of the JIS keys, both mouse buttons and the Switch window shortcut |
| `keyboard_pad_right.png` | 1024x600 | 26446 | `665477915e0203f80eaf28d02a094d33923e9b8c47b30eb833d97b8f5aa27f52` | JIS keyboard with the touchpad and window switcher on the right |
| `keyboard_quick_layout.png` | 1024x600 | 23774 | `905a65abb28b18b8f0c829a87e442cd1de64bee0b441d11a4025fe2f148e69a7` | KEYBOARD view, HOST layout quick switch held open: the pinned layouts first, then the recents that are not pinned, the layout in force marked (ADR-039 section 8) |
| `keyboard_workspaces.png` | 1024x600 | 23918 | `2627aa0e41abd89a79c6072e04f82a00676fad6aad5c303a42e76fffa5fbddaa` | Keyboard workspace picker with side touchpads and grouped top-edge button positions |
| `keyboard_zh.png` | 1024x600 | 24941 | `4dc69851629811d479093cda472ce45a45f388eb80b315cca2bd277d725ed630` | KEYBOARD view, ZH layer composing "nihao": pinyin input and the candidate strip (ADR-024 FR-746) |
| `maintenance.png` | 1024x600 | 18674 | `436e1dba55232dab07b633c98645f5a64c9a325ad28c2e6196b92a347b9c7523` | SHORTCUTS view during maintenance (OTA): local input blocked |
| `mouse.png` | 1024x600 | 16178 | `dafba78dfb4cc579d0541b59ea72a52bf4e473e00f9639c94be96f2197291a28` | MOUSE view, HOST active, CLIENT available |
| `mouse_drag_lock.png` | 1024x600 | 16801 | `dc21edde69571e9108b5a78d0082e9770c97b987acc7870656c5093000bda074` | MOUSE view, drag lock held by the gesture engine (FR-107) |
| `mouse_help.png` | 1024x600 | 23003 | `e5f36bf7bfb6223fe18659342393e9da12963a7f9ce434402d41beee4b41ed70` | MOUSE view, the gesture help card opened with the "?" key |
| `owner_remote.png` | 1024x600 | 18826 | `baea1cf1788234dc138c80d785d39d9287e0d4bb46c9591198fd3177a5bed381` | SHORTCUTS view, control owner REMOTE: local commands blocked (FR-006, FR-308) |
| `pair_accept.png` | 1024x600 | 19622 | `687b172b877f7e3d85793de49a67448fa61fae2c28dc88284da44c7c06291153` | Pairing - Accept or Reject the named client |
| `pair_code.png` | 1024x600 | 19483 | `769740effdf707332087b0e4c79f0fd5f100ff3d806b1830befbedecddafb9fb` | Pairing window - the code and the two countdowns |
| `pair_toast_paired.png` | 1024x600 | 17217 | `3199dc8ee9c90835083ca02d4d0717a5d78756a8382a44024aabe5ecdab5dbde` | Pairing outcome - paired, reported once |
| `pair_unavailable.png` | 1024x600 | 20173 | `60a2695f736eabd25e964d56ac0d5c67e06e4f248152ff06184821f26a82df5d` | CLIENT details - pairing unavailable while remote is off |
| `settings_about.png` | 1024x600 | 38205 | `74127a0734cc6b9af89ffa94d6f4b18ab8b93bbbcc5a56ef453ac04f82b774b5` | Settings, About with the license, the Required Notice and the generated third-party notices (FR-751) |
| `settings_appearance.png` | 1024x600 | 15066 | `0a9f0b46daa01b1b970ebca2a78864d3cee7630cfa87122fff718b08075e60a9` | Settings, Appearance with color theme and display brightness (FR-505) |
| `settings_clients.png` | 1024x600 | 15961 | `76161832955b6e53f810d05f3c24218c4e2f0a3e0423965fc12fdeca1ccdc7c0` | Settings, paired clients with the FR-733 trusted flag and Forget (FR-726) |
| `settings_clients_full.png` | 1024x600 | 23616 | `3f88016c9de34afbff2262e9d15138ba2da9e2b57160aaa1dc516a09dc7f75c0` | Settings, a full paired-client register (8 of 8): the list scrolls, no row is clipped |
| `settings_confirm.png` | 1024x600 | 21081 | `bd7cea857e90532e1d64280559bd8303e4d2c838c18f4dc300b9aca2ccd7b52c` | Settings, confirmation required before pair.require_accept is switched off (ADR-023) |
| `settings_forget.png` | 1024x600 | 17996 | `c93f84b2951b1775d92014c498f54961c55ca1a2320f14752ef278a97692091e` | Settings, Forget a paired client behind a confirmation (FR-726, ADR-023) |
| `settings_host_layout.png` | 1024x600 | 16679 | `bcb39dac85e377b673741c6846e4e9307db907f2bb5ddf0e113cbc9ccba7bcc0` | Settings, Keyboard section: the HOST layout picker at its first level, opened on the page that holds the stored id (FR-741) |
| `settings_host_layout_home.png` | 1024x600 | 19331 | `3e458bf06d3f1a2f46bd5f37d5201b1825a34ed8ceae21263cbf3597145450a4` | Settings, HOST layout picker home: the layout in force with its home row, eight pinned favorites and the recents (ADR-039) |
| `settings_host_layout_page.png` | 1024x600 | 16679 | `bcb39dac85e377b673741c6846e4e9307db907f2bb5ddf0e113cbc9ccba7bcc0` | Settings, HOST layout picker paged on: a page of layouts with no stored id among them, Prev and Next both live (FR-741) |
| `settings_host_layout_search.png` | 1024x600 | 20178 | `4848f8f0088d760df59d7ebfb01b1cfe0bc6f690bf0113e2988ef1503ad7d063` | Settings, HOST layout picker search: the query, the picker's own keyboard and the layouts whose id or name match (ADR-039) |
| `settings_host_layout_star.png` | 1024x600 | 21985 | `cf310e91c3bcfc781d4faacb51c96d811a18794b824b0aaf7ffedc6a0b8c7d28` | Settings, HOST layout list: the A-Z strip above the rows, one bucket away from the top, and a ninth favorite refused (ADR-039) |
| `settings_host_layout_variants.png` | 1024x600 | 20715 | `f76cfc3471d9212f1a45f3d85cce0dd12847c419fa2c3971af90cab4395bac2d` | Settings, HOST layout picker at its second level: the variants of one layout, Back returns to the layout list (FR-741) |
| `settings_input.png` | 1024x600 | 27165 | `75fcc81c6e3cfb42e05b56f876136188a4055b4734767801c4ecc1a93d807d35` | Settings, Input with pointer speed, mouse gestures and keep-awake interval (FR-501..504/711) |
| `settings_keyboard.png` | 1024x600 | 24970 | `f5b3897a5d567b926c9a2c83fb07556fd33bd4171656e8bf0ed96f12c7cadc15` | Settings, Keyboard section: the input language list with English always on, the HOST layout row with the stored id, Unicode delivery stepper with its setup hint (ADR-037, FR-741, FR-747) |
| `settings_remote.png` | 1024x600 | 21483 | `1157d908ec569acf3702d9b91ab92272d3daadc2fbbe6c992123dc8ccbc73786` | Settings, Remote section with the ADR-023 safe defaults (FR-731, FR-732, FR-733) |
| `settings_shortcuts.png` | 1024x600 | 13054 | `ab3531809a94c63958298dbf5b75074999b3ad46830b23f4b9e3ee2e9f8cebfe` | Settings, Shortcuts section with the FR-734 Alt+F4 confirmation policy (ADR-023) |
| `settings_unknown.png` | 1024x600 | 21337 | `b4a74a96e22b25597af9209bc7748beaf0814e83bb93b7033069673675c00a72` | Settings before the domain published a record: every value Unknown, every control off (07 12.2) |
| `shortcuts.png` | 1024x600 | 20599 | `993bfb32c753f9db416e7241d7a6f306cee3f62c7b67bea2d6f2d4a8e6368a2e` | SHORTCUTS view, commands available |
| `shortcuts_confirm.png` | 1024x600 | 22840 | `2b3e85c593a0c462e81068552ea5a0edcd5f5d7e732e481eaf43763bb0fdc1f2` | SHORTCUTS view, Alt+F4 confirmation modal (FR-734, ADR-023) |
| `shortcuts_host_lost.png` | 1024x600 | 19127 | `44eac026ab2ab0d61527e97ee9b5d8dab311b7d62e2bd863795be980499c04d1` | SHORTCUTS view, HOST detached: local commands blocked (FR-308) |
| `shortcuts_layout_ctrl.png` | 1024x600 | 20570 | `fe45878fa967310821694819dc5d0e2ef60675999315adfee4e6cd83b5cb0e43` | SHORTCUTS view, HOST layout chord for macOS / Linux (FR-744) |
| `shortcuts_toast.png` | 1024x600 | 20662 | `774b51ec53674d058199b6e2e8d6ec74b5191ad1ee44fac8db3602f8173b63bf` | SHORTCUTS view, Show desktop requested (the toast claims nothing about the HOST) |
