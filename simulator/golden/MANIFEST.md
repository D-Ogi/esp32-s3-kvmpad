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
| `approval_prompt.png` | 1024x600 | 18161 | `4d0c74cd6994790bf604eb7efc41b6c46306312ea591e5fedcb2a188b1b58b0d` | Control approval - Allow or Deny, 30 s |
| `client_details.png` | 1024x600 | 19505 | `6157677f496101f028808b859ea5b3dddb04c5e5d17513e2017790cb7e59c1fd` | CLIENT details card opened from the top bar indicator (FR-003) |
| `client_lost.png` | 1024x600 | 15227 | `989c6648158653cff33a2b8f63e096f519d4da15f046d77a0835c38f93f6078b` | MOUSE view, CLIENT session stale (reconnecting) |
| `client_offline.png` | 1024x600 | 14989 | `2fcb0b93180c03e180aaa40d829a33f41470d36275780b375fc322a18dbda62e` | MOUSE view, no network: CLIENT offline, Wi-Fi link down |
| `error_host_offline.png` | 1024x600 | 17450 | `204cda1400cd9448789279c93806ce301c1a89c1c2d7ae327064c5f219161258` | HOST details card with a persistent HOST_OFFLINE error |
| `error_queue_overflow.png` | 1024x600 | 17674 | `79abab307f6adaf40660205d57807e7c1b116aabe5203957bd5dd06db253398c` | HOST details card with a persistent HID queue overflow error |
| `host_details.png` | 1024x600 | 17669 | `1e2726cce5a43079271cd45359fd3970c74b07543e8415c2ba810da279ebff0f` | HOST details card opened from the top bar indicator (FR-003) |
| `host_lost.png` | 1024x600 | 14197 | `af55d9980c539336e4d17e06915e6f5ff35597a524a474e8668eed3083249cce` | MOUSE view, HOST disconnected (FR-006: nothing is sent) |
| `host_lost_keyboard.png` | 1024x600 | 22942 | `6ce402d6ae4164c2ff150ebbf4f5ab39de72720aa8cfd7bd2af1c9cf02990d15` | KEYBOARD view, HOST disconnected: every cap disabled |
| `host_suspended.png` | 1024x600 | 14547 | `c84efabebca372b84b10a89a5337f9e2c29b9e977d44f001db3836b950f8751a` | MOUSE view, HOST USB suspended |
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
| `mouse.png` | 1024x600 | 14716 | `7966646abbb383ec54566df943db733224bcbd8a08807117fa45309ac946bded` | MOUSE view, HOST active, CLIENT available |
| `mouse_drag_lock.png` | 1024x600 | 15342 | `edfeb4d229afe30bdda2a93cc9cdb96a2e05d5025e16d703512fcb9da3839bbe` | MOUSE view, drag lock held by the gesture engine (FR-107) |
| `mouse_help.png` | 1024x600 | 22420 | `c8bcfbe18e71406094be9bed9cf176d4aa3f190f7b7bdb3f77ae444156938e62` | MOUSE view, the gesture help card opened with the "?" key |
| `owner_remote.png` | 1024x600 | 18826 | `baea1cf1788234dc138c80d785d39d9287e0d4bb46c9591198fd3177a5bed381` | SHORTCUTS view, control owner REMOTE: local commands blocked (FR-006, FR-308) |
| `pair_accept.png` | 1024x600 | 18400 | `62b078d8370cc9943235252cebaa49034f9ff49d71f9e948ac7aa1c3a92bddec` | Pairing - Accept or Reject the named client |
| `pair_code.png` | 1024x600 | 18385 | `51cbba098c00bbb7a02dae8775cb159a2a450b2a49f74a2c9e0945731745bee7` | Pairing window - the code and the two countdowns |
| `pair_toast_paired.png` | 1024x600 | 15767 | `c377a5fb058b7f4ab53f6c70053a8007bdf2a0d13b4b1e1ed9a68ce574feb8fc` | Pairing outcome - paired, reported once |
| `pair_unavailable.png` | 1024x600 | 19017 | `67941d728d909331385123e66208176f3a5f4e934b8c5781f88b791ea3630daa` | CLIENT details - pairing unavailable while remote is off |
| `settings_about.png` | 1024x600 | 44354 | `aaae18b0f75b5c4c287df69907c34972e4cf1c3f70a2b56faa0f7ea110c82561` | Settings, About with the license, the Required Notice and the generated third-party notices (FR-751) |
| `settings_appearance.png` | 1024x600 | 15025 | `0700bda1e4ab0e0af2cb597cf78c8cc4323b2f115fded5ef6661771115da105e` | Settings, Appearance with color theme and display brightness (FR-505) |
| `settings_clients.png` | 1024x600 | 15924 | `d02dc7384690cfcb0e120e4b68e744c0d71fd20b724b293f367dae4388a78b6a` | Settings, paired clients with the FR-733 trusted flag and Forget (FR-726) |
| `settings_clients_full.png` | 1024x600 | 23593 | `30fc607aa250f19c3b3a0fce1152796772ec8b4f76f712d531ba1b9d444316c3` | Settings, a full paired-client register (8 of 8): the list scrolls, no row is clipped |
| `settings_confirm.png` | 1024x600 | 21059 | `0fbf515e4950ae27c57db3039aa8129d8355fef2fad096fc9df8925346d68efd` | Settings, confirmation required before pair.require_accept is switched off (ADR-023) |
| `settings_forget.png` | 1024x600 | 17987 | `4aa96f7736f66b8f07223dbfa006a2cfd75cad80ff8e6d1c7a17145e8527b670` | Settings, Forget a paired client behind a confirmation (FR-726, ADR-023) |
| `settings_host_layout.png` | 1024x600 | 16667 | `f52419108f32ca820ad4fb4a9fde96e90b45bf36d0902393a713e21725a00cca` | Settings, Keyboard section: the HOST layout picker at its first level, opened on the page that holds the stored id (FR-741) |
| `settings_host_layout_home.png` | 1024x600 | 19328 | `0b4e37c1dbc18721d6b13c8168c5d01d19d5aaae708ea50e9e3d13ead48a5861` | Settings, HOST layout picker home: the layout in force with its home row, eight pinned favorites and the recents (ADR-039) |
| `settings_host_layout_page.png` | 1024x600 | 16667 | `f52419108f32ca820ad4fb4a9fde96e90b45bf36d0902393a713e21725a00cca` | Settings, HOST layout picker paged on: a page of layouts with no stored id among them, Prev and Next both live (FR-741) |
| `settings_host_layout_search.png` | 1024x600 | 20152 | `a78b4586ef3b1b25e786d9963fac2dce70c3ad2a60d1071aea57a01e903ce60e` | Settings, HOST layout picker search: the query, the picker's own keyboard and the layouts whose id or name match (ADR-039) |
| `settings_host_layout_star.png` | 1024x600 | 21952 | `0a5eed3276e549a28d4646b3029dba2ffdb4149e4295360ca4b4e03e32afdbbb` | Settings, HOST layout list: the A-Z strip above the rows, one bucket away from the top, and a ninth favorite refused (ADR-039) |
| `settings_host_layout_variants.png` | 1024x600 | 20683 | `4dda2a53db5be3e1bb10efc3c22e371e20b8cbaa0ee81a30b63194fa4ea69857` | Settings, HOST layout picker at its second level: the variants of one layout, Back returns to the layout list (FR-741) |
| `settings_input.png` | 1024x600 | 15681 | `8347382d3fd11d464c5c4906b54c18e5d1f2ea5d44bba344427b86d9d81bb4ec` | Settings, Input with keep-awake interval (FR-711) |
| `settings_keyboard.png` | 1024x600 | 24930 | `64263a8c2c67fb94363142d9e752fe24b922324853fb6d15ebf4d6915722a207` | Settings, Keyboard section: the input language list with English always on, the HOST layout row with the stored id, Unicode delivery stepper with its setup hint (ADR-037, FR-741, FR-747) |
| `settings_remote.png` | 1024x600 | 21431 | `b1541de6cdf7c103848107ab225971631f708ba196d469e44ab1c5841169e2e4` | Settings, Remote section with the ADR-023 safe defaults (FR-731, FR-732, FR-733) |
| `settings_shortcuts.png` | 1024x600 | 13006 | `b2fb831c4c4d1ddfc462e4b4709d5433182a48316e2b871f7f7e010eb01b9240` | Settings, Shortcuts section with the FR-734 Alt+F4 confirmation policy (ADR-023) |
| `settings_unknown.png` | 1024x600 | 21258 | `7f9d4fa6e47633fb21bbe691fe6bcad121ade505e648710d3e08f6bc5c7f13d0` | Settings before the domain published a record: every value Unknown, every control off (07 12.2) |
| `shortcuts.png` | 1024x600 | 20599 | `993bfb32c753f9db416e7241d7a6f306cee3f62c7b67bea2d6f2d4a8e6368a2e` | SHORTCUTS view, commands available |
| `shortcuts_confirm.png` | 1024x600 | 22840 | `2b3e85c593a0c462e81068552ea5a0edcd5f5d7e732e481eaf43763bb0fdc1f2` | SHORTCUTS view, Alt+F4 confirmation modal (FR-734, ADR-023) |
| `shortcuts_host_lost.png` | 1024x600 | 19127 | `44eac026ab2ab0d61527e97ee9b5d8dab311b7d62e2bd863795be980499c04d1` | SHORTCUTS view, HOST detached: local commands blocked (FR-308) |
| `shortcuts_layout_ctrl.png` | 1024x600 | 20570 | `fe45878fa967310821694819dc5d0e2ef60675999315adfee4e6cd83b5cb0e43` | SHORTCUTS view, HOST layout chord for macOS / Linux (FR-744) |
| `shortcuts_toast.png` | 1024x600 | 20662 | `774b51ec53674d058199b6e2e8d6ec74b5191ad1ee44fac8db3602f8173b63bf` | SHORTCUTS view, Show desktop requested (the toast claims nothing about the HOST) |
