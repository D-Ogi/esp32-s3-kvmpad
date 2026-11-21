// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
const assert = require("node:assert/strict");
const {fixture} = require("./studio_preview.cjs");
const {widget, elements, run} = fixture(process.argv[2]);
assert.equal(run("document.documentElement.dataset.orientation"), "portrait");
assert.equal(elements.home.hidden, true);
assert.equal(elements.connections.textContent, "USB detached");
assert.equal(widget("desktop").children[0].textContent, "USB desktop offline");
widget("desktop").children[1].fire("click");
assert.match(elements.status.textContent, /on KVMpad/);
assert.equal(elements.connections.textContent, "USB detached");
assert.equal(widget("desktop").children[1].textContent, "Start USB monitor");
const modes = id => widget(id).children[2].children;
assert.deepEqual(modes("desktop").map(b => b.textContent), ["640×480", "1024×600"]);
assert.equal(modes("desktop")[0]["aria-pressed"], "true");
modes("desktop")[1].fire("click");
assert.equal(modes("desktop")[0]["aria-pressed"], "false");
assert.equal(widget("keyboard").children.length, 4);
assert.deepEqual(widget("keyboard").children.at(-1).children.map(k => k.textContent),
                 ["123", "Ctrl", "Alt", "R Alt", "Tab", "Caps\nUnknown", "Space", "◀", "▶"]);
const key = name => widget("keyboard").children.flatMap(row => row.children).find(k => k.textContent === name);
for (const origin of ["letters", "numbers"]) {
    if (origin === "numbers") key("123").fire("click");
    key("Fn").fire("click");
    for (let i = 1; i <= 12; i++) assert.ok(key("F" + i));
    for (const name of ["Win", "Win+", "Ins", "PgUp", "PgDn"]) assert.ok(key(name));
    assert.ok(key("Back").focused);
    key("Back").fire("click");
    assert.ok(key("Fn").focused);
    assert.ok(key(origin === "numbers" ? "ABC" : "q"));
}
key("ABC").fire("click");
const gestures = widget("touchpad").children.find(child => child.textContent === "Gestures");
gestures.fire("click");
let overlay = elements.surface.children.at(-1);
const panel = overlay.children[0];
assert.equal(panel["aria-label"], "Touchpad gestures");
assert.deepEqual(panel.children.filter(child => child.disabled).map(child => child.textContent),
                 ["Left click", "Right click", "Middle click"]);
assert.ok(panel.children[0].focused);
assert.ok(elements.surface.children[0].inert);
panel.children[0].fire("click");
assert.ok(!elements.surface.children[0].inert && gestures.focused);
gestures.fire("click");
overlay = elements.surface.children.at(-1);
overlay.fire("keydown", {key:"Escape"});
assert.ok(!elements.surface.children.includes(overlay));
gestures.fire("click");
overlay = elements.surface.children.at(-1);
overlay.fire("click", {target:overlay});
assert.ok(!elements.surface.children.includes(overlay));
assert.equal(widget("touchpad").style.top, "454px");
assert.equal(widget("keyboard").style.top, "648px");
assert.equal(widget("pointer").hidden, true);
elements.surface.children.at(-1).fire("click", {detail: 0});
const picker = elements.surface.children.at(-1);
const buttons = picker.children[1].children;
assert.deepEqual(buttons.map(b => b.textContent), ["Console", "Pointer"]);
buttons[1].fire("click");
assert.equal(widget("console").hidden, true);
assert.equal(widget("pointer").hidden, false);
assert.equal(modes("pointer_desktop")[1]["aria-pressed"], "true");
assert.equal(widget("large_touchpad").style.height, "482px");
assert.equal(widget("pointer_desktop").style.height, "438px");
console.log("PASS offline USB console, explicit local start, portrait geometry and screen switch");
