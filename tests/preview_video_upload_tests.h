#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <vector>

static void testPreviewVideoUploads(EnginePreviewD3D11& renderer, CDisplayContextD3D11& display, const SimpleMesh& templateMesh) {
    using Microsoft::WRL::ComPtr;
    auto boundVideo = [&](ComPtr<ID3D11Resource>& yRes, ComPtr<ID3D11Resource>& uvRes,
                          ComPtr<ID3D11ShaderResourceView>& yView, ComPtr<ID3D11ShaderResourceView>& uvView) {
        ID3D11ShaderResourceView* views[3]{nullptr, nullptr, nullptr};
        display.GetContext()->PSGetShaderResources(0, 3, views);
        yView.Attach(views[0]);
        uvView.Attach(views[1]);
        if (views[2]) views[2]->Release();
        require(yView && uvView, "Video Y/UV views are not bound after video draw");
        yView->GetResource(&yRes);
        uvView->GetResource(&uvRes);
        require(yRes && uvRes, "Video Y/UV resources are missing");
    };
    auto renderVideo = [&](const std::shared_ptr<const VideoFrame>& frame) {
        SimpleMesh mesh = templateMesh;
        mesh.video = frame;
        renderer.render({mesh});
        return renderer.readPixel(32, 32);
    };
    auto red = std::make_shared<VideoFrame>();
    red->width = red->height = 2;
    red->luma = {81, 81, 81, 81};
    red->chroma = {240, 90};
    auto white = std::make_shared<VideoFrame>();
    white->width = white->height = 2;
    white->luma = {235, 235, 235, 235};
    white->chroma = {128, 128};
    uint32_t pixel = renderVideo(red);
    nearByte(pixel, 0, 254);
    nearByte(pixel, 1, 0);
    nearByte(pixel, 2, 0);
    nearByte(pixel, 3, 0);
    ComPtr<ID3D11Resource> yA, uvA;
    ComPtr<ID3D11ShaderResourceView> yViewA, uvViewA;
    boundVideo(yA, uvA, yViewA, uvViewA);
    pixel = renderVideo(white);
    nearByte(pixel, 0, 255);
    nearByte(pixel, 1, 254);
    nearByte(pixel, 2, 255);
    nearByte(pixel, 3, 0);
    ComPtr<ID3D11Resource> yB, uvB;
    ComPtr<ID3D11ShaderResourceView> yViewB, uvViewB;
    boundVideo(yB, uvB, yViewB, uvViewB);
    require(yA.Get() == yB.Get() && uvA.Get() == uvB.Get(), "Same-size video frame recreated GPU textures");
    require(yViewA.Get() == yViewB.Get() && uvViewA.Get() == uvViewB.Get(), "Same-size video frame recreated views");
    pixel = renderVideo(red);
    nearByte(pixel, 0, 254);
    nearByte(pixel, 1, 0);
    nearByte(pixel, 2, 0);
    nearByte(pixel, 3, 0);
    ComPtr<ID3D11Resource> yR, uvR;
    ComPtr<ID3D11ShaderResourceView> yViewR, uvViewR;
    boundVideo(yR, uvR, yViewR, uvViewR);
    require(yR.Get() == yA.Get() && uvR.Get() == uvA.Get(), "Retained video snapshot did not reuse GPU textures");
    auto gray = std::make_shared<VideoFrame>();
    gray->width = 4;
    gray->height = 2;
    gray->luma = {128, 128, 128, 128, 128, 128, 128, 128};
    gray->chroma = {128, 128, 128, 128};
    pixel = renderVideo(gray);
    nearByte(pixel, 3, 0);
    for (unsigned channel = 0; channel < 3; ++channel) {
        const int value = int((pixel >> (channel * 8)) & 255);
        require(value >= 126 && value <= 134, "Resized video frame sampled wrong level");
    }
    ComPtr<ID3D11Resource> yC, uvC;
    ComPtr<ID3D11ShaderResourceView> yViewC, uvViewC;
    boundVideo(yC, uvC, yViewC, uvViewC);
    require(yC.Get() != yA.Get() && uvC.Get() != uvA.Get(), "Dimension change did not recreate GPU textures");
    pixel = renderVideo(white);
    nearByte(pixel, 0, 255);
    nearByte(pixel, 1, 254);
    nearByte(pixel, 2, 255);
    nearByte(pixel, 3, 0);
    ComPtr<ID3D11Resource> yD, uvD;
    ComPtr<ID3D11ShaderResourceView> yViewD, uvViewD;
    boundVideo(yD, uvD, yViewD, uvViewD);
    require(yD.Get() != yC.Get() && uvD.Get() != uvC.Get(), "Shrinking video frame did not recreate GPU textures");
    auto striped = std::make_shared<VideoFrame>();
    striped->width = striped->height = 4;
    striped->luma = {16, 16, 16, 16, 81, 81, 81, 81, 145, 145, 145, 145, 235, 235, 235, 235};
    striped->chroma = {240, 90, 240, 90, 128, 128, 128, 128};
    pixel = renderVideo(striped);
    nearByte(pixel, 0, 203);
    nearByte(pixel, 1, 74);
    nearByte(pixel, 2, 76);
    nearByte(pixel, 3, 0);
    auto quad = [&](float x0, float x1, const std::shared_ptr<const VideoFrame>& frame) {
        SimpleMesh mesh;
        mesh.projection = templateMesh.projection;
        mesh.video = frame;
        mesh.vertices = {
            {{x0, 1, 0.5f}, {0, 0}, {1, 1, 1, 1}},
            {{x1, 1, 0.5f}, {1, 0}, {1, 1, 1, 1}},
            {{x1, -1, 0.5f}, {1, 1}, {1, 1, 1, 1}},
            {{x0, -1, 0.5f}, {0, 1}, {1, 1, 1, 1}},
        };
        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    };
    SimpleMesh leftRed = quad(-1, 0, red), rightWhite = quad(0, 1, white);
    renderer.render({leftRed, rightWhite});
    uint32_t left = renderer.readPixel(16, 32), right = renderer.readPixel(48, 32);
    nearByte(left, 0, 254);
    nearByte(left, 1, 0);
    nearByte(left, 2, 0);
    nearByte(left, 3, 0);
    nearByte(right, 0, 255);
    nearByte(right, 1, 254);
    nearByte(right, 2, 255);
    nearByte(right, 3, 0);
    ComPtr<ID3D11Resource> yS, uvS;
    ComPtr<ID3D11ShaderResourceView> yViewS, uvViewS;
    boundVideo(yS, uvS, yViewS, uvViewS);
    SimpleMesh leftWhite = quad(-1, 0, white), rightRed = quad(0, 1, red);
    renderer.render({leftWhite, rightRed});
    left = renderer.readPixel(16, 32);
    right = renderer.readPixel(48, 32);
    nearByte(left, 0, 255);
    nearByte(left, 1, 254);
    nearByte(left, 2, 255);
    nearByte(left, 3, 0);
    nearByte(right, 0, 254);
    nearByte(right, 1, 0);
    nearByte(right, 2, 0);
    nearByte(right, 3, 0);
    ComPtr<ID3D11Resource> yT, uvT;
    ComPtr<ID3D11ShaderResourceView> yViewT, uvViewT;
    boundVideo(yT, uvT, yViewT, uvViewT);
    require(yT.Get() == yS.Get() && uvT.Get() == uvS.Get(), "Same-frame sequential draws recreated GPU textures");
    auto rejects = [&](std::shared_ptr<VideoFrame> bad) {
        SimpleMesh mesh = templateMesh;
        mesh.video = bad;
        try {
            renderer.render({mesh});
        } catch (const std::invalid_argument&) {
            return;
        }
        throw std::runtime_error("Malformed video frame was accepted");
    };
    auto empty = std::make_shared<VideoFrame>();
    empty->width = empty->height = 2;
    rejects(empty);
    auto odd = std::make_shared<VideoFrame>();
    odd->width = 3;
    odd->height = 2;
    odd->luma = std::vector<uint8_t>(6, 128);
    odd->chroma = std::vector<uint8_t>(3, 128);
    rejects(odd);
    auto shortLuma = std::make_shared<VideoFrame>(*red);
    shortLuma->luma.pop_back();
    rejects(shortLuma);
    auto shortChroma = std::make_shared<VideoFrame>(*red);
    shortChroma->chroma.pop_back();
    rejects(shortChroma);
    pixel = renderVideo(red);
    nearByte(pixel, 0, 254);
    nearByte(pixel, 3, 0);
    std::puts("PreviewVideoUploads passed: same-size pixel update, texture/view reuse, resize recreate, snapshot restore, disjoint draw order, patterned pitch/VU oracle, malformed reject.");
}
