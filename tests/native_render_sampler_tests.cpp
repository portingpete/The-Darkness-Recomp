#include "app/windows/cpu_sampler.h"
#include <stdexcept>
using namespace DarkRecomp::Native;
static void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
int main() {try {
    {
        NativeCpuSampler disabled(GetCurrentThread(),false,false,false);
        disabled.beginRender();Sleep(20);disabled.endRender();disabled.report();disabled.stop();
        require(disabled.renderSampleCount(RenderSamplePhase::other)==0,"Disabled renderer sampler did work");
    }
    sampleRendererCpu=true;
    NativeCpuSampler sampler(GetCurrentThread(),false,false,true);
    setRenderSamplePhase(RenderSamplePhase::geometry);
    Sleep(40);
    require(sampler.renderSampleCount(RenderSamplePhase::geometry)==0,"Sampler included time outside rendering");
    // A sleeping calling thread also tests safe suspension/resumption while
    // inside a system wait. No other process or worker is sampled by this mode.
    sampler.beginRender();Sleep(140);sampler.endRender();Sleep(20);
    const auto geometry=sampler.renderSampleCount(RenderSamplePhase::geometry);
    require(geometry>0,"Active rendering was never sampled");
    setRenderSamplePhase(RenderSamplePhase::constants);
    sampler.beginRender();Sleep(140);sampler.endRender();Sleep(20);
    const auto constants=sampler.renderSampleCount(RenderSamplePhase::constants);
    require(constants>0,"New rendering phase was never sampled");
    require(sampler.renderSampleCount(RenderSamplePhase::geometry)==geometry,"Samples crossed rendering phases");
    Sleep(40);
    require(sampler.renderSampleCount(RenderSamplePhase::constants)==constants,"Inactive time leaked into rendering samples");
    for(unsigned p=0;p<unsigned(RenderSamplePhase::count);++p)
        if(p!=unsigned(RenderSamplePhase::geometry) && p!=unsigned(RenderSamplePhase::constants))
            require(sampler.renderSampleCount(RenderSamplePhase(p))==0,"Instruction was attributed to an inactive phase");
    sampler.report();Sleep(100);sampler.stop();
    require(sampler.renderSampleCount(RenderSamplePhase::constants)==constants,"Stopping the sampler added inactive samples");
    std::printf("NativeRenderSampler passed: disabled gate, active rendering only, phase attribution, safe resume, asynchronous report and stop; geometry=%llu constants=%llu.\n",geometry,constants);
    return 0;
}catch(const std::exception& error) {std::fprintf(stderr,"%s\n",error.what());return 1;}}
