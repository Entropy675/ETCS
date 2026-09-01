// RenderFrameRateLoader.cc
//
// Frame-rate harness for RenderProvider::Surface. Measures four workloads
// against the same window and reports per-frame timing, so the numbers say
// which part of a frame costs what rather than just "N fps":
//
//   clear            acquire + empty render pass + submit + present. The
//                    floor: what a frame costs before anything is drawn.
//   rects            clear + 64 solid rects. Pipeline bind, push constants,
//                    64 draws of 4 vertices.
//   blit-cached      clear + one full-canvas layer blit, layer NEVER
//                    touched. The steady state of a canvas nobody is
//                    painting on: the upload is skipped entirely.
//   blit-dirty       same, but the layer is marked dirty every frame, so
//                    every frame re-uploads the whole thing.
//
// The last pair is the one worth reading: their difference IS what the
// dirty flag buys a 2D editor, and it scales with canvas size, which is why
// the layer dimensions are an argument.
//
// WHAT THESE NUMBERS ARE NOT. The swapchain uses FIFO present mode, which
// on real hardware blocks until vblank -- so on a GPU with a display this
// measures the refresh rate and little else, and any workload that fits in
// a frame reads the same. The interesting readings come from where it
// CANNOT hit refresh: a software rasterizer (Xvfb + Mesa lavapipe, how this
// is exercised in CI) has no vblank to wait for and is slow enough that the
// workloads separate. Treat the output as relative cost between workloads
// on one machine, never as an absolute frame rate for the runtime.
//
//   Xvfb :99 -screen 0 1024x768x24 &
//   DISPLAY=:99 ./bin/Run_RenderFrameRateLoader [frames] [layer_w] [layer_h]
//
// Run from the ETCS root -- the shader path is cwd-relative, same
// convention run_website.etcs uses for ./www.

#include "../ETCS.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Timing
{
    double mean_ms   = 0.0;
    double min_ms    = 0.0;
    double max_ms    = 0.0;
    double p50_ms    = 0.0;
    double p95_ms    = 0.0;
    double p99_ms    = 0.0;
    double fps       = 0.0;
    double total_ms  = 0.0;
};

// Percentiles off the sorted sample rather than a running estimate: the
// sample counts here are small enough to sort, and a p99 from 300 frames is
// only meaningful if it is a real observation.
Timing summarize(std::vector<double> samples)
{
    Timing t;
    if (samples.empty()) return t;
    std::sort(samples.begin(), samples.end());

    double sum = 0.0;
    for (double s : samples) sum += s;

    auto pct = [&](double p) -> double
    {
        size_t idx = static_cast<size_t>(p * (samples.size() - 1) + 0.5);
        return samples[idx];
    };

    t.total_ms = sum;
    t.mean_ms  = sum / samples.size();
    t.min_ms   = samples.front();
    t.max_ms   = samples.back();
    t.p50_ms   = pct(0.50);
    t.p95_ms   = pct(0.95);
    t.p99_ms   = pct(0.99);
    t.fps      = (t.mean_ms > 0.0) ? 1000.0 / t.mean_ms : 0.0;
    return t;
}

void print_row(const char* label, const Timing& t)
{
    std::cout << "  " << std::left << std::setw(13) << label << std::right << std::fixed
              << std::setw(9) << std::setprecision(1) << t.fps      << " fps"
              << std::setw(9) << std::setprecision(3) << t.mean_ms  << " mean"
              << std::setw(9) << t.p50_ms << " p50"
              << std::setw(9) << t.p95_ms << " p95"
              << std::setw(9) << t.p99_ms << " p99"
              << std::setw(9) << t.min_ms << " min"
              << std::setw(9) << t.max_ms << " max   (ms)\n";
}

} // namespace

int main(int argc, char** argv)
{
    const uint32_t frames  = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 300;
    const uint32_t layer_w = (argc > 2) ? static_cast<uint32_t>(std::atoi(argv[2])) : 512;
    const uint32_t layer_h = (argc > 3) ? static_cast<uint32_t>(std::atoi(argv[3])) : 512;
    const uint32_t warmup  = 20;   // discarded: first frames pay swapchain
                                    // acquire warm-up and the first upload

    WIRE_CONTEXT();

    ETCS::Entity* window = ETCS::spawn_entity("WindowProvider", "Window", env, loader);
    if (!window) { std::cerr << "Failed to load WindowProvider:Window\n"; return 1; }
    window->call("Window.Create", "800 600 RenderProvider FrameRate", ctx);

    ETCS::Entity* instance = ETCS::spawn_entity("RenderProvider", "Instance", env, loader);
    if (!instance) { std::cerr << "Failed to load RenderProvider:Instance\n"; return 1; }
    instance->call("Instance.Create", "", ctx);

    ETCS::Entity* surface = ETCS::make_typed_child("RenderProvider", "Surface", window, loader);
    if (!surface) { std::cerr << "Failed to attach RenderProvider:Surface\n"; return 1; }
    surface->call("Surface.Create",
                   (std::to_string(instance->getRID()) + " modules/RenderProvider/shaders/").c_str(), ctx);

    ETCS::Entity* layer = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
    if (!layer) { std::cerr << "Failed to spawn RenderProvider:ImageSurface\n"; return 1; }
    layer->call("ImageSurface.Create", (std::to_string(layer_w) + " " + std::to_string(layer_h)).c_str(), ctx);
    layer->call("ImageSurface.Clear", "0.15 0.18 0.35 1.0", ctx);
    layer->call("ImageSurface.DrawRect", "16 16 128 96 0.9 0.75 0.15 1.0", ctx);

    // The layer's own Pixels_, reached the generic way -- the harness needs
    // it to mark the buffer dirty for the blit-dirty phase without going
    // back through a work function, which is the same access a
    // PintaProvider layer editor would use.
    Pixels_* px = static_cast<Pixels_*>(layer->getInterfacePointer(ETCS::Buffer("Pixels")));
    if (!px) { std::cerr << "Layer exposes no Pixels interface\n"; return 1; }

    const std::string blitArgs = std::to_string(layer->getRID()) + " 0 0 800 600 1.0";

    std::cout << "RenderProvider frame rate -- " << frames << " frames per workload after "
              << warmup << " warmup, layer " << layer_w << "x" << layer_h << "\n";

    // One phase. `body` issues the frame's draw calls; Present is timed
    // around, since that is where acquire/record/submit/present all happen.
    auto run_phase = [&](const char* label, auto&& body) -> Timing
    {
        for (uint32_t i = 0; i < warmup; ++i) { body(); surface->call("Surface.Present", "", ctx); }

        std::vector<double> samples;
        samples.reserve(frames);
        for (uint32_t i = 0; i < frames; ++i)
        {
            body();
            auto t0 = std::chrono::steady_clock::now();
            surface->call("Surface.Present", "", ctx);
            auto t1 = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        Timing t = summarize(std::move(samples));
        print_row(label, t);
        return t;
    };

    std::cout << "\n";
    run_phase("clear", [&]
    {
        surface->call("Surface.Clear", "0.05 0.05 0.08 1.0", ctx);
    });

    run_phase("rects", [&]
    {
        surface->call("Surface.Clear", "0.05 0.05 0.08 1.0", ctx);
        for (int i = 0; i < 64; ++i)
        {
            const std::string r = std::to_string((i % 8) * 90) + " " + std::to_string((i / 8) * 70)
                                + " 80 60 0.85 0.2 0.2 1.0";
            surface->call("Surface.DrawRect", r.c_str(), ctx);
        }
    });

    const Timing cached = run_phase("blit-cached", [&]
    {
        surface->call("Surface.Clear", "0.05 0.05 0.08 1.0", ctx);
        surface->call("Surface.Blit", blitArgs.c_str(), ctx);
    });

    const Timing dirty = run_phase("blit-dirty", [&]
    {
        px->MarkDirty();   // forces a full re-upload of the layer this frame
        surface->call("Surface.Clear", "0.05 0.05 0.08 1.0", ctx);
        surface->call("Surface.Blit", blitArgs.c_str(), ctx);
    });

    const double upload_ms = dirty.mean_ms - cached.mean_ms;
    const double bytes     = static_cast<double>(layer_w) * layer_h * 4.0;
    std::cout << "\n  upload cost: " << std::fixed << std::setprecision(3) << upload_ms
              << " ms/frame for " << std::setprecision(1) << (bytes / 1024.0)
              << " KiB (blit-dirty - blit-cached)";
    if (upload_ms > 0.0)
        std::cout << " = " << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) / (upload_ms / 1000.0)
                  << " MiB/s";
    std::cout << "\n  -- that difference is what the dirty flag saves a canvas nobody is painting on.\n";

    layer->call("ImageSurface.Delete", "", ctx);
    surface->call("Surface.Delete", "", ctx);
    instance->call("Instance.Delete", "", ctx);
    window->call("Window.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::cout << "\nFrame rate test complete.\n";
    return 0;
}
