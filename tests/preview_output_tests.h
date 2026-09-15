static void testPreviewOutputResize(DarkRecomp::EnginePreviewD3D11& renderer,
                                    DarkRecomp::CDisplayContextD3D11& display) {
    DarkRecomp::SimpleMesh mesh;
    mesh.projection = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    mesh.vertices = {
        {{-1,-1,.5f},{0,1},{1,1,1,1}}, {{1,-1,.5f},{1,1},{1,1,1,1}},
        {{1,1,.5f},{1,0},{1,1,1,1}}, {{-1,1,.5f},{0,0},{1,1,1,1}},
    };
    mesh.indices = {0,1,2,0,2,3}; mesh.opaque = true;
    auto red = std::make_shared<DarkRecomp::ColorImage>();
    red->width = red->height = 1; red->pixels = {255,0,0,255};
    mesh.colorTexture = red;
    for (bool wide : {true, false}) {
        const uint32_t w = wide ? 128 : 64, h = wide ? 64 : 128;
        renderer.releaseDisplayTarget(); display.Resize(w, h);
        renderer.render({mesh}); renderer.copyToDisplay();
        ComPtr<ID3D11Texture2D> back, staging;
        require(SUCCEEDED(display.GetSwapChain()->GetBuffer(0, IID_PPV_ARGS(&back))), "Output readback buffer missing");
        D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
        desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(display.GetDevice()->CreateTexture2D(&desc, nullptr, &staging)), "Output staging allocation failed");
        display.GetContext()->CopyResource(staging.Get(), back.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(display.GetContext()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Output map failed");
        bool correct = true;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                const bool content = wide ? (x >= 32 && x < 96) : (y >= 32 && y < 96);
                correct &= pixel[0] == (content ? 255 : 0) && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
            }
        display.GetContext()->Unmap(staging.Get(), 0);
        require(correct, "Resized output stretched, cropped, or left stale border pixels");
    }
    renderer.releaseDisplayTarget(); display.Resize(64, 64);
    renderer.render({mesh}); renderer.copyToDisplay();
    uint32_t pixel = 0;
    require(display.ReadbackCenterPixel(pixel) && pixel == 0xFF0000FF, "Output did not restore direct copy after resize");
    auto video = std::make_shared<DarkRecomp::VideoFrame>();
    video->width = 32; video->height = 18;
    video->luma.assign(32*18, 235); video->chroma.assign(16*9*2, 128);
    mesh.colorTexture.reset(); mesh.video = video;
    renderer.render({mesh});
    require((renderer.readPixel(32,32) & 0xFF) > 240 &&
            renderer.readPixel(32,4) == 0xFF000000 && renderer.readPixel(32,60) == 0xFF000000,
            "Full-screen video did not preserve its original aspect");
    // A following ordinary draw must recover the full output viewport.
    auto overlay = mesh; overlay.video.reset(); overlay.colorTexture = red;
    renderer.render({mesh, overlay});
    require(renderer.readPixel(32,4) == 0xFF0000FF, "Video fit leaked into the following UI draw");
    // An embedded video is already positioned by the game and must stay there.
    for (auto& vertex : mesh.vertices) vertex.position[0] *= .5f;
    renderer.render({mesh});
    require((renderer.readPixel(32,4) & 0xFF) > 240, "Embedded video was fitted as a full-screen movie");
    std::puts("PreviewOutput passed: exact GPU pixels for wide/tall fits, border clearing, resource reuse and restored direct copy.");
}
