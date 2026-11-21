// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SimConnect.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstddef>
#include <fcntl.h>
#include <io.h>

// The SDK supplies the structure packing, dispatch IDs, exports and event constants.
struct Variable { const char* name; const char* unit; };
constexpr Variable variables[] = {
    {"AIRSPEED INDICATED", "knots"}, {"INDICATED ALTITUDE", "feet"},
    {"PLANE HEADING DEGREES MAGNETIC", "degrees"}, {"VERTICAL SPEED", "feet per minute"},
    {"PLANE PITCH DEGREES", "degrees"}, {"PLANE BANK DEGREES", "degrees"}
};

// A complete but erroneous response invalidates the requested values immediately.
static bool provider_error(unsigned count) {
    double values[6];
    for (unsigned i=0; i<count; ++i) values[i]=std::numeric_limits<double>::quiet_NaN();
    fwrite(values,sizeof(double),count,stdout);
    fflush(stdout);
    return false; // reopen the read helper and rebuild this subscription
}

static bool sample(HANDLE sim, DWORD request,const unsigned channels[6],unsigned count) {
    if (FAILED(SimConnect_RequestDataOnSimObject(sim, request, 1, SIMCONNECT_OBJECT_ID_USER,
                                                 SIMCONNECT_PERIOD_ONCE))) return false;
    auto deadline = GetTickCount64() + 1000;
    while (GetTickCount64() < deadline) {
        SIMCONNECT_RECV* packet = nullptr;
        DWORD bytes = 0;
        if (SUCCEEDED(SimConnect_GetNextDispatch(sim, &packet, &bytes)) && packet &&
            bytes >= sizeof(SIMCONNECT_RECV)) {
            if (packet->dwID == SIMCONNECT_RECV_ID_QUIT) return false;
            if (packet->dwID == SIMCONNECT_RECV_ID_EXCEPTION) return provider_error(count);
            if (packet->dwID != SIMCONNECT_RECV_ID_SIMOBJECT_DATA) continue;
            auto data = reinterpret_cast<SIMCONNECT_RECV_SIMOBJECT_DATA*>(packet);
            if (bytes < offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA, dwData)) return provider_error(count);
            if (data->dwRequestID != request) continue;
            if (bytes < offsetof(SIMCONNECT_RECV_SIMOBJECT_DATA, dwData) + count * sizeof(double) ||
                data->dwDefineID != 1 || data->dwDefineCount != count) return provider_error(count);
            double values[6]{};
            std::memcpy(values, &data->dwData, count*sizeof(double));
            for (unsigned i=0;i<count;i++) {
                // Positive pitch is nose up, positive roll is right wing down.
                if (channels[i]>=4) values[i]=-values[i];
            }
            return fwrite(values,sizeof(double),count,stdout)==count && fflush(stdout)==0;
        }
        Sleep(2);
    }
    return false;
}

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    HANDLE sim{};
    if (FAILED(SimConnect_Open(&sim, "KVMpad HOST", nullptr, 0, nullptr, 0))) return 3;
    int status = 0;
    DWORD request = 0;
    unsigned channels[6]{}, count=0, mask=63;
    bool selected=false;
    while (status == 0) {
        int command = fgetc(stdin);
        if (command == EOF) break;
        if (command=='S' && !selected && !count) {
            int requested=fgetc(stdin);
            if (requested<1 || requested>63) { status=2; break; }
            mask=static_cast<unsigned>(requested);
            selected=true;
            if (fputc('S',stdout)==EOF || fputc(requested,stdout)==EOF || fflush(stdout)) status=3;
        } else if (command == 'Q') {
            if (!count) {
                for (unsigned i=0;i<6;i++) {
                    if (!(mask & (1u<<i))) continue;
                    const auto& var=variables[i];
                    if (FAILED(SimConnect_AddToDataDefinition(sim,1,var.name,var.unit,SIMCONNECT_DATATYPE_FLOAT64))) {
                        status=3;
                        break;
                    }
                    channels[count++]=i;
                }
            }
            if (!status && !sample(sim,++request,channels,count)) status=3;
        } else if (!selected && !count && (command == '+' || command == '-')) {
            const char* event = command == '+' ? "FLAPS_INCR" : "FLAPS_DECR";
            if (FAILED(SimConnect_MapClientEventToSimEvent(sim, 1, event)) ||
                FAILED(SimConnect_TransmitClientEvent(sim, SIMCONNECT_OBJECT_ID_USER, 1, 0,
                       SIMCONNECT_GROUP_PRIORITY_HIGHEST, SIMCONNECT_EVENT_FLAG_GROUPID_IS_PRIORITY)))
                status = 3;
            else if (fputc('A', stdout) == EOF || fflush(stdout)) status = 3;
            break;
        } else status = 2;
    }
    SimConnect_Close(sim);
    return status;
}
