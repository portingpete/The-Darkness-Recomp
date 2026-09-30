#pragma once
#include <fstream>
#include <iterator>

// Exercise actual screenshot I/O failures through the same nonfatal path used
// by F8, then render another frame and successfully capture it.
static void testPreviewShadowCaptureFailure(EnginePreviewD3D11& renderer) {
    const auto directory=std::filesystem::temp_directory_path()/
        ("DarkRecomp-shadow-screenshot-"+std::to_string(GetCurrentProcessId())+"-"+
         std::to_string(GetTickCount64()));
    require(std::filesystem::create_directory(directory),"Cannot create shadow screenshot fixture");
    auto white=std::make_shared<AlphaImage>();white->width=white->height=1;white->pixels={255};
    SimpleMesh triangle;triangle.texture=white;
    triangle.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    triangle.vertices={
        {{-.75f,-.75f,.5f},{0,0},{1,0,0,1}},
        {{0,.75f,.5f},{0,0},{1,0,0,1}},
        {{.75f,-.75f,.5f},{0,0},{1,0,0,1}}
    };
    triangle.indices={0,1,2};
    renderer.render({triangle});
    const auto original=renderer.readPixel(32,32);
    require(original==0xFF0000FF,"Shadow screenshot fixture did not render red");
    // A missing parent causes the real ofstream open/write path to fail after
    // GPU readback; the exception must not escape into the fatal render loop.
    require(!renderer.trySaveShadowCapture(directory/"missing"/"inspection-1.bmp"),
            "Shadow screenshot with an unwritable destination reported success");
    require(renderer.readPixel(32,32)==original && !renderer.frameInProgress(),
            "Failed shadow screenshot changed the completed frame");
    for(auto& vertex:triangle.vertices) {
        vertex.color[0]=vertex.color[2]=0;vertex.color[1]=vertex.color[3]=1;
    }
    renderer.render({triangle});renderer.copyToDisplay();
    require(renderer.readPixel(32,32)==0xFF00FF00,"Rendering did not continue after capture failure");
    const auto saved=directory/"inspection-2.bmp";
    require(renderer.trySaveShadowCapture(saved),"Next shadow screenshot did not recover after I/O failure");
    auto read=[](const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input),{});
    };
    const auto bytes=read(saved);
    require(bytes.size()>54 && bytes[0]=='B' && bytes[1]=='M',"Recovered shadow screenshot is not a BMP");
    renderer.render({});
    require(!renderer.trySaveShadowCapture(saved) && read(saved)==bytes,
            "Repeated shadow screenshot overwrote existing evidence");
    require(renderer.readPixel(32,32)==0xFF000000,"Rejected overwrite changed rendering");
    std::filesystem::remove(saved);std::filesystem::remove(directory);
    std::puts("ShadowScreenshot: nonfatal I/O failure, continued rendering, recovery and overwrite protection passed.");
}
