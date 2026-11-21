// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "pack_scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>

static pack_scene_t scene;
static char data[65536];
static kvm_json_tok_t tokens[8192];

static bool load(const char *folder)
{
    const char *const names[]={"manifest.json","ui.json","bindings.json"};
    for (unsigned i=0;i<3;i++) {
        char path[512];
        if (snprintf(path,sizeof(path),"%s/%s",folder,names[i]) >= (int)sizeof(path)) return false;
        FILE *fp=fopen(path,"rb");
        if (!fp) return false;
        size_t n=fread(data,1,sizeof(data),fp);
        fclose(fp);
        kvm_json_doc_t d;
        if (n>=sizeof(data) || kvm_json_parse_local(&d,data,n,tokens,8192,24)!=KVM_JSON_OK) return false;
        bool ok=i==0 ? pack_scene_manifest(&scene,&d) : i==1 ? pack_scene_ui(&scene,&d) : pack_scene_bindings(&scene,&d);
        if (!ok) return false;
    }
    return true;
}

int main(int argc,char **argv)
{
    if (argc==7 && !strcmp(argv[1],"magnifier")) {
        if (!load(argv[2])) return 1;
        const pack_action_t *a=pack_scene_action(&scene,6,PACK_TAP);
        const unsigned ids[]={0,3,4,2}; // initial, then zoom/native/overview taps in the example
        for (unsigned i=0;i<4;i++) {
            if (i && !pack_scene_tap(&scene,ids[i])) return 1;
            const char *mode=pack_scene_capture_mode(&scene,a);
            if (strcmp(mode ? mode:"invalid",argv[3+i])) return 1;
        }
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"accept")) return load(argv[2]) ? 0 : 1;
    if (argc==3 && !strcmp(argv[1],"reject")) return load(argv[2]) ? 1 : 0;
    if (argc==3 && !strcmp(argv[1],"eval")) {
        if (!load(argv[2]) || !scene.bindings) return 1;
        pack_binding_t *b=&scene.binding[0];
        uint32_t tick;
        char number[80], text[32];
        while (scanf("%" SCNu32 " %79s",&tick,number)==2) {
            pack_value_t input={.real=strtod(number,NULL)}, output;
            bool missing=!strcmp(number,"Stale") || !strcmp(number,"Unavailable");
            bool ok=pack_binding_apply(&scene,b,missing ? NULL : &input,tick,&output);
            puts(ok ? pack_value_text(b->output,&output,text) : missing ? number : "Unavailable");
        }
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"show-state")) {
        if (!load(argv[2]) || scene.states!=1 || scene.actions!=2 || !scene.binding[0].local) return 1;
        char text[32];
        puts(pack_state_text(&scene.state[0],text));
        for (unsigned i=0; i<scene.actions; i++) {
            if (!pack_scene_tap(&scene,scene.action[i].widget)) return 1;
            puts(pack_state_text(&scene.state[0],text));
        }
        return 0;
    }
    if (argc!=5) return 2;
    assert(load(argv[1]) && scene.widgets==7 && scene.actions==7 && scene.permissions==(PACK_PERM_UI|PACK_PERM_JOYSTICK));
    const pack_action_t *a=pack_scene_action(&scene,1,PACK_CHANGE);
    assert(a && a->kind==PACK_AXES && a->mask==(PACK_X|PACK_Y));
    assert(pack_scene_action(&scene,2,PACK_CHANGE)->mask==PACK_Z);
    assert(pack_scene_action(&scene,3,PACK_CHANGE)->mask==PACK_RZ);
    assert(pack_scene_action(&scene,4,PACK_PRESS)->down && !pack_scene_action(&scene,4,PACK_RELEASE)->down);
    assert(!pack_scene_tap(&scene,4));
    assert(load(argv[2]) && scene.sources==7 && scene.bindings==7 && scene.required_permissions==PACK_PERM_TELEMETRY);
    assert(scene.source[0].channel==0 && scene.source[0].max_age_ms==1000);
    assert(scene.source[6].channel==5 && scene.binding[5].field==PACK_BIND_ROLL);
    assert(load(argv[3]) && scene.actions==5 && scene.required_permissions==PACK_PERM_CAPTURE);
    assert(pack_scene_action(&scene,6,PACK_TAP)->kind==PACK_START_STREAM);
    assert(pack_scene_tap(&scene,3) && !strcmp(scene.state[0].value.text,"ZOOM_2X"));
    assert(load(argv[4]) && scene.actions==2 && scene.permissions==PACK_PERM_UI);
    assert(pack_scene_tap(&scene,scene.action[0].widget));
    puts("PASS four example scenes, permissions, action targets, axes, releases, telemetry topics and capture mode");
    return 0;
}
