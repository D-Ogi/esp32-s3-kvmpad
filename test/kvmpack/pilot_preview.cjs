// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
const assert = require("node:assert/strict");
const {fixture} = require("./studio_preview.cjs");
let elapsed;
const mock = fixture(process.argv[2], {
    now() { return 0; },
    setTimeout(fn) { elapsed=fn; return 1; },
    clearTimeout() { elapsed=null; }
});
const chart=mock.widget("charts");
const [slots,map,tools]=chart.children;
const [a,b,which]=slots.children;
const img=map.children[0];
assert.match(img.alt,/Taxi/);
tools.children[2].fire("click");
const oldZoom=tools.children[1].textContent;
assert.notEqual(oldZoom,"100%");
b.fire("click");
assert.match(img.alt,/Runway 23/);
assert.equal(tools.children[1].textContent,"100%");
which.value="0"; which.fire("change");
assert.match(img.alt,/Taxi/);
assert.equal(tools.children[1].textContent,oldZoom);
a.fire("click");
assert.match(img.alt,/Runway 23/);

const panel=mock.widget("autopilot_panel");
function walk(node) { return [node,...node.children.flatMap(walk)]; }
{
    const radio=mock.widget("radio_panel");
    const tabs=mock.widget("radio_selector");
    assert.equal(tabs.role,"tablist");
    assert.ok(!walk(radio).some(el=>el.textContent==="COM 1" || el.textContent==="COM 2"));
    function bank(i) {
        tabs.children[i].fire("click");
        assert.equal(tabs.children[i]["aria-selected"],"true");
    }
    function read(text) {
        const node=walk(radio).find(el=>el.textContent===text);
        assert.ok(node,text);return node;
    }
    function clickRadio(text) {
        let b=read(text);
        while(b.tag!=="button")b=b.parent;
        b.fire("click");
    }
    function enter(s){for(const ch of s)clickRadio(ch);}
    read("Offline");assert.equal(read("Swap").disabled,true);
    clickRadio("Start demo");read("Demo");read("118.100 MHz");read("121.900 MHz");
    clickRadio("STANDBY");enter("118020");assert.equal(read("OK").disabled,true);
    clickRadio("⌫");clickRadio("⌫");enter("05");clickRadio("OK");
    read("121.900 MHz");assert.equal(read("Swap").disabled,true);elapsed();read("118.005 MHz");
    clickRadio("Swap");elapsed();assert.equal(read("STANDBY").parent.children[1].textContent,"118.100 MHz");
    clickRadio("STANDBY");enter("120");bank(1);read("124.850 MHz");assert.ok(read("OK").disabled);
    clickRadio("STANDBY");enter("136990");clickRadio("OK");bank(0);elapsed();
    read("118.005 MHz");read("118.100 MHz");clickRadio("7000");
    assert.ok(read("8").disabled && read("9").disabled);enter("007");assert.ok(read("OK").disabled);
    enter("3");clickRadio("OK");elapsed();read("0073");clickRadio("ON");elapsed();
    assert.equal(read("ON")["aria-selected"],"true");
    clickRadio("STANDBY");enter("120");mock.run('widgets.get("radio_panel").cancel()');
    read("118.100 MHz");assert.ok(read("OK").disabled);
    bank(1);read("136.990 MHz");clickRadio("Swap");
    mock.run('widgets.get("radio_panel").cancel()');assert.equal(elapsed,null);read("123.450 MHz");
    clickRadio("Go offline");read("Offline");read("----");assert.ok(read("Swap").disabled);
    bank(0);clickRadio("Start demo");read("118.100 MHz");bank(1);read("123.450 MHz");
    const legacy=fixture(process.argv[2],undefined,pack=>{
        pack.bindings.connections=pack.bindings.connections.filter(c=>c.to!=="radio_panel.selected");
    }).widget("radio_panel");
    for(const name of ["COM 1","COM 2"])assert.ok(walk(legacy).some(el=>el.textContent===name));
    console.log("PASS radio offline, independent COM channels, keypad validation, swap, octal code, modes and navigation");
}
function find(text) {
    const node=walk(panel).find(el=>el.textContent===text);
    assert.ok(node, text);
    return node;
}
function click(text) { find(text).fire("click"); }
assert.equal(find("AP · Offline").disabled,true);
const session=mock.widget("autopilot_session");
function chooseSession(index) {
    session.children[index].fire("click");
    assert.equal(session.children[index]["aria-selected"],"true");
}
assert.equal(session.children[0]["aria-selected"],"true");
assert.ok(!walk(panel).some(el=>el.textContent==="Start demo"));
chooseSession(1);
click("Heading");
click("+10"); click("Cancel");
find("Selected · 275°");
click("Heading");
const input=walk(panel).find(el=>el.tag==="input");
input.value="361";input.fire("input");
assert.equal(find("Send setting").disabled,true);
input.value="360";input.fire("input");click("Send setting");
find("Awaiting confirmation · 000°");
find("Selected · 275°");
elapsed();
find("Selected · 000°");
click("Demo simulator"); click("Reject next command · Off"); click("Close");
click("Altitude"); click("+1000"); click("Send setting"); elapsed();
find("Command rejected");find("Selected · 6000 ft");
click("NAV"); elapsed();find("Armed · NAV");
click("Demo simulator");click("Capture NAV");click("Close");find("Active · NAV");
click("Vertical speed");click("-500");click("Send setting");
chooseSession(0);
assert.equal(elapsed,null);
assert.equal(find("AP · Offline").disabled,true);
find("Last received · +800 ft/min");
chooseSession(1);find("Selected · 275°");
click("Altitude");click("+1000");chooseSession(0);
assert.equal(find("AP · Offline").disabled,true);
chooseSession(1);find("Selected · 6000 ft");
click("Demo simulator");assert.ok(!walk(panel).some(el=>el.textContent==="Disconnect demo"));
chooseSession(0);assert.equal(find("AP · Offline").disabled,true);
const legacyPanel=fixture(process.argv[2],undefined,pack=>{
    pack.bindings.connections=pack.bindings.connections.filter(c=>c.to!=="autopilot_panel.selected");
}).widget("autopilot_panel");
for (const name of ["Start demo","Demo simulator","Disconnect demo","Start demo"]) {
    const b=walk(legacyPanel).find(el=>el.tag==="button" && el.textContent===name);
    assert.ok(b,name);b.fire("click");
}
console.log("PASS pinned chart swap and zoom; autopilot offline, draft, bounds, confirmation, rejection, mode capture and disconnect");

const aircraftTab = (id,i) => mock.widget(id).children[i];
const selectedAircraft = (id,i) => assert.equal(aircraftTab(id,i)["aria-selected"],"true");
aircraftTab("aircraft_setup",1).fire("click");
for (const id of ["aircraft_setup","aircraft_taxi","aircraft_approach"]) selectedAircraft(id,1);
assert.equal(mock.widget("da40_taxi").hidden,false);
assert.equal(mock.widget("c172_approach").hidden,true);
aircraftTab("da40_taxi_1_check",1).fire("click");
aircraftTab("aircraft_approach",0).fire("click");
selectedAircraft("aircraft_setup",0);
assert.equal(mock.widget("c172_approach").hidden,false);
selectedAircraft("c172_taxi_1_check",0);
aircraftTab("aircraft_taxi",1).fire("click");
selectedAircraft("da40_taxi_1_check",1);
console.log("PASS shared aircraft selection with independent checklist items");
