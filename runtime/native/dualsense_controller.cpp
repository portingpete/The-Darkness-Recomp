#include "input.h"
#include "dualsense_controller.h"
#include <cstring>

namespace DarkRecomp::Native {
namespace {
struct SonyGamepad {
    uint16_t buttons;uint8_t leftTrigger,rightTrigger;
    int16_t leftX,leftY,rightX,rightY;
};
static_assert(sizeof(SonyGamepad)==12);
extern "C" {
void* dark_dualsense_create();
void dark_dualsense_destroy(void*);
uint32_t dark_dualsense_state(void*,uint32_t,SonyGamepad*);
uint32_t dark_dualsense_rumble(void*,uint32_t,uint16_t,uint16_t);
}
// Only the native input worker reaches these callbacks. Rust HID handles and
// platform enumeration are never used by a guest poll or renderer thread.
struct SonyProvider {
    void* context=nullptr;
    ULONGLONG retry=0;
    std::array<SonyGamepad,4> previous{};
    std::array<DWORD,4> packet{},status{};
    SonyProvider() {status.fill(ERROR_DEVICE_NOT_CONNECTED);}
    ~SonyProvider() {if(context)dark_dualsense_destroy(context);}
    void prepare() {
        if(!context && GetTickCount64()>=retry) {
            context=dark_dualsense_create();retry=GetTickCount64()+1000;
        }
    }
};
SonyProvider& sony() {thread_local SonyProvider provider;return provider;}
DWORD WINAPI controllerState(DWORD host,XINPUT_STATE* output) noexcept {
    if(host>=4)return host<8?XInputGetState(host-4,output):ERROR_DEVICE_NOT_CONNECTED;
    auto& p=sony();p.prepare();SonyGamepad pad{};
    const DWORD result=dark_dualsense_state(p.context,host,&pad);
    *output={};
    if(result==ERROR_SUCCESS) {
        if(p.status[host]!=ERROR_SUCCESS || std::memcmp(&pad,&p.previous[host],sizeof(pad)))++p.packet[host];
        p.previous[host]=pad;output->dwPacketNumber=p.packet[host];
        output->Gamepad={pad.buttons,pad.leftTrigger,pad.rightTrigger,pad.leftX,pad.leftY,pad.rightX,pad.rightY};
    }
    p.status[host]=result;return result;
}
DWORD WINAPI controllerCapabilities(DWORD host,DWORD flags,XINPUT_CAPABILITIES* output) noexcept {
    if(host>=4)return host<8?XInputGetCapabilities(host-4,flags,output):ERROR_DEVICE_NOT_CONNECTED;
    if(sony().status[host]!=ERROR_SUCCESS)return ERROR_DEVICE_NOT_CONNECTED;
    *output={XINPUT_DEVTYPE_GAMEPAD,XINPUT_DEVSUBTYPE_GAMEPAD,XINPUT_CAPS_FFB_SUPPORTED,
        {0xf3ff,255,255,-1,-1,-1,-1},{65535,65535}};
    return ERROR_SUCCESS;
}
DWORD WINAPI controllerVibration(DWORD host,XINPUT_VIBRATION* value) noexcept {
    if(host>=4)return host<8?XInputSetState(host-4,value):ERROR_DEVICE_NOT_CONNECTED;
    return dark_dualsense_rumble(sony().context,host,value->wLeftMotorSpeed,value->wRightMotorSpeed);
}
bool playStationSlot(uint32_t host) {return host<4;}
}
ControllerApi productionControllerApi() {
    ControllerApi api{controllerState,controllerCapabilities,controllerVibration,&GetTickCount64};
    api.hostCount=8;api.playStation=playStationSlot;return api;
}
}
