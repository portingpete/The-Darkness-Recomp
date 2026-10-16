#include "subtitle_font.h"
#include "runtime/native/native_menu_text.h"
#include <Windows.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

namespace DarkRecomp::Native {
std::optional<uint32_t> westernSubtitleCodepoint(uint16_t slot) noexcept {
    if (slot < 0x80 || (slot >= 0xa0 && slot <= 0xff)) return slot;
    constexpr uint16_t windows1252[]{
        0x20ac,0,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0,0x017d,0,
        0,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x02dc,0x2122,0x0161,0x203a,0x0153,0,0x017e,0x0178
    };
    if (slot >= 0x80 && slot <= 0x9f) {
        const auto codepoint = windows1252[slot - 0x80];
        return codepoint ? std::optional<uint32_t>(codepoint) : std::nullopt;
    }
    for (const auto codepoint : windows1252)
        if (codepoint && codepoint == slot) return slot;
    return {};
}
std::optional<uint32_t> russianSubtitleCodepoint(uint16_t slot) noexcept {
    if (slot < 0x80) return slot;
    if (slot == 0xa8) return 0x0401;
    if (slot == 0xb8) return 0x0451;
    if (slot >= 0xc0 && slot <= 0xff) return 0x0410 + slot - 0xc0;
    if (slot >= 0x100) return westernSubtitleCodepoint(slot);
    return {};
}
namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
struct Cell {
    uint32_t codepoint; unsigned left, top, right, bottom;
    bool operator==(const Cell&) const = default;
};
struct Definition {
    uint64_t generation = 0;
    uint32_t textureId = 0;
    uint32_t font = 0;
    unsigned width = 0, height = 0;
    bool russian = false;
    std::vector<Cell> cells;
    std::shared_ptr<const AlphaImage> alpha;
    std::shared_ptr<const ColorImage> color;
};
struct Registration {
    std::shared_ptr<const Definition> definition;
    std::vector<std::weak_ptr<const ColorImage>> sources;
};
struct Cached {
    std::shared_ptr<const Definition> definition;
    std::shared_ptr<const ColorImage> source, replacement;
    unsigned factor = 0;
    uint64_t used = 0;
};
struct AlphaSource {
    std::shared_ptr<const AlphaImage> alpha;
    std::shared_ptr<const ColorImage> color;
    uint64_t used = 0;
};
struct FailedRaster {
    std::weak_ptr<const Definition> definition;
    std::weak_ptr<const ColorImage> source;
    unsigned factor = 0;
};
std::mutex registryMutex;
std::vector<Registration> registrations;
std::vector<Cached> cache;
std::vector<AlphaSource> alphaSources;
std::array<FailedRaster,64> failedRasters{};
unsigned failedRasterNext = 0;
bool forceRasterizerUnavailableForTest = false;
uint64_t rasterizerAttempts = 0;
uint64_t generation = 0, use = 0;
size_t cacheBytes = 0;
constexpr size_t cacheBudget = 64 * 1024 * 1024;
constexpr uint64_t fnvOffset = 0xcbf29ce484222325ull, fnvPrime = 0x100000001b3ull;
// Approved Russian Text.xfc SHA256 318b5936b9f51c26310becb383015a53f7e51b9ff165924c5aeabb10dae538fd.
// Match the ordered slot/UV cells AND the original A8 coverage before interpreting CP1251.
constexpr uint64_t russianMetadata = 0xe92ff58209a1b22cull;
constexpr uint64_t russianAlpha = 0x61546e11bd70369dull;
bool approvedRussianMetadata(const Definition& definition) {
    if (definition.width != 512 || definition.height != 256 || definition.cells.size() != 214) return false;
    uint64_t hash = fnvOffset;
    for (const auto& cell : definition.cells)
        for (const auto value : {cell.codepoint, cell.left, cell.top, cell.right, cell.bottom})
            for (unsigned shift = 0; shift < 32; shift += 8)
                hash = (hash ^ uint8_t(value >> shift)) * fnvPrime;
    return hash == russianMetadata;
}
bool diagnostics() {
    static const bool enabled=GetEnvironmentVariableA("DARK_TRACE_SUBTITLE_FONT",nullptr,0)!=0;
    return enabled;
}
void traceReplacement(const char* reason,uint32_t id,const std::shared_ptr<const ColorImage>& source,
    unsigned factor,const Definition* definition=nullptr) {
    static std::atomic<unsigned> lines{0};
    if(!diagnostics() || lines++>=32)return;
    std::fprintf(stderr,"[SubtitleFontTrace] replacement reason=%s id=%u factor=%u source=%p size=%ux%u codec=%u definitions=%zu registeredAlpha=%p registeredColor=%p currentAlpha=%p currentColor=%p\n",
        reason,id,factor,source.get(),source?source->width:0,source?source->height:0,source?source->sourceCodec:0,
        registrations.size(),definition?definition->alpha.get():nullptr,definition?definition->color.get():nullptr,
        previewAlphaImageForSubtitleFont(id).get(),previewColorImageForSubtitleFont(id).get());
}

uint32_t be32(const uint8_t* p) { uint32_t value; std::memcpy(&value,p,4); return _byteswap_ulong(value); }
uint16_t be16(const uint8_t* p) { return uint16_t(unsigned(p[0]) * 256 + p[1]); }
float number(const uint8_t* p) { return std::bit_cast<float>(be32(p)); }
bool copy(uint8_t* base, uint64_t address, void* output, size_t size) {
    return address && address < 0x100000000ull && size <= 0x100000000ull - address &&
        copyRenderMemory(base,address,output,size);
}
bool word(uint8_t* base, uint64_t address, uint32_t& output) {
    uint8_t bytes[4]; if(!copy(base,address,bytes,4))return false;
    output=be32(bytes);return true;
}
bool coordinate(float value, unsigned extent, unsigned& output) {
    if(!std::isfinite(value) || value<0 || value>1)return false;
    const double pixels=double(value)*extent, rounded=std::round(pixels);
    if(std::abs(pixels-rounded)>0.02)return false;
    output=unsigned(rounded);return output<=extent;
}
std::shared_ptr<Definition> snapshot(uint8_t* base,uint32_t font,const char*& reason) {
    auto reject=[&](const char* why)->std::shared_ptr<Definition>{reason=why;return {};};
    std::array<uint8_t,92> header{};
    if(!copy(base,font,header.data(),header.size()) || be32(header.data())!=0x82073268)return reject("font-header");
    const auto vector=be32(header.data()+12), list=be32(header.data()+84), id=be32(header.data()+88);
    uint32_t count=0, records=0, imageVector=0, pages=0;
    if(!id || id>65535 || !vector || !list || !word(base,uint64_t(vector)+4,count) || !count || count>2048 ||
       !word(base,uint64_t(vector)+24,records) || !records ||
       !word(base,uint64_t(list)+56,imageVector) || !imageVector || !word(base,uint64_t(imageVector)+4,pages) || pages!=1)return reject("font-vector/page/id");
    const float inverseWidth=number(header.data()+64),inverseHeight=number(header.data()+68);
    if(!std::isfinite(inverseWidth) || !std::isfinite(inverseHeight) || inverseWidth<=0 || inverseHeight<=0)return reject("inverse-dimensions");
    const double width=1.0/double(inverseWidth),height=1.0/double(inverseHeight);
    if(width<1 || height<1 || width>512.02 || height>256.02 ||
       std::abs(width-std::round(width))>0.02 || std::abs(height-std::round(height))>0.02)return reject("atlas-dimensions");
    auto result=std::make_shared<Definition>();result->textureId=id;result->font=font;
    result->russian=russianNativeMenus();
    result->width=unsigned(std::round(width));result->height=unsigned(std::round(height));
    std::vector<uint8_t> bytes(size_t(count)*40);
    if(!copy(base,records,bytes.data(),bytes.size()))return reject("glyph-records");
    for(unsigned index=0;index<count;++index) {
        const auto* r=bytes.data()+size_t(index)*40;
        if(be16(r+28)>=0x8000)continue;
        Cell c{be16(r+32),0,0,0,0};
        if(!coordinate(number(r),result->width,c.left) || !coordinate(number(r+4),result->height,c.top) ||
           !coordinate(number(r+8),result->width,c.right) || !coordinate(number(r+12),result->height,c.bottom) ||
           c.left>c.right || c.top>c.bottom)return reject("glyph-uv");
        if(c.left==c.right || c.top==c.bottom)continue;
        const float cellWidth=number(r+16),cellHeight=number(r+20);
        if(!std::isfinite(cellWidth) || !std::isfinite(cellHeight) ||
           std::abs(cellWidth-float(c.right-c.left))>0.02f || std::abs(cellHeight-float(c.bottom-c.top))>0.02f)return reject("glyph-span");
        bool duplicate=false;
        for(const auto& previous:result->cells) {
            const bool overlap=c.left<previous.right && c.right>previous.left && c.top<previous.bottom && c.bottom>previous.top;
            if(!overlap)continue;
            if(c.left==previous.left && c.right==previous.right && c.top==previous.top && c.bottom==previous.bottom &&
               c.codepoint==previous.codepoint) {duplicate=true;break;}
            return reject("glyph-overlap"); // Never overwrite another character's shared atlas cell.
        }
        if(!duplicate)result->cells.push_back(c);
    }
    if(result->cells.empty())return reject("empty-glyphs");
    reason="metadata-valid";return result;
}
struct Rasterizer {
    Ptr<IDWriteFactory> factory;
    Ptr<IDWriteFontFace> face, fallback;
    Ptr<IDWriteRenderingParams> params;
    Ptr<IDWriteBitmapRenderTarget> target;
    Ptr<IDWriteBitmapRenderTarget1> grayscale;
    DIBSECTION bitmap{};
    bool family(IDWriteFontCollection* collection,const wchar_t* name,Ptr<IDWriteFontFace>& output) {
        UINT32 index=0;BOOL exists=FALSE;
        if(FAILED(collection->FindFamilyName(name,&index,&exists)) || !exists)return false;
        Ptr<IDWriteFontFamily> family;Ptr<IDWriteFont> font;
        return SUCCEEDED(collection->GetFontFamily(index,&family)) &&
            SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STRETCH_NORMAL,DWRITE_FONT_STYLE_NORMAL,&font)) &&
            SUCCEEDED(font->CreateFontFace(&output));
    }
    Rasterizer() {
        ++rasterizerAttempts;
        if(forceRasterizerUnavailableForTest)return;
        if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))return;
        Ptr<IDWriteFontCollection> collection;Ptr<IDWriteGdiInterop> gdi;
        if(FAILED(factory->GetSystemFontCollection(&collection)) || !family(collection.Get(),L"Arial",face) ||
           FAILED(factory->CreateCustomRenderingParams(1.0f,0.0f,0.0f,DWRITE_PIXEL_GEOMETRY_FLAT,DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,&params)) ||
           FAILED(factory->GetGdiInterop(&gdi)) || FAILED(gdi->CreateBitmapRenderTarget(nullptr,512,512,&target)) ||
           FAILED(target.As(&grayscale)) || FAILED(grayscale->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE))) {target.Reset();return;}
        family(collection.Get(),L"Segoe UI",fallback);
        target->SetPixelsPerDip(1.0f);
        const auto dc=target->GetMemoryDC();
        if(GetObjectW(GetCurrentObject(dc,OBJ_BITMAP),sizeof(bitmap),&bitmap)!=sizeof(bitmap) || !bitmap.dsBm.bmBits ||
           bitmap.dsBm.bmWidth!=512 || bitmap.dsBm.bmHeight!=512 || bitmap.dsBm.bmBitsPixel!=32)target.Reset();
    }
    bool glyph(uint32_t codepoint,unsigned width,unsigned height,std::vector<uint8_t>& output) {
        if(!target || !width || !height || width>480 || height>480 || codepoint>0x10ffff)return false;
        UINT16 glyph=0;auto* selected=face.Get();
        if(FAILED(selected->GetGlyphIndices(&codepoint,1,&glyph)))return false;
        if(!glyph && fallback) {selected=fallback.Get();if(FAILED(selected->GetGlyphIndices(&codepoint,1,&glyph)))return false;}
        if(!glyph)return false;
        const FLOAT advance=0;const DWRITE_GLYPH_OFFSET offset{};
        DWRITE_GLYPH_RUN run{selected,128,1,&glyph,&advance,&offset,FALSE,0};
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        auto clear=[&] {std::memset(bitmap.dsBm.bmBits,0,size_t(bitmap.dsBm.bmWidthBytes)*512);};
        clear();if(FAILED(target->SetCurrentTransform(&identity)) ||
           FAILED(target->DrawGlyphRun(128,256,DWRITE_MEASURING_MODE_NATURAL,&run,params.Get(),RGB(255,255,255))))return false;
        unsigned left=512,top=512,right=0,bottom=0;
        auto* pixels=static_cast<const uint8_t*>(bitmap.dsBm.bmBits);
        for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x)if(pixels[size_t(y)*bitmap.dsBm.bmWidthBytes+x*4]) {
            left=(std::min)(left,x);top=(std::min)(top,y);right=(std::max)(right,x+1);bottom=(std::max)(bottom,y+1);
        }
        if(left>=right || top>=bottom || !left || !top || right==512 || bottom==512)return false;
        const float sx=float(width)/float(right-left),sy=float(height)/float(bottom-top);
        const DWRITE_MATRIX transform{sx,0,0,sy,4-float(left)*sx,4-float(top)*sy};
        clear();if(FAILED(target->SetCurrentTransform(&transform)) ||
           FAILED(target->DrawGlyphRun(128,256,DWRITE_MEASURING_MODE_NATURAL,&run,params.Get(),RGB(255,255,255))))return false;
        output.resize(size_t(width)*height);
        bool visible=false;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            const auto coverage=pixels[size_t(y+4)*bitmap.dsBm.bmWidthBytes+(x+4)*4];
            output[size_t(y)*width+x]=coverage;visible|=coverage!=0;
        }
        return visible;
    }
};
uint8_t scaledAlpha(const ColorImage& source,unsigned x,unsigned y,unsigned factor) {
    const double sx=(double(x)+0.5)/factor-0.5,sy=(double(y)+0.5)/factor-0.5;
    const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const double fx=sx-ix,fy=sy-iy;
    auto at=[&](int px,int py) {return double(source.pixels[(size_t(std::clamp(py,0,int(source.height)-1))*source.width+
        unsigned(std::clamp(px,0,int(source.width)-1)))*4+3]);};
    return uint8_t(std::clamp(std::lround((at(ix,iy)*(1-fx)+at(ix+1,iy)*fx)*(1-fy)+
        (at(ix,iy+1)*(1-fx)+at(ix+1,iy+1)*fx)*fy),0l,255l));
}
std::shared_ptr<const ColorImage> rasterize(const Definition& definition,const ColorImage& source,unsigned factor) {
    Rasterizer rasterizer;
    if(!rasterizer.target)return {};
    auto result=std::make_shared<ColorImage>();result->width=source.width*factor;result->height=source.height*factor;
    result->generatedSubtitleFont=true;result->authoredMips=true;result->sourceCodec=2;
    result->pixels.resize(size_t(result->width)*result->height*4,255);
    for(unsigned y=0;y<result->height;++y)for(unsigned x=0;x<result->width;++x)
        result->pixels[(size_t(y)*result->width+x)*4+3]=scaledAlpha(source,x,y,factor);
    unsigned rendered=0,kept=0;
    std::vector<uint8_t> coverage;
    for(const auto& cell:definition.cells) {
        const auto codepoint=definition.russian ? russianSubtitleCodepoint(uint16_t(cell.codepoint)) :
            westernSubtitleCodepoint(uint16_t(cell.codepoint));
        if(!codepoint) {++kept;continue;}
        unsigned left=cell.right,top=cell.bottom,right=cell.left,bottom=cell.top;
        for(unsigned y=cell.top;y<cell.bottom;++y)for(unsigned x=cell.left;x<cell.right;++x)
            if(source.pixels[(size_t(y)*source.width+x)*4+3]) {
                left=(std::min)(left,x);top=(std::min)(top,y);right=(std::max)(right,x+1);bottom=(std::max)(bottom,y+1);
            }
        if(left>=right || top>=bottom) {
            for(unsigned y=cell.top*factor;y<cell.bottom*factor;++y)for(unsigned x=cell.left*factor;x<cell.right*factor;++x)
                result->pixels[(size_t(y)*result->width+x)*4+3]=0;
            continue;
        }
        if(!rasterizer.glyph(*codepoint,(right-left)*factor,(bottom-top)*factor,coverage)) {++kept;continue;}
        ++rendered;
        for(unsigned y=cell.top*factor;y<cell.bottom*factor;++y)for(unsigned x=cell.left*factor;x<cell.right*factor;++x)
            result->pixels[(size_t(y)*result->width+x)*4+3]=0;
        for(unsigned y=0;y<(bottom-top)*factor;++y)for(unsigned x=0;x<(right-left)*factor;++x)
            result->pixels[(size_t(top*factor+y)*result->width+left*factor+x)*4+3]=coverage[size_t(y)*(right-left)*factor+x];
    }
    if(!rendered || !result->valid())return {};
    std::fprintf(stderr,"[SubtitleFont] vector atlas id=%u factor=%u size=%ux%u glyphs=%u fallback=%u\n",
        definition.textureId,factor,result->width,result->height,rendered,kept);
    return result;
}
std::shared_ptr<const ColorImage> replacementLocked(uint32_t textureId,const std::shared_ptr<const ColorImage>& source,unsigned factor) {
    if(!source || !source->valid() || source->generatedSubtitleFont || source->faces!=1 || source->firstMip ||
       (factor!=2 && factor!=4 && factor!=8)) {traceReplacement("source/factor",textureId,source,factor);return {};}
    auto registration=std::find_if(registrations.begin(),registrations.end(),[&](const auto& r){return r.definition->textureId==textureId;});
    if(registration==registrations.end()) {traceReplacement("unregistered",textureId,source,factor);return {};}
    const auto definition=registration->definition;
    if(definition->width!=source->width || definition->height!=source->height) {traceReplacement("size",textureId,source,factor,definition.get());return {};}
    const bool modern=definition->color && source==definition->color &&
        previewColorImageForSubtitleFont(textureId)==definition->color;
    const bool legacy=definition->alpha && previewAlphaImageForSubtitleFont(textureId)==definition->alpha;
    if(!modern && !legacy) {traceReplacement("generation",textureId,source,factor,definition.get());return {};}
    const bool verified=std::any_of(registration->sources.begin(),registration->sources.end(),
        [&](const auto& original){return original.lock()==source;});
    if(!verified) {
        if(!modern && definition->alpha->pixels.size()!=source->pixels.size()/4) {traceReplacement("alpha-size",textureId,source,factor,definition.get());return {};}
        uint64_t alphaHash=fnvOffset;
        for(size_t i=0;i<source->pixels.size()/4;++i) {
            const auto* pixel=source->pixels.data()+i*4;
            if(pixel[0]!=255 || pixel[1]!=255 || pixel[2]!=255 || (!modern && pixel[3]!=definition->alpha->pixels[i])) {
                traceReplacement("rgb/alpha",textureId,source,factor,definition.get());return {};
            }
            if(definition->russian)alphaHash=(alphaHash^pixel[3])*fnvPrime;
        }
        if(definition->russian && alphaHash!=russianAlpha) {traceReplacement("russian-alpha",textureId,source,factor,definition.get());return {};}
        std::erase_if(registration->sources,[](const auto& original){return original.expired();});
        if(registration->sources.size()>=16)return {};
        registration->sources.push_back(source);
    }
    for(auto& entry:cache)if(entry.source==source && entry.definition==definition && entry.factor==factor) {
        entry.used=++use;return entry.replacement;
    }
    if(std::any_of(failedRasters.begin(),failedRasters.end(),[&](const auto& failed){
        return failed.factor==factor && failed.definition.lock()==definition && failed.source.lock()==source;
    }))return {};
    try {
        const auto result=rasterize(*definition,*source,factor);
        if(result) {
            const auto bytes=result->bytes();
            while(!cache.empty() && cacheBytes+bytes>cacheBudget) {
                const auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.used<b.used;});
                cacheBytes-=oldest->replacement->bytes();cache.erase(oldest);
            }
            if(bytes<=cacheBudget) {
                cache.push_back({definition,source,result,factor,++use});cacheBytes+=bytes;return result;
            }
        }
    } catch(...) { } // Optional CPU/font resources keep the original bitmap.
    traceReplacement("raster-failed",textureId,source,factor,definition.get());
    failedRasters[failedRasterNext++%failedRasters.size()]={definition,source,factor};
    return {};
}
}
namespace Tests {
// A narrow contract seam; gameplay never changes rasterizer availability.
void forceSubtitleFontRasterizerUnavailable(bool unavailable) noexcept {
    std::lock_guard lock(registryMutex);forceRasterizerUnavailableForTest=unavailable;
}
uint64_t subtitleFontRasterizerAttempts() noexcept {
    std::lock_guard lock(registryMutex);return rasterizerAttempts;
}
}
unsigned subtitleFontRasterScale(unsigned renderHeight,unsigned subtitlePercent) noexcept {
    const uint64_t demand=uint64_t(renderHeight)*subtitlePercent;
    unsigned factor=1;while(factor<8 && demand>uint64_t(720)*100*factor)factor*=2;
    return factor;
}
bool subtitleFontResourceAllowsReplacement(uint8_t* base,uint32_t resource) noexcept {
    // CStr::Str (821F76B8): +4 points at a big-endian flags word followed
    // by narrow characters. Wide/invalid strings do not authorize substitution.
    uint32_t buffer=0;
    uint8_t flags[2]{};
    if(!word(base,uint64_t(resource)+4,buffer) || !copy(base,buffer,flags,2) || (be16(flags)&0xa000))return false;
    std::array<char,260> name{};
    for(size_t i=0;i<name.size();++i) {
        if(!copy(base,uint64_t(buffer)+2+i,&name[i],1))return false;
        if(name[i])continue;
        std::string_view leaf(name.data(),i);
        const auto slash=leaf.find_last_of("\\/:");
        if(slash!=leaf.npos)leaf.remove_prefix(slash+1);
        leaf=leaf.substr(0,leaf.find('.'));
        const bool replaceable=leaf.size()==4 && (leaf[0]=='T' || leaf[0]=='t') &&
            (leaf[1]=='E' || leaf[1]=='e') && (leaf[2]=='X' || leaf[2]=='x') && (leaf[3]=='T' || leaf[3]=='t');
        if(diagnostics())std::fprintf(stderr,"[SubtitleFontTrace] resource=%s replaceable=%u\n",name.data(),unsigned(replaceable));
        return replaceable;
    }
    return false;
}
void registerSubtitleFont(uint8_t* base,uint32_t font,bool replaceable) noexcept {
    try {
        if(!replaceable) {
            uint32_t id=0;
            if(!word(base,uint64_t(font)+88,id))return;
            std::lock_guard lock(registryMutex);
            std::erase_if(registrations,[&](const auto& entry){
                return entry.definition->font==font && entry.definition->textureId==id;
            });
            return;
        }
        const char* reason="unknown";
        auto definition=snapshot(base,font,reason);
        static std::atomic<unsigned> lines{0};
        const bool tracing=diagnostics() && lines++<16;
        if(tracing) {
            uint32_t id=0,vector=0,count=0;word(base,uint64_t(font)+88,id);word(base,uint64_t(font)+12,vector);
            if(vector)word(base,uint64_t(vector)+4,count);
            std::fprintf(stderr,"[SubtitleFontTrace] register font=%08X reason=%s id=%u records=%u\n",font,reason,id,count);
        }
        if(!definition)return;
        if(definition->russian && !approvedRussianMetadata(*definition)) {
            // Invalidate a previous approved definition if this address is reused by another font.
            registerSubtitleFont(base,font,false);
            return;
        }
        definition->alpha=previewAlphaImageForSubtitleFont(definition->textureId);
        definition->color=previewColorImageForSubtitleFont(definition->textureId);
        if(definition->alpha && (definition->alpha->width!=definition->width || definition->alpha->height!=definition->height ||
           definition->alpha->pixels.size()!=size_t(definition->width)*definition->height))definition->alpha.reset();
        if(definition->color && (!definition->color->valid() || definition->color->faces!=1 || definition->color->firstMip ||
           definition->color->width!=definition->width || definition->color->height!=definition->height))definition->color.reset();
        if(tracing)std::fprintf(stderr,"[SubtitleFontTrace] register-owned id=%u size=%ux%u alpha=%p color=%p\n",
            definition->textureId,definition->width,definition->height,definition->alpha.get(),definition->color.get());
        // Keep approved metadata when loading precedes the first atlas upload.
        // The lazy retry must never authorize a decorative font by itself.
        std::lock_guard lock(registryMutex);
        const auto existing=std::find_if(registrations.begin(),registrations.end(),
            [&](const auto& entry){return entry.definition->textureId==definition->textureId;});
        if(existing!=registrations.end()) {
            const auto& previous=*existing->definition;
            if(previous.font==definition->font && previous.width==definition->width && previous.height==definition->height &&
               previous.russian==definition->russian &&
               previous.alpha==definition->alpha && previous.color==definition->color && previous.cells==definition->cells)return;
        }
        definition->generation=++generation;
        std::erase_if(registrations,[&](const auto& r){return r.definition->textureId==definition->textureId;});
        if(registrations.size()>=32)registrations.erase(registrations.begin());
        registrations.push_back({std::move(definition),{}});
    } catch(...) {}
}
void ensureSubtitleFontRegistered(uint8_t* base,uint32_t font) noexcept {
    try {
        uint32_t vtable=0,id=0;
        if(!word(base,font,vtable) || vtable!=0x82073268 || !word(base,uint64_t(font)+88,id) || !id || id>65535)return;
        const auto alpha=previewAlphaImageForSubtitleFont(id);
        const auto color=previewColorImageForSubtitleFont(id);
        if(!alpha && !color)return;
        {
            std::lock_guard lock(registryMutex);
            const auto existing=std::find_if(registrations.begin(),registrations.end(),
                [&](const auto& entry){return entry.definition->textureId==id;});
            if(existing==registrations.end() || existing->definition->font!=font)return;
            if(existing->definition->alpha==alpha && existing->definition->color==color)return;
        }
        registerSubtitleFont(base,font,true);
    } catch(...) {}
}
std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t textureId,const std::shared_ptr<const ColorImage>& source,unsigned factor) noexcept {
    if(factor==1)return {};
    try {std::lock_guard lock(registryMutex);return replacementLocked(textureId,source,factor);}catch(...) {return {};}
}
std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t textureId,const std::shared_ptr<const AlphaImage>& source,unsigned factor) noexcept {
    if(!source || factor==1 || !source->width || !source->height || source->width>512 || source->height>256 ||
       source->pixels.size()!=size_t(source->width)*source->height)return {};
    try {
        std::lock_guard lock(registryMutex);
        const auto registration=std::find_if(registrations.begin(),registrations.end(),[&](const auto& r){return r.definition->textureId==textureId;});
        if(registration==registrations.end() || registration->definition->alpha!=source)return {};
        auto found=std::find_if(alphaSources.begin(),alphaSources.end(),[&](const auto& entry){return entry.alpha==source;});
        if(found==alphaSources.end()) {
            auto color=std::make_shared<ColorImage>();color->width=source->width;color->height=source->height;color->sourceCodec=2;
            color->pixels.resize(source->pixels.size()*4,255);
            for(size_t i=0;i<source->pixels.size();++i)color->pixels[i*4+3]=source->pixels[i];
            if(alphaSources.size()>=16)alphaSources.erase(std::min_element(alphaSources.begin(),alphaSources.end(),
                [](const auto& a,const auto& b){return a.used<b.used;}));
            alphaSources.push_back({source,color,++use});found=std::prev(alphaSources.end());
        }
        found->used=++use;return replacementLocked(textureId,found->color,factor);
    }catch(...) {return {};}
}
}
