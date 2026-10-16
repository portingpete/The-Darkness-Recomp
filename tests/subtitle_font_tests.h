#pragma once
#include "renderer/engine/subtitle_font.h"
#include "renderer/d3d11/engine_preview.h"
#include "runtime/native/graphics_settings.h"
#include "runtime/native/native_menu_text.h"

namespace DarkRecomp::Native::Tests {
void forceSubtitleFontRasterizerUnavailable(bool unavailable) noexcept;
uint64_t subtitleFontRasterizerAttempts() noexcept;
}

// Synthetic original atlas/guest metadata plus real system-font rasterization.
// This tests rendering contracts without launching the game or touching saves.
struct SubtitleFontFixture {
    std::vector<uint8_t> memory = std::vector<uint8_t>(8192);
    uint32_t textureId;
    std::shared_ptr<const AlphaImage> alpha;
    std::shared_ptr<const ColorImage> color;
    const unsigned width,height;
    explicit SubtitleFontFixture(uint32_t id,bool modern=false,unsigned w=64,unsigned h=32,bool deferred=false,
                                 const char* resource="TEXT"):
        memory(4096+size_t(w)*h),textureId(id),width(w),height(h) {
        auto* b=memory.data();
        put(b+64,0x82097610);put(b+72,4096);put(b+76,width*height);
        put(b+80,width);put(b+84,height);put(b+88,width);put(b+92,1);
        put(b+96,0x40000);put(b+104,0x800);
        put(b+128,0x82073268);put(b+140,256);put(b+192,std::bit_cast<uint32_t>(1.f/width));
        put(b+196,std::bit_cast<uint32_t>(1.f/height));put(b+212,384);put(b+216,id);
        put(b+260,4);put(b+280,512);put(b+440,448);put(b+452,1);
        const uint16_t codes[]{'A',0xffff,' ','.'};
        for(unsigned i=0;i<4;++i) {
            auto* r=b+512+i*40;
            put(r,std::bit_cast<uint32_t>(float(i)/4));put(r+4,0);
            put(r+8,std::bit_cast<uint32_t>(float(i+1)/4));put(r+12,std::bit_cast<uint32_t>(1.f));
            put(r+16,std::bit_cast<uint32_t>(float(width/4)));put(r+20,std::bit_cast<uint32_t>(float(height)));
            r[32]=uint8_t(codes[i]>>8);r[33]=uint8_t(codes[i]);
        }
        for(unsigned y=7;y<24;++y)for(unsigned x=4;x<12;++x)
            b[4096+y*width+x]=uint8_t(x==4 || x==11 || y==7 || y==15?255:0);
        for(unsigned y=9;y<22;++y)b[4096+y*width+width/4+6]=180;
        b[4096+25*width+3*width/4+6]=255;
        enableEnginePreview();previewPrepareTexture(id);
        auto image=std::make_shared<ColorImage>();image->width=width;image->height=height;
        if(modern) {
            require(decodeUploadImage(b,64,4096,*image)==nullptr,"Modern font upload decode failed");
            if(deferred)color=std::move(image);
            else {
                previewPublishTexture(id,0x80000000u+id,image);color=previewColorImageForSubtitleFont(id);
                require(color==image && !previewAlphaImageForSubtitleFont(id),"Modern font fixture did not capture owned color generation");
            }
        } else {
            previewObserveImage(b,64,id,0);
            alpha=previewAlphaImageForSubtitleFont(id);require(bool(alpha),"Font fixture alpha upload failed");
            image->pixels.resize(size_t(width)*height*4,255);
            for(size_t i=0;i<alpha->pixels.size();++i)image->pixels[i*4+3]=alpha->pixels[i];
            color=std::move(image);
        }
        put(b+772,800);std::memcpy(b+802,resource,std::strlen(resource)+1);
        registerSubtitleFont(b,128,subtitleFontResourceAllowsReplacement(b,768));
    }
};
static void subtitleFontCpuContract() {
    require(subtitleFontRasterScale(720,100)==1 && subtitleFontRasterScale(720,50)==1 &&
        subtitleFontRasterScale(1080,100)==2 && subtitleFontRasterScale(2160,100)==4 &&
        subtitleFontRasterScale(2160,200)==8 && subtitleFontRasterScale(2160,50)==2,"Font raster density differs");
    // Every stock locale's alphabet must use the same vector path, including accents.
    for(const auto text : {u"English: The quick brown fox.", u"Deutsch: ÄÖÜäöüß",
                          u"Français: ÀÂÆÇÈÉÊËÎÏÔŒÙÛÜŸàâæçèéêëîïôœùûüÿ",
                          u"Español: ¡¿ÁÉÍÓÚÜÑáéíóúüñ", u"Italiano: ÀÈÉÌÒÙàèéìòù"})
        for(const auto* character=text;*character;++character)
            require(westernSubtitleCodepoint(uint16_t(*character))==uint32_t(*character),
                "Supported Western locale alphabet lost vector mapping");
    for(uint16_t slot=0xc0;slot<=0xff;++slot)
        require(russianSubtitleCodepoint(slot)==uint32_t(0x0410+slot-0xc0),
            "Russian Cyrillic letter changed identity");
    require(russianSubtitleCodepoint(0xa8)==0x0401 && russianSubtitleCodepoint(0xb8)==0x0451 &&
            russianSubtitleCodepoint('A')=='A' && russianSubtitleCodepoint(0x201c)==0x201c,
        "Russian Yo, ASCII or Unicode punctuation lost vector mapping");
    for(const uint16_t slot:{0x80,0x81,0xa1,0xa9,0xab,0xbf,0x0416,0xffff})
        require(!russianSubtitleCodepoint(slot),"Russian custom glyph acquired a guessed letter");
    for(const bool modern:{false,true}) {
        SubtitleFontFixture localized(modern?60125:60124,modern);
        // The unsupported second cell supplies a bitmap fallback baseline.
        std::array<std::shared_ptr<const ColorImage>,3> authored;
        for(unsigned i=0;i<authored.size();++i) {
            authored[i]=subtitleFontReplacementFor(localized.textureId,localized.color,2u<<i);
            require(bool(authored[i]),"ASCII glyph lost its vector replacement");
        }
        for(const uint16_t slot:{0x81,0x8d,0x8f,0x90,0x9d,0x0416,0x0401,0x4f60,0xffff}) {
            require(!westernSubtitleCodepoint(slot),"Custom slot acquired a guessed Western character");
            auto* code=localized.memory.data()+512+40+32;
            code[0]=uint8_t(slot>>8);code[1]=uint8_t(slot);
            registerSubtitleFont(localized.memory.data(),128,true);
            for(unsigned i=0;i<authored.size();++i) {
                const unsigned factor=2u<<i;
                const auto result=subtitleFontReplacementFor(localized.textureId,localized.color,factor);
                require(bool(result),"Localized slot disabled unrelated ASCII rendering");
                for(unsigned y=0;y<localized.height*factor;++y)
                    for(unsigned x=localized.width/4*factor;x<localized.width/2*factor;++x) {
                        const auto at=(size_t(y)*result->width+x)*4;
                        require(std::memcmp(result->pixels.data()+at,authored[i]->pixels.data()+at,4)==0,
                            "Custom glyph was reinterpreted instead of preserving authored coverage");
                    }
            }
        }
        for(const uint16_t slot:{0xa1,0xbf,0xc4,0xc6,0xc7,0xdf,0xe1,0xe9,0xed,0xf1,0xf3,0xfa,0xfc,
                                  0x80,0x93,0x96,0x0152,0x0160,0x2014,0x201c,0x20ac}) {
            const auto mapped=westernSubtitleCodepoint(slot);
            require(mapped.has_value(),"Known Western character retained blurry bitmap fallback");
            if(slot>=0xa0)require(*mapped==slot,"Western Unicode character changed identity");
            auto* code=localized.memory.data()+512+40+32;
            code[0]=uint8_t(slot>>8);code[1]=uint8_t(slot);
            registerSubtitleFont(localized.memory.data(),128,true);
            for(unsigned i=0;i<authored.size();++i) {
                const unsigned factor=2u<<i;
                const auto result=subtitleFontReplacementFor(localized.textureId,localized.color,factor);
                require(bool(result),"Western character lost its vector atlas");
                bool changed=false,visible=false;
                for(unsigned y=0;y<localized.height*factor;++y)
                    for(unsigned x=localized.width/4*factor;x<localized.width/2*factor;++x) {
                        const auto at=(size_t(y)*result->width+x)*4+3;
                        changed|=result->pixels[at]!=authored[i]->pixels[at];
                        visible|=result->pixels[at]!=0;
                    }
                require(changed && visible,"Western accent did not receive sharp vector lettering");
            }
        }
    }
    require(westernSubtitleCodepoint(0x80)==0x20ac && westernSubtitleCodepoint(0x93)==0x201c &&
        westernSubtitleCodepoint(0x96)==0x2013,"Windows-1252 punctuation mapped to the wrong character");
    {
        initializeNativeMenuText(true,1);
        struct LocaleOwner {~LocaleOwner(){initializeNativeMenuText(false,1);}} localeOwner;
        SubtitleFontFixture russian(60126,true,64,32,true);
        const auto original=russian.color->pixels;
        const auto attempts=DarkRecomp::Native::Tests::subtitleFontRasterizerAttempts();
        require(subtitleFontResourceAllowsReplacement(russian.memory.data(),768),
            "Russian TEXT resource was excluded before its fingerprint could be checked");
        previewPublishTexture(russian.textureId,0x80000000u+russian.textureId,russian.color);
        ensureSubtitleFontRegistered(russian.memory.data(),128);
        for(const unsigned factor:{1u,2u,4u,8u})
            require(!subtitleFontReplacementFor(russian.textureId,russian.color,factor),
                "Unrecognized Russian font lost its original glyph atlas");
        require(russian.color->pixels==original &&
            attempts==DarkRecomp::Native::Tests::subtitleFontRasterizerAttempts(),
            "Russian font pixels changed or reached the rasterizer");
    }
    for(const bool modern:{false,true}) {
        SubtitleFontFixture decorative(modern?60121:60120,modern,64,32,false,"DARKNESS");
        const auto original=decorative.color->pixels;
        const auto attempts=DarkRecomp::Native::Tests::subtitleFontRasterizerAttempts();
        for(const unsigned factor:{1u,2u,4u,8u}) {
            ensureSubtitleFontRegistered(decorative.memory.data(),128);
            require(!subtitleFontReplacementFor(decorative.textureId,decorative.color,factor) &&
                !subtitleFontReplacementFor(decorative.textureId,decorative.alpha,factor),
                "Darkness quotes acquired replacement lettering");
        }
        require(decorative.color->pixels==original &&
            attempts==DarkRecomp::Native::Tests::subtitleFontRasterizerAttempts(),
            "Decorative font pixels changed or reached the rasterizer");
    }
    SubtitleFontFixture resource(60122,true,64,32,false,"fonts\\text.xfc");
    require(bool(subtitleFontReplacementFor(resource.textureId,resource.color,4)),
        "Named ordinary font lost replacement lettering");
    registerSubtitleFont(resource.memory.data(),128,false);
    ensureSubtitleFontRegistered(resource.memory.data(),128);
    require(!subtitleFontReplacementFor(resource.textureId,resource.color,4),
        "Decorative load at a reused font address retained cached lettering");
    for(const char* name:{"DARKNESS","fonts/DARKNESS.xfc","TEXTURE","UNKNOWN",""}) {
        std::memset(resource.memory.data()+802,0,260);
        std::memcpy(resource.memory.data()+802,name,std::strlen(name));
        require(!subtitleFontResourceAllowsReplacement(resource.memory.data(),768),
            "Decorative or unknown font resource authorized replacement");
    }
    std::memcpy(resource.memory.data()+802,"TEXT",5);
    resource.memory[800]=0x20;
    require(!subtitleFontResourceAllowsReplacement(resource.memory.data(),768),
        "Wide resource name authorized replacement");
    resource.memory[800]=0;
    std::memset(resource.memory.data()+802,'T',260);
    require(!subtitleFontResourceAllowsReplacement(resource.memory.data(),768) &&
        !subtitleFontResourceAllowsReplacement(resource.memory.data(),0),
        "Invalid resource name authorized replacement");
    SubtitleFontFixture deferredDecorative(60123,true,64,32,true,"DARKNESS");
    previewPublishTexture(deferredDecorative.textureId,0x80000000u+deferredDecorative.textureId,deferredDecorative.color);
    ensureSubtitleFontRegistered(deferredDecorative.memory.data(),128);
    require(!subtitleFontReplacementFor(deferredDecorative.textureId,deferredDecorative.color,4),
        "Lazy Darkness atlas upload authorized replacement lettering");
    SubtitleFontFixture f(60101);const auto original=f.color->pixels;
    require(!subtitleFontReplacementFor(f.textureId,f.color,1) &&
        !subtitleFontReplacementFor(f.textureId,f.color,3) &&
        !subtitleFontReplacementFor(60199,f.color,4),"Original/unregistered font should retain original pixels");
    for(unsigned factor:{2u,4u,8u}) {
        auto result=subtitleFontReplacementFor(f.textureId,f.color,factor);
        require(result && result->valid() && result->generatedSubtitleFont &&
            result->width==64*factor && result->height==32*factor,"Vector atlas was not created");
        require(result==subtitleFontReplacementFor(f.textureId,f.color,factor),"Font cache did not reuse immutable result");
        ensureSubtitleFontRegistered(f.memory.data(),128);registerSubtitleFont(f.memory.data(),128,true);
        require(result==subtitleFontReplacementFor(f.textureId,f.color,factor),"Repeated current-font registration invalidated vector cache");
        unsigned changed=0,covered=0;
        for(unsigned y=7*factor;y<24*factor;++y)for(unsigned x=4*factor;x<12*factor;++x) {
            const auto value=result->pixels[(size_t(y)*result->width+x)*4+3];
            covered+=value!=0;changed+=value!=original[(size_t(y/factor)*64+x/factor)*4+3];
        }
        require(changed>0 && covered>0,"Vector glyph did not replace bitmap detail");
        require(result->pixels[(size_t(12*factor)*result->width+22*factor)*4+3]>0,"Missing vector glyph lost original coverage");
        require(result->pixels[(size_t(12*factor)*result->width+40*factor)*4+3]==0,"Empty glyph cell became visible");
        for(unsigned y=0;y<32*factor;++y)for(unsigned x=0;x<16*factor;++x)
            if(x<4*factor || x>=12*factor || y<7*factor || y>=24*factor)
                require(result->pixels[(size_t(y)*result->width+x)*4+3]==0,"Vector glyph escaped original ink bounds");
        const auto alphaResult=subtitleFontReplacementFor(f.textureId,f.alpha,factor);
        require(alphaResult && alphaResult->pixels==result->pixels,"Alpha/color paths rasterized differently");
    }
    require(f.color->pixels==original,"Font replacement mutated original atlas");
    auto foreign=std::make_shared<ColorImage>(*f.color);foreign->pixels[3]^=1;
    require(!subtitleFontReplacementFor(f.textureId,foreign,4),"Wrong atlas generation accepted");
    previewObserveImage(f.memory.data(),64,f.textureId,0);
    require(!subtitleFontReplacementFor(f.textureId,f.color,4) &&
        !subtitleFontReplacementFor(f.textureId,f.alpha,4),"Reused texture ID inherited font metadata");
    registerSubtitleFont(f.memory.data(),128,true);
    require(bool(subtitleFontReplacementFor(f.textureId,f.color,4)),"New captured generation did not register");
    SubtitleFontFixture modern(60105,true);
    require(bool(subtitleFontReplacementFor(modern.textureId,modern.color,4)),"Modern color upload did not register");
    auto reuploaded=std::make_shared<ColorImage>(*modern.color);
    require(!subtitleFontReplacementFor(modern.textureId,reuploaded,4),"Modern font accepted uncaptured image identity");
    previewPrepareTexture(modern.textureId);previewPublishTexture(modern.textureId,0x80000000u+modern.textureId,reuploaded);
    require(!subtitleFontReplacementFor(modern.textureId,modern.color,4) &&
        !subtitleFontReplacementFor(modern.textureId,reuploaded,4),"Modern reused ID inherited stale glyph metadata");
    registerSubtitleFont(modern.memory.data(),128,true);
    require(bool(subtitleFontReplacementFor(modern.textureId,reuploaded,4)),"Modern new generation did not register");
    SubtitleFontFixture late(60109,true,64,32,true);
    ensureSubtitleFontRegistered(late.memory.data(),128);
    require(!subtitleFontReplacementFor(late.textureId,late.color,4),"Uncaptured lazy font atlas acquired metadata");
    previewPublishTexture(late.textureId,0x80000000u+late.textureId,late.color);
    require(!subtitleFontReplacementFor(late.textureId,late.color,4),"Font attached lazily by texture ID alone");
    ensureSubtitleFontRegistered(late.memory.data(),128);
    const auto delayed=subtitleFontReplacementFor(late.textureId,late.color,4);
    require(bool(delayed),"Current verified font did not register after its lazy upload");
    ensureSubtitleFontRegistered(late.memory.data(),128);
    require(delayed==subtitleFontReplacementFor(late.textureId,late.color,4),"Repeated lazy-font ensure invalidated its cache");
    SubtitleFontFixture overlap(60102);
    // A new invalid load cannot attach an ambiguous UV map to its new atlas.
    previewObserveImage(overlap.memory.data(),64,overlap.textureId,0);
    put(overlap.memory.data()+512+40,std::bit_cast<uint32_t>(0.f));
    registerSubtitleFont(overlap.memory.data(),128,true);
    require(!subtitleFontReplacementFor(overlap.textureId,overlap.color,4),"Overlapping glyph cells accepted");
    ColorImage oversized;oversized.width=4096;oversized.height=1;oversized.pixels.resize(4096*4);
    require(!oversized.valid(),"Authored image cap changed");oversized.generatedSubtitleFont=true;
    require(oversized.valid(),"Generated font image cap rejected4096");oversized.faces=6;
    require(!oversized.valid(),"Generated font accepted cube faces");
    SubtitleFontFixture unavailable(60108,true);
    using namespace DarkRecomp::Native::Tests;
    forceSubtitleFontRasterizerUnavailable(true);
    struct RasterizerOwner {~RasterizerOwner(){forceSubtitleFontRasterizerUnavailable(false);}} rasterizerOwner;
    const auto attempts=subtitleFontRasterizerAttempts();
    require(!subtitleFontReplacementFor(unavailable.textureId,unavailable.color,4) &&
        !subtitleFontReplacementFor(unavailable.textureId,unavailable.color,4) &&
        subtitleFontRasterizerAttempts()==attempts+1,"CPU font failure retried on every glyph draw");
    require(!subtitleFontReplacementFor(unavailable.textureId,unavailable.color,2) &&
        subtitleFontRasterizerAttempts()==attempts+2,"CPU font failure suppressed a new raster density");
    unavailable.memory[512+33]='B';
    registerSubtitleFont(unavailable.memory.data(),128,true);
    require(!subtitleFontReplacementFor(unavailable.textureId,unavailable.color,4) &&
        subtitleFontRasterizerAttempts()==attempts+3,"CPU font failure suppressed a new definition generation");
    forceSubtitleFontRasterizerUnavailable(false);
    require(!subtitleFontReplacementFor(unavailable.textureId,unavailable.color,4) &&
        subtitleFontRasterizerAttempts()==attempts+3,"Failed font retried unchanged generation after availability change");
    unavailable.memory[512+33]='A';
    registerSubtitleFont(unavailable.memory.data(),128,true);
    require(bool(subtitleFontReplacementFor(unavailable.textureId,unavailable.color,4)) &&
        subtitleFontRasterizerAttempts()==attempts+4,"Fresh font generation did not recover after rasterizer failure");
    std::puts("SubtitleFont CPU: vector2/4/8 density, authored localized glyphs, Russian atlas preservation, immutable cache and generation guards passed.");
}
static void subtitleFontBoundTexture(ID3D11DeviceContext* context,unsigned slot,unsigned width,unsigned height,bool samplerCheck,unsigned mips=1) {
    ComPtr<ID3D11ShaderResourceView> view;context->PSGetShaderResources(slot,1,&view);
    require(bool(view),"Font texture was not bound");
    ComPtr<ID3D11Resource> resource;ComPtr<ID3D11Texture2D> texture;
    view->GetResource(&resource);check(resource.As(&texture),"Font texture query failed");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    require(desc.Width==width && desc.Height==height && desc.MipLevels==mips,"Wrong font atlas dimensions/mips bound");
    if(samplerCheck) {
        ComPtr<ID3D11SamplerState> sampler;context->PSGetSamplers(0,1,&sampler);
        require(bool(sampler),"Font sampler missing");D3D11_SAMPLER_DESC state{};sampler->GetDesc(&state);
        require(state.Filter==D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT && state.MinLOD==0 && state.MaxLOD==0 &&
            state.AddressU==D3D11_TEXTURE_ADDRESS_CLAMP && state.AddressV==D3D11_TEXTURE_ADDRESS_CLAMP,
            "Vector font did not bind linear base-level clamp sampling");
    }
}
static void subtitleFontWorldContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    SubtitleFontFixture f(60103,true);WorldRendererD3D11 renderer(device,context,1);
    auto geometry=std::make_shared<StoredGeometry>();geometry->vertexCount=4;geometry->stride=32;
    geometry->formats[0]=geometry->formats[1]=4;geometry->vertices.resize(128);geometry->indices={0,1,2,0,2,3};
    const EngineVector positions[]{{-.75f,-.75f,.5f,1},{-.75f,.75f,.5f,1},{.75f,.75f,.5f,1},{.75f,-.75f,.5f,1}};
    const EngineVector uv[]{{0,1,0,1},{0,0,0,1},{.25f,0,0,1},{.25f,1,0,1}};
    for(unsigned v=0;v<4;++v)for(unsigned lane=0;lane<4;++lane) {
        put(geometry->vertices.data()+v*32+lane*4,std::bit_cast<uint32_t>(positions[v][lane]));
        put(geometry->vertices.data()+v*32+16+lane*4,std::bit_cast<uint32_t>(uv[v][lane]));
    }
    const auto originalGeometry=geometry->vertices;
    WorldDraw draw;draw.geometry={geometry,geometry,0,6};draw.targets={60111,0,0,0,60112};draw.viewport={0,0,128,128};
    draw.material=WorldMaterial::fixed;draw.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
    draw.options.modes.fill(4);draw.options.modes[0]=0;
    for(unsigned lane=0;lane<4;++lane)draw.constants.vectors[lane][lane]=1;
    draw.constants.references[0][2]=10;draw.constants.vectors[10]={1,1,1,1};
    put(draw.attributes.data()+92,0x01100008);draw.attributes[96]=draw.attributes[97]=8;
    draw.attributes[144]=5;draw.attributes[145]=6;draw.textureIds[0]=f.textureId;draw.textures[0]=f.color;
    draw.samplers[0].valid=true;draw.samplers[0].minLinear=draw.samplers[0].magLinear=true;
    draw.samplers[0].address.fill(2);draw.samplers[0].baseOnly=true;
    WorldClear clear;clear.targets=draw.targets;clear.viewport=draw.viewport;clear.flags=49;clear.color={.1f,.2f,.3f,1};clear.depth=.4f;
    auto render=[&] {renderer.clear(clear);require(renderer.draw(draw),"Subtitle world draw failed");return renderer.readSurface(60111,false);};
    renderer.setSubtitleRenderHeight(2160);const auto original=render();
    draw.subtitle=true;renderer.setSubtitleRenderHeight(720);
    require(render()==original,"720p100% subtitle pixels changed");
    renderer.setSubtitleRenderHeight(2160);renderer.clear(clear);const auto depth=renderer.readSurface(60112,true);
    require(renderer.draw(draw),"Vector subtitle world draw failed");subtitleFontBoundTexture(context,0,256,128,true);
    const auto vector=renderer.readSurface(60111,false);
    require(vector!=original,"Subtitle world output did not gain vector detail");
    require(renderer.readSurface(60112,true)==depth,"Subtitle changed depth/stencil");
    draw.subtitle=false;require(render()==original,"Unrelated font draw used replacement atlas");
    SubtitleFontFixture darkness(60124,true,64,32,false,"DARKNESS");
    draw.subtitle=true;draw.textureIds[0]=darkness.textureId;draw.textures[0]=darkness.color;
    require(render()==original,"Darkness world subtitle changed original lettering");
    subtitleFontBoundTexture(context,0,64,32,false,7);
    draw.textureIds[0]=f.textureId;draw.textures[0]=f.color;
    draw.subtitle=true;draw.constants.vectors[10][3]=.35f;
    const auto faded=render();require(faded!=vector && faded!=original,"Subtitle fade was lost");
    require(geometry->vertices==originalGeometry && f.color->pixels.size()==64*32*4,"Native geometry/atlas mutated");
    SubtitleFontFixture full(60106,true,512,256);
    const auto fullAtlas=subtitleFontReplacementFor(full.textureId,full.color,8);
    require(fullAtlas && fullAtlas->width==4096 && fullAtlas->height==2048 &&
        fullAtlas==subtitleFontReplacementFor(full.textureId,full.color,8),"Full atlas cache/cap rejected");
    draw.textureIds[0]=full.textureId;draw.textures[0]=full.color;renderer.setSubtitleRenderHeight(4320);
    renderer.clear(clear);require(renderer.draw(draw),"Full native font atlas upload failed");
    subtitleFontBoundTexture(context,0,4096,2048,true);
    WorldRendererD3D11 limited(device,context,1);
    WorldImageBudgetTestAccess::setBudget(limited,1024*1024);
    WorldImageBudgetTestAccess::setSampleDeadline(limited,UINT64_MAX);
    limited.setSubtitleRenderHeight(2160);draw.subtitle=false;limited.clear(clear);
    require(limited.draw(draw),"Original atlas did not fit limited GPU budget");
    const auto limitedOriginal=limited.readSurface(60111,false);
    draw.subtitle=true;limited.clear(clear);require(limited.draw(draw),"Generated atlas budget fallback rejected subtitle");
    require(limited.readSurface(60111,false)==limitedOriginal,"Generated atlas budget failure changed original pixels/sampler");
    subtitleFontBoundTexture(context,0,512,256,false,10);
    const auto deniedAtlas=subtitleFontReplacementFor(full.textureId,full.color,4);
    require(!WorldImageBudgetTestAccess::contains(limited,deniedAtlas.get()),"Over-budget generated atlas uploaded");
    WorldImageBudgetTestAccess::setBudget(limited,64*1024*1024);
    limited.clear(clear);require(limited.draw(draw),"Blacklisted generated atlas rejected original subtitle");
    require(limited.readSurface(60111,false)==limitedOriginal &&
        !WorldImageBudgetTestAccess::contains(limited,deniedAtlas.get()),"Failed atlas retried every draw");
    limited.setSubtitleRenderHeight(4320);limited.clear(clear);require(limited.draw(draw),"Changed density could not retry font atlas");
    subtitleFontBoundTexture(context,0,4096,2048,true);
    std::puts("SubtitleFont World GPU: native720 unchanged, high-resolution atlas bound, unrelated draws/depth/geometry retained, fade preserved.");
}
static void subtitleFontSimpleContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    SubtitleFontFixture f(60104);
    HWND window=CreateWindowExW(0,L"STATIC",L"subtitle font contract",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window!=nullptr,"Font test window creation failed");
    struct WindowOwner {HWND handle;~WindowOwner(){DestroyWindow(handle);}} owner{window};
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;ComPtr<IDXGISwapChain> swap;
    check(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"Font DXGI device");check(dxgi->GetAdapter(&adapter),"Font adapter");
    check(adapter->GetParent(IID_PPV_ARGS(&factory)),"Font factory");
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=desc.BufferDesc.Height=64;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;
    check(factory->CreateSwapChain(device,&desc,&swap),"Font swapchain");
    EnginePreviewD3D11 renderer(device,context,swap.Get(),128,1440);
    SimpleMesh mesh;mesh.texture=f.alpha;mesh.textureId=f.textureId;mesh.indices={0,1,2,0,2,3};
    mesh.vertices={{{-.75f,-.75f,.5f},{0,1},{1,1,1,1}},{{-.75f,.75f,.5f},{0,0},{1,1,1,1}},
                   {{.75f,.75f,.5f},{.25f,0},{1,1,1,1}},{{.75f,-.75f,.5f},{.25f,1},{1,1,1,1}}};
    for(unsigned lane=0;lane<4;++lane)mesh.projection[lane*4+lane]=1;
    renderer.render({mesh});subtitleFontBoundTexture(context,0,64,32,false);
    std::vector<uint32_t> original;
    for(unsigned y=360;y<1080;y+=72)for(unsigned x=30;x<95;x+=8)original.push_back(renderer.readPixel(x,y));
    mesh.subtitle=true;renderer.render({mesh});subtitleFontBoundTexture(context,2,128,64,true);
    unsigned differences=0;size_t index=0;
    for(unsigned y=360;y<1080;y+=72)for(unsigned x=30;x<95;x+=8)differences+=renderer.readPixel(x,y)!=original[index++];
    require(differences>0,"Simple alpha subtitles did not use vector detail");
    SubtitleFontFixture darkness(60125,false,64,32,false,"DARKNESS");
    mesh.texture=darkness.alpha;mesh.textureId=darkness.textureId;
    renderer.render({mesh});subtitleFontBoundTexture(context,0,64,32,false);
    index=0;
    for(unsigned y=360;y<1080;y+=72)for(unsigned x=30;x<95;x+=8)
        require(renderer.readPixel(x,y)==original[index++],"Darkness simple subtitle changed original lettering");
    std::puts("SubtitleFont Simple GPU: owned alpha path gained vector coverage with linear base-level atlas sampling.");
}
static void subtitleFontContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    const auto saved=graphicsSettings();auto settings=saved;settings.subtitleScalePercent=100;settings.antialiasing=AntialiasingMode::Off;
    require(setGraphicsSettings(settings),"Font test settings rejected");
    struct SettingsOwner {GraphicsSettings saved;~SettingsOwner(){setGraphicsSettings(saved);}} owner{saved};
    subtitleFontCpuContract();subtitleFontWorldContract(device,context);subtitleFontSimpleContract(device,context);
}
