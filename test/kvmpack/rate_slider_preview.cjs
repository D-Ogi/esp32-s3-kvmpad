// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
const assert = require("node:assert/strict");
const {fixture} = require("./studio_preview.cjs");
let now=0, pending=null;
const sim=fixture(process.argv[2],{
    now:()=>now,
    setTimeout(fn,ms) { pending={fn,ms}; return 1; },
    clearTimeout() { pending=null; }
});
const wheel=sim.widget("wheel"), reading=()=>sim.widget("altitude_value").children[0].textContent;
for (const [button, value] of [["down_large","0"],["down_small","0"],["up_large","1000"],
                               ["up_small","1100"],["down_small","1000"]]) {
    sim.widget(button).fire("click");
    assert.equal(reading(),value);
}
function touch(type,y,x=44) { wheel.fire(type,{pointerId:1, clientX:x, clientY:y}); }
touch("pointerdown",124); assert.equal(reading(),"1000"); assert.equal(pending,null);
touch("pointermove",106); assert.equal(reading(),"1100");
const slow=pending.ms;
touch("pointermove",54); assert.equal(reading(),"1100");
assert.ok(pending.ms<slow);
now+=pending.ms; pending.fn(); assert.equal(reading(),"1200");
touch("pointermove",193); assert.equal(reading(),"1100");
touch("pointermove",124); assert.equal(pending,null);
touch("pointermove",193); assert.equal(reading(),"1000");
touch("pointerup",193); assert.equal(pending,null);
touch("pointerdown",54); touch("lostpointercapture",54); assert.equal(pending,null);
const start=Number(reading());
touch("pointerdown",124,110);
touch("pointermove",24); assert.equal(pending,null); assert.equal(Number(reading()),start);
touch("pointerdown",124);
touch("pointermove",24); assert.equal(Number(reading()),start+100);
for (let i=0;i<3;i++) { assert.equal(pending.ms,65); now+=pending.ms; pending.fn(); }
touch("pointermove",24,110);
assert.equal(pending.ms,65); now+=pending.ms; pending.fn(); assert.equal(Number(reading()),start+500);
touch("pointermove",124,110); assert.equal(pending,null);
touch("pointermove",244,110); assert.equal(Number(reading()),start+400);
for (let i=0;i<3;i++) { assert.equal(pending.ms,65); now+=pending.ms; pending.fn(); }
touch("pointerup",244,110); assert.equal(pending,null);
touch("pointermove",24); assert.equal(Number(reading()),start+100); assert.equal(pending,null);
for(let i=0;i<15;i++) wheel.fire("keydown",{key:"ArrowDown",preventDefault(){}});
assert.equal(reading(),"0");
sim.run('state.altitude=19999n; dispatch("increase")'); assert.equal(reading(),"20000");
sim.elements.home.fire("click"); assert.equal(pending,null);
