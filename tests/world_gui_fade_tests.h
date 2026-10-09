// The tentacle GUI reveals a background through a destination-alpha mask.
// Its RGB must match the source artwork across the original fade phases.
// Preserve the original mask so the later SRC_ALPHA extraction and additive
// light passes can still brighten the revealed backdrop.
static void worldGuiFadeContract(ID3D11Device* device,ID3D11DeviceContext* context) {
    constexpr unsigned width=8,height=8;
    constexpr float capturedGain=3.085f,capturedFade=.673f;
    const auto geometry=colorGradeQuad(width,height,width,height);
    auto makeImage=[] {
        auto image=std::make_shared<ColorImage>();image->width=width;image->height=height;
        image->authoredMips=true;image->pixels.resize(size_t(width)*height*4);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            auto* pixel=image->pixels.data()+(size_t(y)*width+x)*4;
            pixel[0]=uint8_t(17+x*11+y*3);pixel[1]=uint8_t(23+x*5+y*7);
            pixel[2]=uint8_t(31+x*3+y*9);pixel[3]=uint8_t(41+x*13+y*5);
        }
        return image;
    };
    auto setPixel=[](ColorImage& image,unsigned x,unsigned y,std::array<uint8_t,4> value) {
        std::copy(value.begin(),value.end(),image.pixels.begin()+(size_t(y)*image.width+x)*4);
    };
    auto sourcePixel=[](const ColorImage& image,unsigned x,unsigned y) {
        std::array<double,4> value{};
        for(unsigned c=0;c<4;++c)value[c]=image.pixels[(size_t(y)*image.width+x)*4+c]/255.0;
        return value;
    };
    auto originalResult=[](const std::array<double,4>& center,
                           const std::array<std::array<double,4>,4>& taps,
                           double gain,double fade) {
        std::array<double,4> result{};
        for(unsigned c=0;c<4;++c) {
            double sum=0;for(const auto& tap:taps)sum+=tap[c];
            const double boosted=center[c]+gain*sum;
            result[c]=fade*.28+(1-fade)*boosted;
        }
        return result;
    };
    auto sourceMatchedResult=[&](const std::array<double,4>& center,
                                const std::array<std::array<double,4>,4>& taps,
                                double gain,double fade) {
        auto result=originalResult(center,taps,gain,fade);
        for(unsigned c=0;c<3;++c)result[c]=center[c];
        return result;
    };
    auto readCenter=[&](WorldRendererD3D11& renderer,unsigned scale,uint32_t target=3201) {
        const auto pixels=renderer.readSurface(target,false);
        const unsigned physicalWidth=width*scale;
        require(pixels.size()==size_t(physicalWidth)*height*scale*8,"GUI fade target extent differs");
        const unsigned x=3*scale+scale/2,y=3*scale+scale/2;
        std::array<uint16_t,4> half{};
        std::memcpy(half.data(),pixels.data()+(size_t(y)*physicalWidth+x)*8,8);
        std::array<double,4> value{};
        for(unsigned c=0;c<4;++c) {
            const unsigned exponent=(half[c]>>10)&31,mantissa=half[c]&1023;
            value[c]=std::ldexp(double(exponent?1024+mantissa:mantissa),int(exponent?exponent:1)-25)*
                     (half[c]&0x8000?-1:1);
        }
        return value;
    };
    auto makeFadePass=[&](const std::shared_ptr<ColorImage>& image,float gain,float fade) {
        auto pass=colorGradeDraw(geometry,3201,width,height);
        pass.fragmentName="GUIFadeToWhite";
        // p0.x and p0.y offset horizontal and vertical samples by one source
        // texel; p0.z remains zero so the samples land on the four cardinal taps.
        pass.fragmentConstants[0]={1.0f/width,1.0f/height,0,0};
        pass.fragmentConstants[1]={0,gain,fade,0};
        pass.textures[0]=image;pass.samplers[0]=colorGradeSampler(false,2);
        return pass;
    };
    auto draw=[&](WorldRendererD3D11& renderer,const std::shared_ptr<ColorImage>& image,
                  float gain,float fade) {
        auto pass=makeFadePass(image,gain,fade);
        WorldClear clear;clear.targets=pass.targets;clear.viewport=pass.viewport;clear.flags=1;
        renderer.clear(clear);require(renderer.draw(pass),"GUIFadeToWhite draw rejected");
        return readCenter(renderer,renderer.renderScale());
    };
    auto verify=[&](const char* label,unsigned scale,const std::array<double,4>& actual,
                    const std::array<double,4>& expected) {
        for(unsigned c=0;c<4;++c)if(std::abs(actual[c]-expected[c])>.01) {
            std::fprintf(stderr,"GUIFade[%s scale=%u channel=%u] actual=%.8f expected=%.8f\n",
                         label,scale,c,actual[c],expected[c]);
            throw std::runtime_error("GUIFadeToWhite output differs from its channel contract");
        }
    };

    for(unsigned scale:{1u,3u}) {
        context->ClearState();WorldRendererD3D11 renderer(device,context,scale);
        auto arbitrary=makeImage();
        setPixel(*arbitrary,3,3,{31,79,143,211});
        const auto unchanged=draw(renderer,arbitrary,0,0);
        verify("zero-gain-copy",scale,unchanged,sourcePixel(*arbitrary,3,3));

        auto gray=makeImage();
        setPixel(*gray,3,3,{91,91,91,137});
        setPixel(*gray,2,3,{12,12,12,26});setPixel(*gray,4,3,{48,48,48,77});
        setPixel(*gray,3,2,{73,73,73,104});setPixel(*gray,3,4,{31,31,31,189});
        const std::array<std::array<unsigned,2>,4> tapCoordinates{{{{2,3}},{{4,3}},{{3,2}},{{3,4}}}};
        std::array<std::array<double,4>,4> grayTaps{};
        for(unsigned i=0;i<4;++i)grayTaps[i]=sourcePixel(*gray,tapCoordinates[i][0],tapCoordinates[i][1]);
        const auto grayExpected=sourceMatchedResult(sourcePixel(*gray,3,3),grayTaps,capturedGain,capturedFade);
        verify("grayscale-source",scale,draw(renderer,gray,capturedGain,capturedFade),grayExpected);

        auto cyan=makeImage();
        setPixel(*cyan,3,3,{31,46,61,77});
        setPixel(*cyan,2,3,{6,45,35,128});setPixel(*cyan,4,3,{8,50,40,64});
        setPixel(*cyan,3,2,{10,60,50,32});setPixel(*cyan,3,4,{12,70,60,96});
        std::array<std::array<double,4>,4> cyanTaps{};
        for(unsigned i=0;i<4;++i)cyanTaps[i]=sourcePixel(*cyan,tapCoordinates[i][0],tapCoordinates[i][1]);
        const auto cyanCenter=sourcePixel(*cyan,3,3);
        const auto cyanOriginal=originalResult(cyanCenter,cyanTaps,capturedGain,capturedFade);
        const auto cyanExpected=sourceMatchedResult(cyanCenter,cyanTaps,capturedGain,capturedFade);
        const auto cyanActual=draw(renderer,cyan,capturedGain,capturedFade);
        verify("colored-background-source",scale,cyanActual,cyanExpected);
        require(std::abs(cyanOriginal[3]-cyanExpected[3])<1e-10 &&
                std::abs(cyanActual[3]-cyanOriginal[3])<.01,
                "GUI background RGB adjustment changed the original alpha sum");
        require(std::abs(cyanOriginal[0]-cyanExpected[0])>.05,
                "GUI fade fixture does not distinguish the source from the original boost");
        for(const auto& phase:std::array<std::array<float,2>,3>{{{{0,capturedFade}},{{capturedGain,0}},{{capturedGain,1}}}})
            verify("fade-phase-source",scale,draw(renderer,cyan,phase[0],phase[1]),
                   sourceMatchedResult(cyanCenter,cyanTaps,phase[0],phase[1]));

        // RGB uses the original destination-alpha reveal. An RGB-only draw
        // must preserve that mask for the subsequent light extraction.
        for(float mask:{0.0f,.35f,1.0f}) {
            auto masked=makeFadePass(cyan,capturedGain,capturedFade);
            put(masked.attributes.data()+92,0x00100008);
            masked.attributes[144]=7;masked.attributes[145]=8;
            WorldClear maskedClear;maskedClear.targets=masked.targets;
            maskedClear.viewport=masked.viewport;maskedClear.flags=1;
            maskedClear.color={.2f,.4f,.6f,mask};renderer.clear(maskedClear);
            require(renderer.draw(masked),"Masked GUI background draw rejected");
            std::array<double,4> expected{.2,.4,.6,mask};
            for(unsigned c=0;c<3;++c)expected[c]=mask*cyanCenter[c]+(1-mask)*expected[c];
            verify("destination-alpha-mask",scale,readCenter(renderer,scale),expected);
        }

        // RGB-only and alpha-writing passes retain their original write masks
        // and alpha blend arithmetic under the source-RGB shader adjustment.
        auto otherBlend=makeFadePass(cyan,capturedGain,capturedFade);
        put(otherBlend.attributes.data()+92,0x00100008);
        otherBlend.attributes[144]=2;otherBlend.attributes[145]=1;
        WorldClear otherClear;otherClear.targets=otherBlend.targets;
        otherClear.viewport=otherBlend.viewport;otherClear.flags=1;
        otherClear.color={.2f,.4f,.6f,.35f};renderer.clear(otherClear);
        require(renderer.draw(otherBlend),"Other GUI blend draw rejected");
        auto otherExpected=cyanCenter;otherExpected[3]=.35;
        verify("other-blend-preserves-alpha",scale,readCenter(renderer,scale),otherExpected);

        auto alphaWriting=makeFadePass(cyan,capturedGain,capturedFade);
        put(alphaWriting.attributes.data()+92,0x01100008);
        alphaWriting.attributes[144]=7;alphaWriting.attributes[145]=8;
        otherClear.targets=alphaWriting.targets;renderer.clear(otherClear);
        require(renderer.draw(alphaWriting),"Alpha-writing GUI draw rejected");
        std::array<double,4> alphaExpected{.2,.4,.6,.35};
        for(unsigned c=0;c<3;++c)alphaExpected[c]=.35*cyanCenter[c]+.65*alphaExpected[c];
        alphaExpected[3]=.35*cyanOriginal[3]+.65*.35;
        verify("alpha-writing-original-blend",scale,readCenter(renderer,scale),alphaExpected);

        auto fadePass=makeFadePass(cyan,capturedGain,capturedFade);
        put(fadePass.attributes.data()+92,0x00100008);
        fadePass.attributes[144]=7;fadePass.attributes[145]=8;
        WorldClear fadeClear;fadeClear.targets=fadePass.targets;fadeClear.viewport=fadePass.viewport;fadeClear.flags=1;
        fadeClear.color={.2f,.4f,.6f,1};
        renderer.clear(fadeClear);require(renderer.draw(fadePass),"End-to-end GUI fade draw rejected");
        const auto scene=colorGradeResolve(renderer,fadePass,3202,width,height,26,4);

        // Exercise the menu's SRC_ALPHA/ZERO extraction and two ONE/ONE light
        // additions without intervening spatial filters. With a mask of 1,
        // extraction must retain the source and the additions produce 3x source.
        auto extraction=colorGradeDraw(geometry,3205,width,height);
        extraction.material=WorldMaterial::fixed;
        extraction.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
        extraction.textureObjects[0]=scene;extraction.samplers[0]=colorGradeSampler(false,2);
        put(extraction.attributes.data()+92,0x01100008);
        extraction.attributes[144]=5;extraction.attributes[145]=1;
        WorldClear extractClear;extractClear.targets=extraction.targets;
        extractClear.viewport=extraction.viewport;extractClear.flags=1;
        renderer.clear(extractClear);require(renderer.draw(extraction),"GUI light extraction rejected");
        auto extractionExpected=cyanCenter;extractionExpected[3]=1;
        verify("source-alpha-light-extraction",scale,readCenter(renderer,scale,3205),extractionExpected);
        const auto extractedLight=colorGradeResolve(renderer,extraction,3206,width,height,26,4);
        const auto extractedPixels=colorGradeRead(renderer,device,context,extractedLight,scale);
        require(extractedPixels.size()==size_t(width*scale)*height*scale*4,"GUI extracted-light extent differs");
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x)for(unsigned c=0;c<3;++c) {
            const auto actual=extractedPixels[(size_t(y)*width*scale+x)*4+c];
            const auto expected=cyan->pixels[(size_t(y/scale)*width+x/scale)*4+c];
            require(std::abs(int(actual)-int(expected))<=2,"GUI extraction changed the source RGB under a full alpha mask");
            require(actual>0,"GUI destination-alpha mask no longer feeds the light filter");
        }

        auto restored=colorGradeDraw(geometry,3207,width,height);
        restored.material=WorldMaterial::fixed;
        restored.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
        restored.textureObjects[0]=scene;restored.samplers[0]=colorGradeSampler(false,2);
        extractClear.targets=restored.targets;renderer.clear(extractClear);
        require(renderer.draw(restored),"GUI scene restoration rejected");
        auto additive=restored;additive.textureObjects[0]=extractedLight;
        put(additive.attributes.data()+92,0x01100008);
        additive.attributes[144]=2;additive.attributes[145]=2;
        require(renderer.draw(additive) && renderer.draw(additive),"GUI additive light passes rejected");
        auto amplified=cyanCenter;for(unsigned c=0;c<3;++c)amplified[c]*=3;
        amplified[3]=3;
        verify("two-additive-light-passes",scale,readCenter(renderer,scale,3207),amplified);
        const auto composedScene=colorGradeResolve(renderer,restored,3208,width,height,26,4);
        const auto identity=colorGradeCube(false);
        auto blackBloom=std::make_shared<ColorImage>();blackBloom->width=blackBloom->height=1;
        blackBloom->pixels={0,0,0,255};
        auto final=colorGradeDraw(geometry,3203,width,height);
        final.options.modes[1]=0;final.options.coordinates[1]=0;
        final.fragmentName="XREngine_Final5";final.fragmentFlags=14; // Actual exposure, glow and RGB-map stages.
        final.fragmentConstants[0]={1,1,1,1}; // Captured exposure vector.
        final.fragmentConstants[1]={0,0,1,1};
        final.textureObjects[0]=composedScene;final.samplers[0]=colorGradeSampler(true,2);
        final.textureObjects[1]=extractedLight;final.samplers[1]=colorGradeSampler(false,2);
        final.textures[2]=identity;final.samplers[2]=colorGradeSampler(true,2);
        WorldClear finalClear;finalClear.targets=final.targets;finalClear.viewport=final.viewport;finalClear.flags=1;
        renderer.clear(finalClear);require(renderer.draw(final),"Final5 color-map draw rejected");
        const auto output=colorGradeResolve(renderer,final,3204,width,height);
        const auto rgba=colorGradeRead(renderer,device,context,output,scale);
        const unsigned finalWidth=width*scale,finalX=3*scale+scale/2,finalY=3*scale+scale/2;
        const auto* finalPixel=rgba.data()+(size_t(finalY)*finalWidth+finalX)*4;
        // Independently model Final5's screen combination, exposure and gamma
        // transfer. The identity cube has exact RGB entries spaced by 15/255.
        auto finalOracle=[](const std::array<double,4>& scene,const std::array<double,4>& bloom) {
            std::array<uint8_t,3> output{};
            for(unsigned c=0;c<3;++c) {
                const double sceneLinear=scene[c]*scene[c],bloomLinear=bloom[c]*bloom[c];
                const double combined=sceneLinear+bloomLinear-std::clamp(sceneLinear*bloomLinear,0.0,1.0);
                const double linear=1-std::exp(-(std::max)(combined,1e-7));
                output[c]=uint8_t(std::lround(std::clamp(std::sqrt((std::max)(linear,1e-8)),0.0,1.0)*255));
            }
            return output;
        };
        const std::array<double,4> noBloom{};
        const auto amplifiedFinal=finalOracle(amplified,cyanCenter),sourceFinal=finalOracle(cyanCenter,noBloom);
        const std::array<uint8_t,3> transitionFinal{finalPixel[0],finalPixel[1],finalPixel[2]};
        for(unsigned c=0;c<3;++c)if(std::abs(int(transitionFinal[c])-int(amplifiedFinal[c]))>2) {
            std::fprintf(stderr,"GUIFadeFinal5[scale=%u channel=%u] actual=%u expected=%u\n",
                         scale,c,transitionFinal[c],amplifiedFinal[c]);
            throw std::runtime_error("Final5 output differs from the additive-light tone-map oracle");
        }
        require(std::abs(int(amplifiedFinal[1])-int(sourceFinal[1]))>10,
                "Final5 fixture does not distinguish a boosted transition from the settled background");

        // A separate fixed-texture pass represents the same settled background.
        // Verify both outputs against their independent tone-map expectations;
        // the lit transition must retain the additive contribution.
        auto background=colorGradeDraw(geometry,3211,width,height);
        background.material=WorldMaterial::fixed;
        background.fragmentName="MRenderXenon_Attrib_TexEnvMode01";
        background.textures[0]=cyan;background.samplers[0]=colorGradeSampler(false,2);
        WorldClear backgroundClear;backgroundClear.targets=background.targets;
        backgroundClear.viewport=background.viewport;backgroundClear.flags=1;
        renderer.clear(backgroundClear);require(renderer.draw(background),"Settled background draw rejected");
        const auto backgroundScene=colorGradeResolve(renderer,background,3212,width,height,26,4);
        auto backgroundFinal=final;backgroundFinal.targets[0]=3213;
        backgroundFinal.textureObjects[0]=backgroundScene;
        backgroundFinal.textureObjects[1]={};backgroundFinal.textures[1]=blackBloom;
        finalClear.targets=backgroundFinal.targets;
        renderer.clear(finalClear);require(renderer.draw(backgroundFinal),"Settled background Final5 draw rejected");
        const auto backgroundOutput=colorGradeResolve(renderer,backgroundFinal,3214,width,height);
        const auto backgroundRgba=colorGradeRead(renderer,device,context,backgroundOutput,scale);
        require(backgroundRgba.size()==rgba.size(),"Settled background output extent differs");
        for(unsigned y=0;y<height*scale;++y)for(unsigned x=0;x<width*scale;++x) {
            const auto source=sourcePixel(*cyan,x/scale,y/scale);
            auto lit=source;for(unsigned c=0;c<3;++c)lit[c]*=3;
            const auto unlitExpected=finalOracle(source,noBloom),litExpected=finalOracle(lit,source);
            const size_t pixel=(size_t(y)*width*scale+x)*4;
            for(unsigned c=0;c<3;++c) {
                require(std::abs(int(backgroundRgba[pixel+c])-int(unlitExpected[c]))<=2,
                        "Settled Final5 output differs from the unlit source tone-map oracle");
                require(std::abs(int(rgba[pixel+c])-int(litExpected[c]))<=2,
                        "Transition Final5 output differs from the additive source tone-map oracle");
            }
        }
        require(int(rgba[(size_t(finalY)*finalWidth+finalX)*4+1])>
                int(backgroundRgba[(size_t(finalY)*finalWidth+finalX)*4+1])+10,
                "GUI transition lost its visible additive light contribution");
        std::printf("GUIFadeFinal5 scale=%u: unlit-oracle RGB=%u,%u,%u; lit RGB=%u,%u,%u.\n",
                    scale,sourceFinal[0],sourceFinal[1],sourceFinal[2],transitionFinal[0],transitionFinal[1],transitionFinal[2]);

        std::printf("GUIFade scale=%u: source RGB, original alpha mask, nonzero extraction, additive light and Final5 tone maps passed.\n",scale);
    }
}
