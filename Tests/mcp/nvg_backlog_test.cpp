// Unit tests for the nanovg async resource backlog compaction (host leak, 2026-11-24 OOM: PlugData grew ~400 MB/s
// while a clock-driven patch repainted). Libraries/nanovg/src/nanovg_async.h, patches/submodules/nanovg/0001.
//
// Every published frame moves its image ops (create/update with a FULL pixel copy, delete, framebuffer passes) into
// Context::committedResources; only the render thread's performRender releases them. When the render thread stops
// taking frames, nothing did: one pixel copy per repaint stayed on the heap. compactResourceBacklog keeps the backlog
// bounded by the live images WITHOUT changing what the render thread ends up with.
//
// Header-only: the test never links the real NanoVG (no GL). Build + run: scripts/test-bridge-safety.sh
#include "../../Libraries/nanovg/src/nanovg_async.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using Op = nanovg::Context::ResourceOp;
using Kind = Op::Kind;

// An image op whose payload is `bytes` long; a create is a square whose width * height * bytes-per-pixel is `bytes`
// (bytes: 4 * side * side for ARGB, side * side for alpha), as the real nvgCreateImage* / nvgUpdateImage make them.
static Op image(Kind kind, int id, size_t bytes)
{
    Op op;
    op.kind = kind;
    op.image = id;
    size_t const pixels = kind == Kind::CreateAlpha ? bytes : bytes / 4;
    int side = 0;
    while (static_cast<size_t>(side) * static_cast<size_t>(side) < pixels)
        ++side;
    op.width = side;
    op.height = side;
    op.data.assign(bytes, 0x7f);
    return op;
}

static Op pass()
{
    Op op;
    op.kind = Kind::FramebufferPass;
    op.pass = std::make_unique<nanovg::CommandBuffer>(256);
    op.pass->putOp(nanovg::Op::EndFrame);
    return op;
}

static size_t total(std::vector<Op> const& ops)
{
    size_t bytes = 0;
    for (auto const& op : ops)
        bytes += nanovg::resourceOpBytes(op);
    return bytes;
}

// The leak shape: a stalled consumer, a producer that re-uploads the same image every repaint.
static void repeatedUpdatesKeepOnlyTheLast()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateARGB, 7, 64));
    for (int frame = 0; frame < 1000; ++frame) {
        ops.push_back(image(Kind::Update, 7, 64));
        ops.back().data[0] = static_cast<unsigned char>(frame);
        nanovg::compactResourceBacklog(ops); // what publish() does each frame while the consumer is behind
    }
    // The update is folded into the create: one op that creates the texture with the newest pixels (so the backend
    // builds its mipmaps from them; a create without pixels would leave the mip levels undefined).
    CHECK(ops.size() == 1);
    CHECK(ops[0].kind == Kind::CreateARGB && ops[0].data.size() == 64);
    CHECK(ops[0].data[0] == static_cast<unsigned char>(999)); // the newest pixels win
    CHECK(total(ops) == 64);
}

// An image cache that creates a new image per repaint and evicts old ones (content-hashed drawImage, clipToImageAlpha).
static void createThenDeleteVanishes()
{
    std::vector<Op> ops;
    int nextId = 0x40000000;
    for (int frame = 0; frame < 500; ++frame) {
        int const id = nextId++;
        ops.push_back(image(Kind::CreateARGB, id, 1024));
        ops.push_back(image(Kind::Delete, id, 0));
        auto const result = nanovg::compactResourceBacklog(ops);
        CHECK(result.orphanedImages.size() == 1 && result.orphanedImages[0] == id);
    }
    CHECK(ops.empty());
    CHECK(total(ops) == 0);
}

// A delete of an image that already exists on the real context must survive; its pending update goes.
static void deleteOfAnOlderImageStays()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::Update, 3, 64));
    ops.push_back(image(Kind::Delete, 3, 0));
    auto const result = nanovg::compactResourceBacklog(ops);
    CHECK(ops.size() == 1 && ops[0].kind == Kind::Delete && ops[0].image == 3);
    CHECK(result.orphanedImages.empty());
    CHECK(result.droppedOps == 1 && result.droppedBytes == 64);
}

// A framebuffer pass replays drawing that may sample any image: nothing is merged across it.
static void passIsABoundary()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateARGB, 9, 64));
    ops.push_back(image(Kind::Update, 9, 64));
    ops.back().data[0] = 0x11;
    ops.push_back(pass());
    ops.push_back(image(Kind::Update, 9, 64));
    ops.push_back(image(Kind::Delete, 9, 0));
    nanovg::compactResourceBacklog(ops);
    // Before the pass: the create, holding the update's pixels (the pass may sample them). After: the update is
    // useless (deleted), the delete stays (its create is on the other side of the pass, so the real texture will exist).
    CHECK(ops.size() == 3);
    CHECK(ops[0].kind == Kind::CreateARGB && ops[0].data.size() == 64 && ops[0].data[0] == 0x11);
    CHECK(ops[1].kind == Kind::FramebufferPass);
    CHECK(ops[2].kind == Kind::Delete);
}

// Order of the survivors is kept, other images are untouched, empty updates go.
static void orderAndOtherImagesKept()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateAlpha, 1, 16));
    ops.push_back(image(Kind::CreateARGBSRGB, 2, 64));
    ops.push_back(image(Kind::Update, 5, 0)); // no pixels: applyResources does nothing with it
    Op fbCreate;
    fbCreate.kind = Kind::CreateFramebuffer;
    ops.push_back(std::move(fbCreate));
    nanovg::compactResourceBacklog(ops);
    CHECK(ops.size() == 3);
    CHECK(ops[0].kind == Kind::CreateAlpha && ops[0].image == 1 && ops[0].data.size() == 16);
    CHECK(ops[1].kind == Kind::CreateARGBSRGB && ops[1].image == 2 && ops[1].data.size() == 64);
    CHECK(ops[2].kind == Kind::CreateFramebuffer);
}

// Bounded: a long stalled run with a fixed set of live images never grows past one create + one update each.
static void stalledRunIsBounded()
{
    std::vector<Op> ops;
    for (int id = 0; id < 8; ++id)
        ops.push_back(image(Kind::CreateARGB, id, 256));
    size_t peak = 0;
    int churn = 1000;
    for (int frame = 0; frame < 2000; ++frame) {
        ops.push_back(image(Kind::Update, frame % 8, 256));
        ops.push_back(image(Kind::CreateARGB, churn, 4096)); // a per-repaint temporary image
        ops.push_back(image(Kind::Delete, churn, 0));
        ++churn;
        nanovg::compactResourceBacklog(ops);
        peak = std::max(peak, total(ops));
    }
    CHECK(ops.size() == 8); // one create per live image, each holding its newest pixels
    CHECK(peak <= 8 * 256 + 4096 + 256); // never more than the live images (+ one frame of new ops)
}

// Review 2026-11-24 (must-fix): the first version emptied the create and kept the update. The GL backend generates
// mipmaps only in nvg__renderCreateTexture, so a NVG_IMAGE_GENERATE_MIPMAPS image ([pic], NVGImage::MipMap) reloaded
// while the consumer was behind got undefined mip levels. The create must keep pixels: the newest ones, with its flags.
static void mipmappedCreateKeepsPixels()
{
    constexpr int generateMipmaps = 1 << 0; // NVG_IMAGE_GENERATE_MIPMAPS
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateARGB, 21, 64));
    ops.back().flags = generateMipmaps;
    ops.push_back(image(Kind::Update, 21, 64));
    ops.back().data[5] = 0x42;
    auto const result = nanovg::compactResourceBacklog(ops);
    CHECK(ops.size() == 1);
    CHECK(ops[0].kind == Kind::CreateARGB && ops[0].flags == generateMipmaps && ops[0].width == 4 && ops[0].height == 4);
    CHECK(ops[0].data.size() == 64 && ops[0].data[5] == 0x42);
    CHECK(result.droppedOps == 1 && result.droppedBytes == 64);
    CHECK(result.orphanedImages.empty());
}

// A create that came without pixels (nvgCreateImage*(..., nullptr)) takes the update's pixels the same way; an alpha
// image counts one byte per pixel.
static void emptyAndAlphaCreatesTakeTheUpdate()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateARGB, 30, 64));
    ops.back().data.clear();
    ops.push_back(image(Kind::Update, 30, 64));
    ops.push_back(image(Kind::CreateAlpha, 31, 16));
    ops.push_back(image(Kind::Update, 31, 16));
    ops.back().data[0] = 0x09;
    nanovg::compactResourceBacklog(ops);
    CHECK(ops.size() == 2);
    CHECK(ops[0].kind == Kind::CreateARGB && ops[0].image == 30 && ops[0].data.size() == 64);
    CHECK(ops[1].kind == Kind::CreateAlpha && ops[1].image == 31 && ops[1].data.size() == 16 && ops[1].data[0] == 0x09);
}

// An update whose size is not the create's (it cannot happen through nvgUpdateImage, which copies width * height *
// bytes-per-pixel; stay safe) is never folded: the create keeps its own pixels, later updates supersede each other.
static void oddSizedUpdateIsNotFolded()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateARGB, 40, 64));
    ops.back().data[0] = 0x01;
    ops.push_back(image(Kind::Update, 40, 32));
    ops.push_back(image(Kind::Update, 40, 32));
    ops.back().data[0] = 0x02;
    nanovg::compactResourceBacklog(ops);
    CHECK(ops.size() == 2);
    CHECK(ops[0].kind == Kind::CreateARGB && ops[0].data.size() == 64 && ops[0].data[0] == 0x01);
    CHECK(ops[1].kind == Kind::Update && ops[1].data.size() == 32 && ops[1].data[0] == 0x02);
}

// Compacting twice changes nothing more, and a create + update + delete of one image leaves nothing behind.
static void idempotentAndFullyOrphaned()
{
    std::vector<Op> ops;
    ops.push_back(image(Kind::CreateARGB, 50, 64));
    ops.push_back(image(Kind::Update, 50, 64));
    ops.push_back(image(Kind::Update, 51, 64)); // an image from before the backlog
    nanovg::compactResourceBacklog(ops);
    auto const again = nanovg::compactResourceBacklog(ops);
    CHECK(again.droppedOps == 0 && again.droppedBytes == 0);
    CHECK(ops.size() == 2 && ops[0].kind == Kind::CreateARGB && ops[1].kind == Kind::Update && ops[1].image == 51);
    ops.push_back(image(Kind::Update, 50, 64));
    ops.push_back(image(Kind::Delete, 50, 0));
    auto const gone = nanovg::compactResourceBacklog(ops);
    CHECK(gone.orphanedImages.size() == 1 && gone.orphanedImages[0] == 50);
    CHECK(ops.size() == 1 && ops[0].kind == Kind::Update && ops[0].image == 51);
}

int main()
{
    repeatedUpdatesKeepOnlyTheLast();
    createThenDeleteVanishes();
    deleteOfAnOlderImageStays();
    passIsABoundary();
    orderAndOtherImagesKept();
    stalledRunIsBounded();
    mipmappedCreateKeepsPixels();
    emptyAndAlphaCreatesTakeTheUpdate();
    oddSizedUpdateIsNotFolded();
    idempotentAndFullyOrphaned();
    if (failures) {
        std::printf("nvg_backlog_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("nvg_backlog_test: ok\n");
    return 0;
}
