#pragma once
namespace {
std::array<DWORD,8> sonyStatus{};
std::array<XINPUT_STATE,8> sonyStates{};
DWORD sonyMotorHost=99;
XINPUT_VIBRATION sonyMotors{};
DWORD WINAPI sonyFixtureState(DWORD host,XINPUT_STATE* output) noexcept {
    if(host>=8)return ERROR_INVALID_PARAMETER;*output=sonyStates[host];return sonyStatus[host];
}
DWORD WINAPI sonyFixtureCaps(DWORD host,DWORD,XINPUT_CAPABILITIES* output) noexcept {
    if(host>=8)return ERROR_INVALID_PARAMETER;
    *output={XINPUT_DEVTYPE_GAMEPAD,XINPUT_DEVSUBTYPE_GAMEPAD,XINPUT_CAPS_FFB_SUPPORTED,
        {0xf3ff,255,255,-1,-1,-1,-1},{65535,65535}};return sonyStatus[host];
}
DWORD WINAPI sonyFixtureMotors(DWORD host,XINPUT_VIBRATION* motors) noexcept {
    if(host>=8)return ERROR_INVALID_PARAMETER;sonyMotorHost=host;sonyMotors=*motors;return sonyStatus[host];
}
bool sonyFixtureKind(uint32_t host) {return host<4;}
}
static void testDualSenseInputRouting() {
    const uint32_t region=memory->allocate(4096),output=region+1,rumble=region+64;
    check(region!=0,"DualSense routing allocation failed");
    const auto read16=[&](uint32_t address) {const auto* p=memory->base()+address;return uint16_t(uint16_t(p[0])<<8|p[1]);};
    ControllerApi api{sonyFixtureState,sonyFixtureCaps,sonyFixtureMotors,&GetTickCount64};
    api.hostCount=8;api.playStation=sonyFixtureKind;
    for(unsigned host=0;host<8;++host) {
        sonyStatus.fill(ERROR_DEVICE_NOT_CONNECTED);sonyStates={};sonyStatus[host]=ERROR_SUCCESS;
        sonyStates[host].Gamepad={XINPUT_GAMEPAD_A,90,180,-12000,20000,-25000,32767};
        NativeInput input(api);
        check(input.getState(*memory,0,0,output)==ERROR_SUCCESS,"Extended host slot did not drive guest zero");
        check(read16(output+4)==XINPUT_GAMEPAD_A && read16(output+8)==uint16_t(-12000) &&
              read16(output+14)==32767,"Sony routing changed guest state endianness/layout");
        check(input.promptSource()==(host<4?PromptInputSource::PlayStation:PromptInputSource::Controller),
              "Host family did not select the matching prompt source");
        check(input.getState(*memory,1,0,output+32)==ERROR_DEVICE_NOT_CONNECTED,"Extra host became a signed-in guest user");
        check(input.getCapabilities(*memory,0,0,output+64)==ERROR_SUCCESS,"Routed gamepad capabilities failed");
        memory->write32(rumble,(12000u<<16)|24000u);
        check(input.setState(*memory,0,0,rumble)==ERROR_SUCCESS && sonyMotorHost==host &&
              sonyMotors.wLeftMotorSpeed==12000 && sonyMotors.wRightMotorSpeed==24000,"Rumble went to another host device");
        input.shutdownControllers();
        check(!sonyMotors.wLeftMotorSpeed && !sonyMotors.wRightMotorSpeed,"Explicit shutdown left motors active");
    }
    sonyStatus.fill(ERROR_DEVICE_NOT_CONNECTED);sonyStates={};
    sonyStatus[0]=sonyStatus[4]=ERROR_SUCCESS;sonyStates[0].Gamepad.wButtons=XINPUT_GAMEPAD_A;
    sonyStates[4].Gamepad.wButtons=XINPUT_GAMEPAD_B;
    {
        NativeInput input(api);
        check(input.getState(*memory,0,0,output)==ERROR_SUCCESS && input.promptSource()==PromptInputSource::PlayStation,
              "Sony primary selection failed");
        sonyStatus[0]=ERROR_DEVICE_NOT_CONNECTED;
        check(input.getState(*memory,0,0,output)==ERROR_SUCCESS && read16(output+4)==XINPUT_GAMEPAD_B &&
              input.promptSource()==PromptInputSource::Controller,"Sony unplug did not fall back to XInput");
        sonyStatus[0]=ERROR_SUCCESS;
        check(input.getState(*memory,0,0,output)==ERROR_SUCCESS && read16(output+4)==XINPUT_GAMEPAD_B,
              "Sony hotplug stole an existing primary controller");
        sonyStatus[4]=ERROR_DEVICE_NOT_CONNECTED;
        check(input.getState(*memory,0,0,output)==ERROR_SUCCESS && input.promptSource()==PromptInputSource::PlayStation,
              "XInput unplug did not return to Sony input");
    }
    memory->release(region);
    puts("DualSense routing: eight host slots, one guest, family prompts, hotplug and rumble shutdown passed.");
}
