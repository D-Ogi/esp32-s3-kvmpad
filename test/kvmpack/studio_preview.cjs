// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Run with Node to exercise the emitted simulator offline. Pass a Playwright module
// as --browser=MODULE to also run the same interactions in a real Chromium page.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {pathToFileURL} = require("node:url");

class Element {
    constructor(tag) {
        this.tag = tag;
        this.dataset = {};
        this.style = {setProperty(key, value) { this[key] = value; }};
        this.listeners = {};
        this.children = [];
        this.textContent = "";
    }
    append(child) { this.children.push(child); child.parent = this; }
    focus() { this.focused = true; }
    remove() { this.parent.children = this.parent.children.filter(child => child !== this); }
    replaceChildren(...children) { this.children = children; }
    setAttribute(key, value) { this[key] = value; }
    setPointerCapture() {}
    addEventListener(key, handler) { (this.listeners[key] ??= []).push(handler); }
    fire(type, detail) {
        for (const fn of this.listeners[type] || []) fn(detail || {});
        this["on" + type]?.(detail || {});
    }
    getBoundingClientRect() {
        const s = this.style;
        return {left: parseInt(s.left), top: 32 + parseInt(s.top), width: parseInt(s.width), height: parseInt(s.height)};
    }
}

function fixture(file, clock, configure) {
    const source = fs.readFileSync(file, "utf8");
    const elements = {};
    for (const id of ["pack","surface","status","title","connections","demo","home","palette"]) elements[id] = new Element("div");
    elements.palette.value = "system";
    const data = source.match(/<script id="pack" type="application\/json">([\s\S]*?)<\/script>/)[1];
    const pack = JSON.parse(data);
    configure?.(pack);
    elements.pack.textContent = JSON.stringify(pack);
    const program = source.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
    const document = {documentElement: new Element("html"), createElement: tag => new Element(tag), querySelector: key => elements[key.slice(1)]};
    const media = new Map();
    const matchMedia = query => {
        if (!media.has(query)) media.set(query, new Element("media"));
        return media.get(query);
    };
    const ctx = vm.createContext({document, matchMedia, performance: clock, setInterval: () => 1, clearInterval: () => {},
        setTimeout: clock?.setTimeout ?? setTimeout, clearTimeout: clock?.clearTimeout ?? clearTimeout});
    const run = script => vm.runInContext(script, ctx, {timeout: 1000});
    run(program);
    const widget = id => run("widgets.get(" + JSON.stringify(id) + ")?.element");
    return {widget, elements, run, media};
}

function checkEvents(folder) {
    const open = name => fixture(path.join(folder, name + ".html"));
    const checklist = open("flight-checklist");
    assert.equal(checklist.elements.connections.textContent, "");
    const item = id => checklist.widget(id);
    const text = () => item("status").children[0].textContent;
    assert.equal(text(), "Pending");
    item("complete").fire("click");
    assert.equal(text(), "Complete");
    const palette = () => checklist.run("document.documentElement.dataset.palette");
    assert.equal(palette(), "day");
    const [dark, contrast] = checklist.media.values();
    dark.matches = true; dark.fire("change");
    assert.equal(palette(), "night");
    contrast.matches = true; contrast.fire("change");
    assert.equal(palette(), "contrast");
    for (const choice of ["night","day","contrast","system"]) {
        checklist.elements.palette.value = choice;
        checklist.elements.palette.fire("change");
        assert.equal(palette(), choice === "system" ? "contrast" : choice);
        assert.equal(text(), "Complete");
    }
    item("reset").fire("click");
    assert.equal(text(), "Pending");
    item("complete").fire("click");
    checklist.elements.home.fire("click");
    checklist.elements.surface.children[0].children[0].fire("click");
    assert.equal(text(), "Pending");
    const yoke = open("flight-yoke");
    const pointer = {pointerId: 1, clientX: 480, clientY: 174};
    yoke.widget("yoke").fire("pointerdown", pointer);
    assert.match(yoke.elements.status.textContent, /X 0.50\s+Y -0.50/);
    yoke.widget("yoke").fire("pointercancel", pointer);
    assert.match(yoke.elements.status.textContent, /X 0.00\s+Y 0.00/);
    const slider = yoke.widget("throttle").children[1];
    slider.value = ".75"; slider.fire("input");
    assert.match(yoke.elements.status.textContent, /Z 0.75/);
    yoke.widget("brake").fire("pointerdown", pointer);
    assert.match(yoke.elements.status.textContent, /B1 down/);
    yoke.widget("brake").fire("lostpointercapture", pointer);
    assert.match(yoke.elements.status.textContent, /B1 up/);
    const gauges = open("flight-instruments");
    assert.equal(yoke.elements.connections.textContent, "USB detached");
    assert.equal(gauges.elements.connections.textContent, "Agent idle  ·  Data unavailable");
    gauges.elements.demo.fire("click");
    assert.equal(gauges.elements.connections.textContent, "Agent idle  ·  Demo data");
    assert.match(gauges.widget("gauge_ias").children[1].textContent, /^117/);
    assert.match(gauges.widget("horizon").children[1].textContent, /Pitch 3°  Roll 0°/);
    gauges.elements.demo.fire("click");
    assert.equal(gauges.elements.status.textContent, "Demo paused");
    const cursor = open("cursor-magnifier");
    assert.equal(cursor.elements.connections.textContent, "Agent idle  ·  Capture off");
    cursor.widget("start").fire("click");
    assert.match(cursor.elements.status.textContent, /capture unavailable/);
    cursor.widget("stop").fire("click");
    assert.equal(cursor.elements.status.textContent, "Stopped");
    const panel = fixture(path.join(folder, "flight-checklist.html"), undefined, pack => {
        const root = pack.ui.root;
        root.bounds.h = 520;
        pack.ui.screens = ["check_page", "setup"];
        root.children = pack.ui.screens.map((id, i) => ({
            type: "Column", id, bounds: {...root.bounds}, props: {title: i ? "Setup" : "Checklist"},
            children: i ? [] : root.children
        }));
    });
    const handle = panel.elements.surface.children.at(-1);
    const touch = (type, x, y) => handle.fire(type, {pointerId: 1, clientX: x, clientY: y});
    assert.equal(panel.elements.home.hidden, true);
    assert.equal(panel.widget("setup").hidden, true);
    panel.widget("complete").fire("click");
    touch("pointerdown", 450, 580); touch("pointerup", 510, 540);
    touch("pointerdown", 450, 580); touch("pointercancel", 450, 520); touch("pointerup", 450, 510);
    assert.equal(handle.hidden, false);
    touch("pointerdown", 450, 580); touch("pointerup", 450, 515);
    const picker = () => panel.elements.surface.children.at(-1);
    assert.equal(picker().className, "screen-picker");
    picker().children[1].children[1].fire("click");
    assert.equal(panel.widget("setup").hidden, false);
    assert.equal(panel.widget("check_page").hidden, true);
    handle.fire("click", {detail: 0});
    picker().children[1].children[0].fire("click");
    assert.equal(panel.widget("status").children[0].textContent, "Complete");
    touch("pointerdown", 500, 580); touch("pointerup", 500, 580);
    picker().children.at(-1).fire("click");
    assert.equal(panel.elements.title.textContent, "KVMpad");
}

async function checkBrowser(page, folder, screenshot) {
    const open = name => page.goto(pathToFileURL(path.join(folder, name + ".html")).href);
    await page.setViewportSize({width: 1024, height: 600});
    await open("flight-checklist");
    const widget = id => page.locator('[data-node="' + id + '"]');
    await widget("complete").click();
    assert.equal(await widget("status").innerText(), "Complete");
    const appearance = page.getByLabel("Appearance");
    for (const [choice, ink, surface] of [
        ["night", "rgb(240, 245, 250)", "rgb(32, 44, 57)"],
        ["contrast", "rgb(255, 255, 255)", "rgb(0, 0, 0)"],
        ["day", "rgb(29, 29, 31)", "rgb(255, 255, 255)"],
    ]) {
        await appearance.selectOption(choice);
        const colors = await widget("complete").evaluate(el => {
            const style = getComputedStyle(el);
            return [style.color, style.backgroundColor];
        });
        assert.deepEqual(colors, [ink, surface]);
        const titleStyle = await widget("title").evaluate(el => [getComputedStyle(el).color, getComputedStyle(el).backgroundColor]);
        assert.deepEqual(titleStyle, [ink, "rgba(0, 0, 0, 0)"]);
        assert.equal(await widget("status").innerText(), "Complete");
    }
    await appearance.selectOption("system");
    if (screenshot) await page.screenshot({path: screenshot});
    await page.locator("#home").click();
    await page.locator(".recovery button").click();
    assert.equal(await widget("status").innerText(), "Pending");
    await open("flight-yoke");
    await page.mouse.move(480,174);
    await page.mouse.down();
    assert.match(await page.locator("#status").innerText(), /X 0.50\s+Y -0.50/);
    await page.mouse.up();
    assert.match(await page.locator("#status").innerText(), /X 0.00\s+Y 0.00/);
    await open("flight-instruments");
    await page.locator("#demo").click();
    assert.doesNotMatch(await widget("gauge_ias").innerText(), /—/);
    await open("cursor-magnifier");
    assert.deepEqual(await widget("live_preview").evaluate(el => [getComputedStyle(el).color, getComputedStyle(el).backgroundColor]),
        ["rgb(255, 255, 255)", "rgb(0, 0, 0)"]);
    await widget("start").click();
    assert.match(await page.locator("#status").innerText(), /capture unavailable/);
}

async function main() {
    const [folder, browserOption, screenshot] = process.argv.slice(2);
    if (!folder) throw Error("Pass a directory containing the four exported example HTML files.");
    checkEvents(folder);
    console.log("Passed synthetic DOM events on all four exported example profiles.");
    if (browserOption) {
        if (!browserOption.startsWith("--browser=")) throw Error("Use --browser=playwright");
        const {chromium} = require(browserOption.slice("--browser=".length));
        const browser = await chromium.launch({headless: true});
        try { await checkBrowser(await browser.newPage(), folder, screenshot); }
        finally { await browser.close(); }
        console.log("Passed Chromium interactions on all four profiles.");
    }
}

module.exports = {checkBrowser, fixture};
if (require.main === module) main().catch(error => { console.error(error); process.exitCode = 1; });
