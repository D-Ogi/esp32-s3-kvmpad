// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
const assert = require("node:assert/strict");
const {fixture} = require("./studio_preview.cjs");
let timeout;
const clock = {now:() => 1200, setTimeout:fn => {timeout=fn; return 1;}, clearTimeout:() => {timeout=null;}};
const {widget,elements} = fixture(process.argv[2],clock);
const touch=widget("pad"), shown=() => widget("chip").children[0].textContent;
assert.equal(shown(),"0");
assert.equal(widget("chip").style["--chip"],"var(--ok)");
assert.equal(widget("chip").children[1].dataset.symbol,"✓");
const fire=(type,x=40,y=64) => touch.fire(type,{pointerId:1,clientX:x,clientY:y});
fire("pointerdown"); fire("pointerup"); assert.equal(shown(),"1");
fire("pointerdown"); timeout(); fire("pointerup"); assert.equal(shown(),"2");
fire("pointerdown"); fire("pointermove",100); fire("pointerup",100); assert.equal(shown(),"90");
fire("pointerdown"); fire("pointercancel"); fire("pointerup"); assert.equal(shown(),"90");
const ring=widget("dial").children[0];
ring.getBoundingClientRect=() => ({width:120,height:120,left:0,top:0});
ring.fire("pointerdown",{pointerId:1,clientX:60,clientY:0});
ring.fire("pointermove",{pointerId:1,clientX:120,clientY:60});
ring.fire("pointerup",{pointerId:1});
assert.equal(shown(),"66");
ring.fire("keydown",{key:"End",preventDefault() {}});
assert.equal(shown(),"100");
const tabs=widget("tabs").children;
assert.equal(widget("two").hidden,true);
tabs[1].fire("click"); assert.equal(shown(),"1"); assert.equal(widget("one").hidden,true);
tabs[0].fire("click"); assert.equal(shown(),"0"); assert.equal(widget("two").hidden,true);
fire("pointerdown"); elements.home.fire("click"); assert.equal(timeout,null);
elements.surface.children[0].children[0].fire("click"); assert.equal(shown(),"0");
console.log("Passed offline touch, hold, drag, cancel, rotary, tab pages, chip and Home reset.");
