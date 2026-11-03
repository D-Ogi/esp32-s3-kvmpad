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
| `approval_prompt.png` | 1024x600 | 15833 | `58da6368559e48a75bd0946568fdbb22460246c6345da04023ff4f555c6a56ed` | Control approval - Allow or Deny, 30 s |
| `client_details.png` | 1024x600 | 17170 | `131d247d1c5647708d5a8caaf2c9a07f220f8f2a30ec4086d3ff3b0fea71bd9c` | CLIENT details card opened from the top bar indicator (FR-003) |
| `client_lost.png` | 1024x600 | 11755 | `10210802e7352a2b138f860bc190a6bd239bace6e8f93e67570562d340c00d2c` | MOUSE view, CLIENT session stale (reconnecting) |
| `client_offline.png` | 1024x600 | 11511 | `fbfdf349f6f8509c780b7c63c5bb888afe0a809ab91fd9a1855812474d16588d` | MOUSE view, no network: CLIENT offline, Wi-Fi link down |
| `error_host_offline.png` | 1024x600 | 15114 | `7c9d9f2616fa9545a7c106b749cee7fcbdb44edc8fb1386474860a441832bb96` | HOST details card with a persistent HOST_OFFLINE error |
| `error_queue_overflow.png` | 1024x600 | 15329 | `59e07b3567f2a14f6719f7ce597b531bddc17f24b00af96c0ced05fd5fff71f4` | HOST details card with a persistent HID queue overflow error |
| `host_details.png` | 1024x600 | 15323 | `693dfce3b9f7162452e7b41b88968c24fe8ed4a0e89d314da27d33bfc180e6c3` | HOST details card opened from the top bar indicator (FR-003) |
| `host_lost.png` | 1024x600 | 11506 | `4b5a66020c1fc15a1f7ea4e827168c4314565bc8330a597c2a3c90c8dce4ed42` | MOUSE view, HOST disconnected (FR-006: nothing is sent) |
| `host_lost_keyboard.png` | 1024x600 | 20577 | `0b23987c13f3bd683d82174c89e7bab82d11a185e417289e79fa0c4dba89a717` | KEYBOARD view, HOST disconnected: every cap disabled |
| `host_suspended.png` | 1024x600 | 11918 | `3acecd0cd88d163448b38df6985b3639579db4e200823e20e10905ace5cfab98` | MOUSE view, HOST USB suspended |
| `keyboard.png` | 1024x600 | 19542 | `858b27c2ed6dc4aa35855dd7390a0326f4a3d0ff3a0bcf3f8e7c53ef1d4c4090` | KEYBOARD view, CapsLock LED off |
| `keyboard_caps_on.png` | 1024x600 | 19856 | `511194c85f863900120d5b726febb15fff50b6bd5bcfa544489c45ed48297fe4` | KEYBOARD view, CapsLock LED on |
| `keyboard_caps_unknown.png` | 1024x600 | 19690 | `84d21201517dd833706128b0d1622bc0c974552620ae26932fc328a9ab71de1a` | KEYBOARD view, the HOST has sent no LED report (FR-206) |
| `keyboard_fn.png` | 1024x600 | 18995 | `13026fc794d1eda0580d65a0cf7d673bd1f589da4151a5d362a2396c2e0fc11a` | KEYBOARD view, local F1-F12 layer on (FR-207) |
| `keyboard_ja.png` | 1024x600 | 23717 | `7cd8c988eb5f81b0bd53c48f719adddbc240548303c16c0adf722a1b36b623a0` | KEYBOARD view, JA layer composing "kanjihenkan": kana reading and the candidate strip (ADR-024 FR-746) |
| `keyboard_ja_flick.png` | 1024x600 | 20551 | `db30d0a3c7ac27d44507dfe4e094d736388df98866fb71f6c28f57f2295d1b15` | KEYBOARD view, JA 12-key flick pad: "kanji" flicked in, reading and candidates (ADR-024 FR-746) |
| `keyboard_key_held.png` | 1024x600 | 19996 | `13f5c09809dea6f72fcda04e1b240f9c64fe0142fbf7a9cefd423577fca4a664` | KEYBOARD view, the A cap held down (FR-203 native DOWN) |
| `keyboard_ko.png` | 1024x600 | 21147 | `fb16474659cfe28b487d072036ccca8910406a75024d01d41d9237083f73198b` | KEYBOARD view, KO layer composing 안녕: 안 committed, 녕 in the preedit strip (ADR-024 FR-746) |
| `keyboard_lang_picker.png` | 1024x600 | 23047 | `f4a51a8788fbcb49ed954c928eab0c0e4eefaa2a8564b360b501f5690e24414e` | KEYBOARD view, language picker held open on the KO layer: Korean current, the two decoder-less rows disabled with their reason (ADR-037) |
| `keyboard_layout_br.png` | 1024x600 | 21735 | `0cfe578005fd7bbf3f4daabb30f9e5da2f970c8939c90521922d07a853c571cf` | KEYBOARD view, HOST layout br: ABNT2 geometry with the LSGT and Ro keys, cedilla legend (FR-742) |
| `keyboard_layout_de.png` | 1024x600 | 21624 | `6005335800ba159e4995f3f37d721d30ed39727421af9a200561d8422ecc8707` | KEYBOARD view, HOST layout de: ISO geometry with the LSGT key and the ISO Enter, umlaut legends (FR-742) |
| `keyboard_layout_fr.png` | 1024x600 | 21560 | `c7ac76f5371b942a8f91d53e31b95810d71c50997965919c7757c13d61c8ca8c` | KEYBOARD view, HOST layout fr: AZERTY on ISO geometry, AltGr legends in the bottom-right corner of a cap (FR-742) |
| `keyboard_layout_jp.png` | 1024x600 | 19920 | `f9e9e11d26607163bf837642411a63afab2ab4c5806db66384edf7119e129dcf` | KEYBOARD view, HOST layout jp: JIS geometry with Yen, Ro, Muhenkan, Henkan and Kana on their own usages (FR-742, FR-743) |
| `keyboard_layout_ru.png` | 1024x600 | 19196 | `529057367c5e4304de708cfbb0a1bb2d4771dd1f79c93a433d833b8ad5ecddb0` | KEYBOARD view, HOST layout ru: Cyrillic legends from the generated legend font, usages unchanged (FR-742, FR-743, TECH-078) |
| `keyboard_layout_us.png` | 1024x600 | 19542 | `858b27c2ed6dc4aa35855dd7390a0326f4a3d0ff3a0bcf3f8e7c53ef1d4c4090` | KEYBOARD view, HOST layout us: ANSI geometry, no LSGT key, ASCII legends on their HID usages (FR-742, FR-743) |
| `keyboard_mods.png` | 1024x600 | 20280 | `fb47d68a30ae28ba943325b4d286cf33c801d5cbf4ba4646f13549b98858b14a` | KEYBOARD view, LShift armed and LCtrl locked (FR-204, ADR-016) |
| `keyboard_quick_layout.png` | 1024x600 | 22149 | `3141c5d86d56b625e3eded7bd6a6d882ddcbdfb7fd8e2f71341a473f34e40869` | KEYBOARD view, HOST layout quick switch held open: the pinned layouts first, then the recents that are not pinned, the layout in force marked (ADR-039 section 8) |
| `keyboard_zh.png` | 1024x600 | 23154 | `3c7dcb31dac874b85a71324fed5c591293ec7622b2623917db9215cb2559bdc4` | KEYBOARD view, ZH layer composing "nihao": pinyin input and the candidate strip (ADR-024 FR-746) |
| `maintenance.png` | 1024x600 | 16765 | `68e1b6e450438bfb7f6cbe6b69deb7ed2d9b539e7f9a585333090af2e06f01ed` | SHORTCUTS view during maintenance (OTA): local input blocked |
| `mouse.png` | 1024x600 | 11260 | `be61c4033848f43a87bc7817bab5e3fd5b471b1bf59275548fb1dc87c0fe33b7` | MOUSE view, HOST active, CLIENT available |
| `mouse_drag_lock.png` | 1024x600 | 11445 | `4fb75c40cce792e0cf63ef27e3b44cac1330a04ea6ee885792344408e36ad179` | MOUSE view, drag lock held by the gesture engine (FR-107) |
| `mouse_help.png` | 1024x600 | 20113 | `e4aa60e0c220c288f297af2a8d67577fe8eb9a933e4e138052c28dab63ce18a5` | MOUSE view, the gesture help card opened with the "?" key |
| `owner_remote.png` | 1024x600 | 16926 | `b0ab09e857f5d684954d91edb678edb32fc7533b37bd7bde587ca481a68bb5d8` | SHORTCUTS view, control owner REMOTE: local commands blocked (FR-006, FR-308) |
| `pair_accept.png` | 1024x600 | 16059 | `69164f3826d9da27f3fcfc9298937c37cf4253c9463854078104a097e7d3de38` | Pairing - Accept or Reject the named client |
| `pair_code.png` | 1024x600 | 16032 | `c85534c30c12d1ec5bf053d206c1d005df523acb3bcdfd31ea0664e3fcc143ea` | Pairing window - the code and the two countdowns |
| `pair_toast_paired.png` | 1024x600 | 12059 | `db3161a13f220e665f1a6e231f665ab1b6599550838bde8856d29ea2c4ebb936` | Pairing outcome - paired, reported once |
| `pair_unavailable.png` | 1024x600 | 16649 | `0f30fbdfa63aae5312b97ecd46459d9123ff49f9715605c1fca896c07c67ae19` | CLIENT details - pairing unavailable while remote is off |
| `settings_about.png` | 1024x600 | 42527 | `188eb4db05e856e3f97f6e5ac4499ed87e0b54cad8524399ea5fda2dece21823` | Settings, About with the license, the Required Notice and the generated third-party notices (FR-751) |
| `settings_clients.png` | 1024x600 | 13977 | `92cb4a7469a75a6e5d70220e6ccfa97916ac8a1014cc27c271a48498291db3af` | Settings, paired clients with the FR-733 trusted flag and Forget (FR-726) |
| `settings_clients_full.png` | 1024x600 | 21581 | `93a72589e7aaa9de168a6ad8d63c187367e2c13de0fc2548fa02fb03c02fdf7a` | Settings, a full paired-client register (8 of 8): the list scrolls, no row is clipped |
| `settings_confirm.png` | 1024x600 | 19077 | `cbc687a792ab646c463ebccddd0ee84b4643a10c13e1ebaea153dea438a992b0` | Settings, confirmation required before pair.require_accept is switched off (ADR-023) |
| `settings_forget.png` | 1024x600 | 15973 | `9b8334f8c7ba5180082c42b24acc59e0a6b1af6aaff0f4d449efa62e62240e79` | Settings, Forget a paired client behind a confirmation (FR-726, ADR-023) |
| `settings_host_layout.png` | 1024x600 | 14636 | `4a941b07bd74d4fda21ff8c0a270e28138c7aa26a1e36022f40f2d943bba3896` | Settings, Keyboard section: the HOST layout picker at its first level, opened on the page that holds the stored id (FR-741) |
| `settings_host_layout_home.png` | 1024x600 | 17449 | `a5c7da27d19d86754d88453d15602c7f9b04d3840294951768f49cf15eca9a82` | Settings, HOST layout picker home: the layout in force with its home row, eight pinned favorites and the recents (ADR-039) |
| `settings_host_layout_page.png` | 1024x600 | 14636 | `4a941b07bd74d4fda21ff8c0a270e28138c7aa26a1e36022f40f2d943bba3896` | Settings, HOST layout picker paged on: a page of layouts with no stored id among them, Prev and Next both live (FR-741) |
| `settings_host_layout_search.png` | 1024x600 | 18183 | `1035579c6866b36220214099eff2c1191df7fa5e6fbfe6116b7fce9427c40a76` | Settings, HOST layout picker search: the query, the picker's own keyboard and the layouts whose id or name match (ADR-039) |
| `settings_host_layout_star.png` | 1024x600 | 19986 | `aba949b8962eab2d3e78d69bf5c9aec31b2d5416b6f34ecd6a1b11eb70e3b3e7` | Settings, HOST layout list: the A-Z strip above the rows, one bucket away from the top, and a ninth favorite refused (ADR-039) |
| `settings_host_layout_variants.png` | 1024x600 | 18747 | `ad6050dd6718ae39cf599d9841fca60903cc831eb039c91da3287edc8434bd54` | Settings, HOST layout picker at its second level: the variants of one layout, Back returns to the layout list (FR-741) |
| `settings_input.png` | 1024x600 | 13570 | `8d256c3754ce10460d19f5ff9a376fd1f9a9058ac7802f6dacfbd6211a26aa35` | Settings, Input section with the keep-awake interval stepper (FR-711, ADR-020) |
| `settings_keyboard.png` | 1024x600 | 22629 | `3025c340ed0289b8e94ad8d4bd8d6cfbbbb6eb7095d4e806221afd2212d1541f` | Settings, Keyboard section: the input language list with English always on, the HOST layout row with the stored id, Unicode delivery stepper with its setup hint (ADR-037, FR-741, FR-747) |
| `settings_remote.png` | 1024x600 | 19103 | `d32d7abe92e31230e2d39604d3d6bd5b956f85671e6c34d24572ee9452d1840f` | Settings, Remote section with the ADR-023 safe defaults (FR-731, FR-732, FR-733) |
| `settings_shortcuts.png` | 1024x600 | 10988 | `ad520fce4b48603a13f3ab1708e7accc3f6c07c0d4cbb676b2e51dede13de69c` | Settings, Shortcuts section with the FR-734 Alt+F4 confirmation policy (ADR-023) |
| `settings_unknown.png` | 1024x600 | 18939 | `70ed4cbf3de7445d8b263420e09282dd579610a8ee69524063e25db13ac01b1e` | Settings before the domain published a record: every value Unknown, every control off (07 12.2) |
| `shortcuts.png` | 1024x600 | 18707 | `6991846baeafb7376e4b2ba05227ce1110b63018f97612ae6951680b011b84f4` | SHORTCUTS view, commands available |
| `shortcuts_confirm.png` | 1024x600 | 21238 | `cd9d13213883a9c3159e2bb55c6cd1c2f673e08e3ac7c1d43c6b862211a1f657` | SHORTCUTS view, Alt+F4 confirmation modal (FR-734, ADR-023) |
| `shortcuts_host_lost.png` | 1024x600 | 17179 | `eacedc5b742b473bf809bb14ddaea14036f7537b0daeaa9b6d219c194f1d4d91` | SHORTCUTS view, HOST detached: local commands blocked (FR-308) |
| `shortcuts_layout_ctrl.png` | 1024x600 | 18676 | `abd31072b015fb26a461b22636d121f884939db8315a6ab774c735dda1da917e` | SHORTCUTS view, HOST layout chord for macOS / Linux (FR-744) |
| `shortcuts_toast.png` | 1024x600 | 18813 | `df1906d703a5dab11f73d58ae4ed594795ccbe1b63028e9d4a59a79086c82bfe` | SHORTCUTS view, Show desktop requested (the toast claims nothing about the HOST) |
