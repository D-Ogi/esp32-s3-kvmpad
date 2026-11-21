// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
const assert = require("node:assert/strict");
const path = require("node:path");
const {fixture} = require("./studio_preview.cjs");
for (const style of ["linear", "calibrated"]) {
    const view = fixture(path.join(process.argv[2], style + ".html"));
    const text = () => view.elements.status.textContent;
    const fire = (name, type, x = 0, y = 0, id = 1) =>
        view.widget(name).fire(type, {pointerId:id, clientX:x, clientY:y});
    for (const [n, direction] of ["n","ne","e","se","s","sw","w","nw"].entries()) {
        fire("hat_" + direction, "pointerdown");
        assert.equal(text(), "Simulated · Hat " + n);
        fire("hat_" + direction, ["pointerup","pointercancel","lostpointercapture"][n % 3]);
        assert.equal(text(), "Simulated · Hat 8");
    }
    fire("hat_e","pointerdown");
    fire("hat_w","pointerdown",0,0,2);
    fire("hat_w","pointerup",0,0,2);
    assert.equal(text(), "Simulated · Hat 2");
    fire("hat_e","pointerup");
    assert.equal(text(), "Simulated · Hat 8");
    fire("yoke","pointerdown",320,316);
    assert.match(text(), /X 0.00\s+Y 0.00/);
    fire("yoke","pointermove",355,316);
    assert.match(text(), style==="linear" ? /X 0.11\s+Y 0.00/ : /X 0.00\s+Y 0.00/);
    fire("yoke","pointermove",480,175);
    assert.match(text(), style==="linear" ? /X 0.50\s+Y -0.50/ : /X 0.05\s+Y 0.05/);
    fire("yoke","pointermove",639,32);
    assert.match(text(), style==="linear" ? /X 1.00\s+Y -1.00/ : /X 1.00\s+Y 1.00/);
    fire("yoke","pointercancel");
    assert.match(text(), /X 0.00\s+Y 0.00/);
    fire("hat_s","pointerdown");
    view.elements.home.fire("click");
    assert.equal(view.run("hatOwner"), null);
}
console.log("PASS offline preview: eight POV directions, second pointer isolation, cancellation, Home and both axis profiles");
