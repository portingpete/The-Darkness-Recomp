#pragma once

namespace {
void testTexturePreloadQueue() {
    using namespace DarkRecomp;
    using namespace DarkRecomp::Native;
    enableEnginePreview();
    std::shared_ptr<const ColorImage> next;
    while (takePreviewTexturePreload(next)) next.reset();
    const auto before = previewTexturePreloadCounters();
    auto makeImage = [](uint8_t red) {
        auto image = std::make_shared<ColorImage>();
        image->width = image->height = 1;
        image->authoredMips = true;
        image->pixels = {red, 0, 0, 255};
        return image;
    };

    auto original = makeImage(10), replacement = makeImage(20);
    previewPublishTexture(50001, 1, original);
    previewPublishTexture(50001, 2, replacement);
    check(takePreviewTexturePreload(next) && next == original,
        "Replacing a CPU texture changed an already queued immutable generation");
    next.reset();
    check(takePreviewTexturePreload(next) && next == replacement && next->pixels[0] == 20,
        "Preload queue lost the replacement texture generation");
    next.reset();
    previewPrepareTexture(50001);
    check(!takePreviewTexturePreload(next) && !next, "Empty preload queue retained an output");

    auto expired = makeImage(30);
    std::weak_ptr<const ColorImage> observer = expired;
    previewPublishTexture(50002, 3, expired);
    previewPrepareTexture(50002);
    expired.reset();
    check(observer.expired(), "Preload backlog retained an evicted CPU image");
    check(takePreviewTexturePreload(next) && !next, "Expired preload entry was dereferenced");

    auto generated = makeImage(40);
    generated->authoredMips = false;
    previewPublishTexture(50003, 4, generated);
    auto sparse = std::make_shared<ColorImage>();
    sparse->width = sparse->height = 2;
    sparse->authoredMips = true;
    sparse->firstMip = 1;
    sparse->mips = {{50, 0, 0, 255}};
    check(sparse->valid(), "Sparse preload fixture invalid");
    previewPublishTexture(50004, 5, sparse);
    check(!takePreviewTexturePreload(next), "Preloading changed generated or sparse mip upload paths");
    previewPrepareTexture(50003);
    previewPrepareTexture(50004);

    auto oldest = makeImage(60), retained = makeImage(70);
    previewPublishTexture(50005, 6, oldest);
    for (unsigned i = 0; i < 4096; ++i) previewPublishTexture(51000 + i, 7, retained);
    const auto full = previewTexturePreloadCounters();
    check(full.pending == 4096 && full.overflow == before.overflow + 1,
        "Preload backlog exceeded its bound or failed to retire its oldest entry");
    unsigned taken = 0;
    while (takePreviewTexturePreload(next)) {
        check(next == retained, "Overflow discarded a newer texture instead of the oldest");
        ++taken;
    }
    next.reset();
    check(taken == 4096 && previewTexturePreloadCounters().pending == 0,
        "Bounded preload backlog could not drain");
    previewPrepareTexture(50005);
    for (unsigned i = 0; i < 4096; ++i) previewPrepareTexture(51000 + i);
    const auto after = previewTexturePreloadCounters();
    check(after.expired == before.expired + 1 && after.taken == before.taken + 4098,
        "Preload accounting confused expired and owned entries");
    puts("Texture preloading preserves immutable generations, weak ownership, mip paths and bounded progress.");
}
}
