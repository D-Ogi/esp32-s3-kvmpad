// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

function autopilotPreview(panel) {
    const fields = {
        HDG: {name:"Heading", unit:"°", min:0, max:360, steps:[-10,-1,1,10]},
        ALT: {name:"Altitude", unit:"ft", min:0, max:45000, steps:[-1000,-100,100,1000]},
        VS: {name:"Vertical speed", unit:"ft/min", min:-6000, max:6000, steps:[-500,-100,100,500]}
    };
    const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi,n));
    const wrap = n => (n % 360 + 360) % 360;
    const format = (key,n) => key === "HDG" ? String(n).padStart(3,"0")+"°" :
        (key === "VS" && n > 0 ? "+" : "")+n+" "+fields[key].unit;
    let demo = false, reviewed = false, ap = true, lateral = "HDG", vertical = "VS";
    let armNav = false, armAlt = true, reportsArmed = true, reject = false, slow = false;
    let editing = null, settings = false, timer = null, notice = "Offline";
    const target = {HDG:275, ALT:6000, VS:800}, actual = {HDG:268, ALT:5400, VS:700};
    function add(parent,tag,caption,className) {
        const node = document.createElement(tag);
        if (caption !== undefined) node.textContent = caption;
        if (className) node.className = className;
        parent.append(node);
        return node;
    }
    function button(parent, name, action, disabled=false) {
        const b = add(parent,"button",name);
        b.disabled = disabled;
        b.onclick = () => { if (!b.disabled) action(); };
        return b;
    }
    function arming() {
        armAlt = ap && vertical === "VS" && (target.ALT-actual.ALT)*target.VS > 0;
    }
    function capture() {
        actual.ALT = target.ALT; actual.VS = 0; vertical = "ALT"; armAlt = false;
    }
    function instrument(parent,key) {
        const canvas=add(parent,"canvas");
        canvas.width=164; canvas.height=64;
        canvas.setAttribute("aria-hidden","true");
        const ctx=canvas.getContext?.("2d");
        if (!ctx) return;
        const ink=getComputedStyle(parent).color, accent=getComputedStyle(panel).getPropertyValue("--accent");
        function line(x,y,toX,toY,color,width=1) {
            ctx.strokeStyle=color; ctx.lineWidth=width;
            ctx.beginPath(); ctx.moveTo(x,y); ctx.lineTo(toX,toY); ctx.stroke();
        }
        const label=(text,x,y) => {ctx.fillStyle=ink;ctx.fillText(text,x,y);};
        ctx.font="10px monospace"; ctx.textAlign="center";
        if (key==="HDG") {
            const bearing=demo?actual.HDG:0;
            function point(delta,r) {
                const a=delta*Math.PI/180;
                return [82+Math.sin(a)*r,100-Math.cos(a)*r];
            }
            for (let d=-60;d<=60;d++) {
                const tick=wrap(bearing+d);
                if (tick%5) continue;
                line(...point(d,83),...point(d,tick%10?78:73),ink);
                if (demo && tick%30===0) label(String(tick).padStart(3,"0"),...point(d,63));
            }
            if (demo) {
                const d=clamp(wrap(target.HDG-bearing+180)-180,-60,60);
                line(...point(d,82),...point(d,95),accent,4);
                line(82,15,82,27,ink,3);
            }
        } else {
            const vs=key==="VS", center=vs?0:actual.ALT, span=vs?6000:1200;
            const pos=n=>82+clamp((n-center)/span,-1,1)*76;
            line(6,32,158,32,ink);
            for (let i=-4;i<=4;i++) line(82+i*19,28,82+i*19,37+(i%2?0:4),ink);
            if (demo) {
                const a=pos(actual[key]), t=pos(target[key]);
                line(a,34,a,54,ink,3); line(t,10,t,31,accent,4);
                label(vs?"0 ft/min":Math.round(center)+" ft",82,63);
            }
        }
    }
    function request(description, apply) {
        if (!demo || timer !== null) return;
        editing = null;
        notice = "Awaiting confirmation · "+description;
        timer = setTimeout(() => {
            timer = null;
            if (reject) { reject = false; notice = "Command rejected"; }
            else { apply(); notice = "Confirmed · "+description; }
            render();
        }, slow ? 2500 : 900);
        render();
    }
    function cancel() {
        if (timer !== null) { clearTimeout(timer); timer = null; notice = "Request canceled"; }
        editing = null; settings = false;
        render();
    }
    function editor(key) {
        const f = fields[key];
        add(panel,"strong","Set "+f.name.toLowerCase());
        add(panel,"span","Confirmed · "+format(key,target[key]));
        const input = add(panel,"input");
        input.setAttribute("aria-label",f.name+" in "+f.unit);
        input.value = String(target[key]);
        input.inputMode = key === "VS" ? "text" : "numeric";
        const row = add(panel,"div",undefined,"ap-row");
        function draft() {
            const text = input.value.trim().replace(/−/g,"-");
            if (!/^[+-]?\d+$/.test(text)) return null;
            const n = Number(text);
            return Number.isSafeInteger(n) && n>=f.min && n<=f.max ? (key === "HDG" ? wrap(n) : n) : null;
        }
        for (const step of f.steps) button(row,(step>0?"+":"")+step,() => {
            const n = (draft() ?? target[key])+step;
            input.value = String(key === "HDG" ? wrap(n) : clamp(n,f.min,f.max));
            validate();
        });
        const hint = add(panel,"span",f.min+" to "+f.max+" "+f.unit);
        const actions = add(panel,"div",undefined,"ap-row");
        button(actions,"Cancel",cancel);
        const send = button(actions,"Send setting",() => {
            const n = draft();
            if (n !== null && n !== target[key]) request(format(key,n),() => {target[key]=n; arming();});
        });
        function validate() {
            const value = draft();
            send.disabled = value === null || value === target[key];
            input.setAttribute("aria-invalid",String(value===null));
            hint.textContent = (value===null ? "Enter a whole number from " : "")+f.min+" to "+f.max+" "+f.unit;
        }
        input.addEventListener("input",validate);
        validate();
    }
    function options() {
        add(panel,"strong","Demo simulator");
        function toggle(title,on,change) {
            button(panel,title+" · "+(on?"On":"Off"),() => { change(); render(); });
        }
        toggle("Report armed modes",reportsArmed,() => { reportsArmed=!reportsArmed; });
        toggle("2.5 s response",slow,() => { slow=!slow; });
        toggle("Reject next command",reject,() => { reject=!reject; });
        button(panel,"Advance 30 s",() => {
            if (lateral==="HDG") actual.HDG=wrap(actual.HDG+clamp(wrap(target.HDG-actual.HDG+180)-180,-30,30));
            if (vertical==="VS") {
                const next=actual.ALT+target.VS/2;
                actual.VS=target.VS;
                if (armAlt && (target.ALT-actual.ALT)*(target.ALT-next)<=0) capture();
                else { actual.ALT=Math.round(clamp(next,0,45000)); arming(); }
            }
            render();
        },!ap);
        button(panel,"Capture selected altitude",() => { capture(); render(); },!armAlt);
        button(panel,"Capture NAV",() => { armNav=false; lateral="NAV"; render(); },!armNav);
        button(panel,"Disconnect demo",() => {demo=false; notice="Link lost · last received settings"; cancel();});
        button(panel,"Close",() => {settings=false; render();});
    }
    function render() {
        panel.replaceChildren();
        const heading = add(panel,"div",undefined,"ap-row");
        add(heading,"strong","Autopilot");
        button(heading,demo ? "Demo simulator" : "Start demo",() => {
            if (demo) settings = true;
            else { demo=true; reviewed=true; notice="Demo connected"; }
            render();
        });
        if (settings) { options(); return; }
        if (editing) { editor(editing); return; }
        const blocked = !demo || timer !== null;
        button(panel,demo ? (ap ? "AP ON" : "AP OFF") : "AP · Offline",() => request(ap?"AP off":"AP on",() => {
            ap=!ap; armAlt=armNav=false; lateral=ap?"ROL":null; vertical=ap?"PIT":null;
        }),blocked);
        const grid = add(panel,"div",undefined,"ap-telemetry");
        for (const [name,active,armed] of [["Lateral",lateral,armNav?"NAV":"—"],["Vertical",vertical,armAlt?"ALT":"—"]]) {
            const cell = add(grid,"div",name);
            add(cell,"strong","Active · "+(demo?(active||"—"):"Offline"));
            add(cell,"span","Armed · "+(!demo?"Offline":reportsArmed?armed:"Unavailable"));
        }
        for (const [key,f] of Object.entries(fields)) {
            const card = button(panel,f.name,() => {editing=key; render();},blocked);
            card.className = "ap-target";
            add(card,"strong",(demo?"Selected":reviewed?"Last received":"Selected")+" · "+(reviewed?format(key,target[key]):"—"));
            add(card,"span","Actual · "+(demo?format(key,actual[key]):"—"));
            instrument(card,key);
        }
        const modes = add(panel,"div",undefined,"ap-row");
        for (const mode of ["HDG","NAV","VS","ALT"]) button(modes,mode,() => request(mode,() => {
            if (mode==="NAV") { if (lateral!=="NAV") armNav=!armNav; }
            else if (mode==="HDG") { lateral=mode; armNav=false; }
            else { vertical=mode; if (mode==="ALT") actual.VS=0; arming(); }
        }),blocked||!ap);
        add(panel,"output",notice).setAttribute("aria-live","polite");
    }
    render();
    return cancel;
}
