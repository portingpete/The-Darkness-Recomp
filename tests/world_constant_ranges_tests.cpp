#include "renderer/d3d11/world_constant_ranges.h"
#include "engine_world_template.generated.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include <intrin.h>

using namespace DarkRecomp;
using namespace DarkRecomp::Native;
using Microsoft::WRL::ComPtr;
static void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}

// Independent read oracle: let the actual HLSL preprocessor select template
// branches, then enumerate each emitted c[...] expression (including nested
// refs[]). Palette address reads cover every address bind() accepts, 0..153.
static std::array<bool,256> templateReads(const WorldVertexOptions& options,
    const std::array<std::array<uint32_t,4>,9>& references) {
    std::vector<std::pair<std::string,std::string>> definitions{
        {"MWCOMP",std::to_string(options.weights)},{"POSITION_TRANS",std::to_string(options.positionConversion)},
        {"USE_NORMAL",std::to_string(options.normal)},{"USE_TANGENTS",std::to_string(options.tangents)},
        {"NORMALIZE_NORMAL",std::to_string(options.normalizeNormal)},{"VERTEX_COLOR",std::to_string(options.vertexColor)}};
    for(unsigned stage=0;stage<8;++stage) {
        const auto suffix=std::to_string(stage);
        definitions.insert(definitions.end(),{{"MODE_"+suffix,std::to_string(options.modes[stage])},
            {"COORD_"+suffix,std::to_string(options.coordinates[stage])},
            {"CONVERT_"+suffix,std::to_string(options.conversions[stage])},
            {"MATRIX_"+suffix,std::to_string(options.matrices[stage])}});
    }
    std::vector<D3D_SHADER_MACRO> macros;
    for(const auto& [name,value]:definitions)macros.push_back({name.c_str(),value.c_str()});
    macros.push_back({nullptr,nullptr});
    ComPtr<ID3DBlob> code,errors;
    require(SUCCEEDED(D3DPreprocess(engineWorldTemplateSource,std::strlen(engineWorldTemplateSource),
        "world_constant_range_oracle",macros.data(),nullptr,&code,&errors)),"Actual world template preprocessing failed");
    const std::string source(static_cast<const char*>(code->GetBufferPointer()),code->GetBufferSize());
    static const std::regex referenced(R"((?:A0\.[xyzw]\+)?refs\[([0-8])\]\.([xyz])(?:\+([0-9]+))?)");
    std::array<bool,256> result{};
    for(size_t position=0;(position=source.find("c[",position))!=std::string::npos;) {
        const size_t first=position+2;size_t end=first;unsigned depth=1;
        while(end<source.size() && depth) {if(source[end]=='[')++depth;else if(source[end]==']')--depth;if(depth)++end;}
        require(!depth,"Unclosed constant expression in preprocessed template");
        std::string expression=source.substr(first,end-first);
        expression.erase(std::remove_if(expression.begin(),expression.end(),[](unsigned char c){return std::isspace(c)!=0;}),expression.end());
        position=end+1;
        if(expression=="256")continue; // cbuffer declaration, not a read.
        std::smatch match;unsigned row=0,addresses=1;
        if(std::regex_match(expression,match,referenced)) {
            row=references[unsigned(match[1].str()[0]-'0')][unsigned(match[2].str()[0]-'x')];
            if(match[3].matched)row+=unsigned(std::stoul(match[3].str()));
            if(expression.starts_with("A0."))addresses=154;
        } else if(expression.starts_with("0+"))row=unsigned(std::stoul(expression.substr(2)));
        else throw std::runtime_error("Unknown actual template read: "+expression);
        require(row<256 && addresses<=256-row,"Oracle fixture constant read exceeds bank");
        for(unsigned address=0;address<addresses;++address)result[row+address]=true;
    }
    return result;
}
static WorldVertexConstants fixture() {
    WorldVertexConstants constants;
    constants.references[0]={64,12,10,0};
    for(unsigned stage=0;stage<8;++stage)constants.references[stage+1]={16+stage*2,32+stage*4,180+stage*8,0};
    return constants;
}
static void coverageContract() {
    unsigned permutations=0;
    auto verify=[&](const WorldVertexOptions& options,const WorldVertexConstants& constants) {
        const auto reads=templateReads(options,constants.references);
        const auto ranges=worldConstantRanges(options,constants.references);
        std::array<EngineVector,256> uploaded{},requested{};
        for(unsigned row=0;row<256;++row)if(reads[row]) {
            requested[row][row%4]=std::bit_cast<float>(0x80000000u);
            require(!ranges.equal(uploaded,requested),"Read-range comparison omitted an actual HLSL row or lost signed zero");
            requested[row][row%4]=0;
        }
        require(ranges.rows<=256 && ranges.count<=32 && ranges.equal(uploaded,requested),"Range union is invalid");
        ++permutations;
    };
    const auto constants=fixture();
    WorldVertexOptions options;options.modes.fill(4);
    for(unsigned weights=0;weights<=8;++weights)for(bool position:{false,true})for(bool tangents:{false,true}) {
        options.weights=weights;options.positionConversion=position;options.tangents=tangents;
        options.normal=tangents;options.normalizeNormal=tangents;options.vertexColor=position;
        options.conversions.fill(true);options.matrices.fill(false);verify(options,constants);
    }
    constexpr unsigned modes[]{0,1,4,7,8,9,10,11,13,16,17,18,20,22,23,24};
    for(unsigned stage=0;stage<8;++stage)for(unsigned mode:modes)for(bool conversion:{false,true})for(bool matrix:{false,true}) {
        // Preprocessor oracle covers even unsupported stage combinations;
        // these intentionally select the conservative full-bank plan.
        options={};options.modes.fill(4);options.modes[stage]=uint8_t(mode);
        options.normal=options.tangents=true;options.coordinates.fill(0);
        options.conversions[stage]=conversion;options.matrices[stage]=matrix;verify(options,constants);
    }
    uint32_t random=0x63726F77;
    auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned trial=0;trial<128;++trial) {
        auto varied=constants;options={};options.weights=next()%9;
        options.positionConversion=next()%2;options.normal=next()%2;options.tangents=next()%2;
        options.normalizeNormal=next()%2;options.vertexColor=next()%2;
        varied.references[0]={next()%101,next()%255,next()%256,0};
        for(unsigned stage=0;stage<8;++stage) {
            options.modes[stage]=uint8_t(modes[next()%std::size(modes)]);options.coordinates[stage]=uint8_t(next()%8);
            options.conversions[stage]=next()%2;options.matrices[stage]=next()%2;
            varied.references[stage+1]={next()%255,next()%253,next()%249,0};
        }
        verify(options,varied);
    }
    std::printf("Constant read coverage: %u actual preprocessed HLSL permutations passed.\n",permutations);
}
static void equalityAndBoundaries() {
    auto constants=fixture();WorldVertexOptions options;options.modes.fill(4);
    options.modes[3]=9;
    const auto ranges=worldConstantRanges(options,constants.references);
    require(ranges.rows<32,"Unskinned range plan unexpectedly compared most of the bank");
    auto requested=constants;
    requested.vectors[255][0]=1;
    require(ranges.equal(constants.vectors,requested.vectors),"Unused row changed consumed equality");
    constexpr uint32_t bits[]{0x80000000,1,0x807FFFFF,0x00800000,0x7F7FFFFF,0xFF7FFFFF,0x7FC12345};
    for(auto value:bits) {
        requested=constants;requested.vectors[10][3]=std::bit_cast<float>(value);
        require(!ranges.equal(constants.vectors,requested.vectors),"Consumed raw bits aliased zero");
    }
    for(unsigned first:{0u,100u}) {
        options.weights=8;constants.references[0][0]=first;
        const auto palette=worldConstantRanges(options,constants.references);
        requested=constants;requested.vectors[first+155][3]=1;
        require(!palette.equal(constants.vectors,requested.vectors),"Last legal palette row was omitted");
    }
    auto full=[&](const WorldConstantRanges& plan) {
        require(plan.count==1 && plan.rows==256 && plan.ranges[0].first==0 && plan.ranges[0].count==256,
            "Unknown/overflow state did not retain full-bank equality");
    };
    constants.references[0][0]=101;full(worldConstantRanges(options,constants.references));
    constants=fixture();options={};options.modes.fill(4);constants.references[0][2]=UINT32_MAX;full(worldConstantRanges(options,constants.references));
    constants=fixture();options.positionConversion=true;constants.references[0][1]=255;full(worldConstantRanges(options,constants.references));
    constants=fixture();options={};options.modes.fill(4);options.modes[0]=2;full(worldConstantRanges(options,constants.references));
    options.modes[0]=1;constants.references[1][2]=253;full(worldConstantRanges(options,constants.references));
    options.modes[0]=0;options.matrices[0]=true;constants.references[1][1]=253;full(worldConstantRanges(options,constants.references));
    std::puts("Constant equality: unused rows, raw float bits, palette endpoints, unknown options and overflowing references passed.");
}

__declspec(noinline) static uint64_t compareBanks(const WorldConstantRanges& ranges,
    const std::array<EngineVector,256>& uploaded,const std::array<EngineVector,256>& requested,unsigned repeats) {
    uint64_t sum=0;for(unsigned i=0;i<repeats;++i) {_ReadWriteBarrier();sum+=ranges.equal(uploaded,requested);}return sum;
}
__declspec(noinline) static uint64_t compareFullBanks(const std::array<EngineVector,256>& uploaded,
    const std::array<EngineVector,256>& requested,unsigned repeats) {
    uint64_t sum=0;for(unsigned i=0;i<repeats;++i) {_ReadWriteBarrier();sum+=std::memcmp(uploaded.data(),requested.data(),sizeof(uploaded))==0;}return sum;
}
static void benchmark() {
    for(unsigned weights:{0u,4u,8u}) {
        auto constants=fixture();WorldVertexOptions options;options.modes.fill(4);options.weights=weights;options.modes[3]=9;
        const auto ranges=worldConstantRanges(options,constants.references),full=WorldConstantRanges::full();
        for(const char* workload:{"unchanged","first-row-change","last-read-change","unused-change"}) {
            auto requested=constants;
            if(std::string_view(workload)=="first-row-change")requested.vectors[0][0]=1;
            if(std::string_view(workload)=="last-read-change")requested.vectors[ranges.ranges[ranges.count-1].first+ranges.ranges[ranges.count-1].count-1][3]=1;
            if(std::string_view(workload)=="unused-change")requested.vectors[255][0]=1;
            std::vector<double> before,after;
            auto measure=[&](const auto& plan,auto& times) {
                constexpr unsigned repeats=262144;
                const auto begin=std::chrono::steady_clock::now();
                const auto sum=plan.rows==256?compareFullBanks(constants.vectors,requested.vectors,repeats):
                    compareBanks(plan,constants.vectors,requested.vectors,repeats);
                const auto end=std::chrono::steady_clock::now();
                require(sum==(plan.equal(constants.vectors,requested.vectors)?repeats:0),"Benchmark equality changed");
                times.push_back(std::chrono::duration<double,std::nano>(end-begin).count()/repeats);
            };
            for(unsigned trial=0;trial<9;++trial) {
                if(trial%2){measure(ranges,after);measure(full,before);}else{measure(full,before);measure(ranges,after);}
            }
            std::sort(before.begin(),before.end());std::sort(after.begin(),after.end());
            std::printf("ConstantCompare weights=%u ranges=%u rows=%u workload=%s oldNs=%.3f newNs=%.3f reduction=%.2f%%\n",
                weights,ranges.count,ranges.rows,workload,before[4],after[4],100*(1-after[4]/before[4]));
        }
    }
}
int main(int argc,char** argv) {
    try {coverageContract();equalityAndBoundaries();if(argc>1 && std::string_view(argv[1])=="--benchmark")benchmark();return 0;}
    catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
