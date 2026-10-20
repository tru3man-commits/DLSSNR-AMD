Warning: truncated output (original token count: 69248)
Total output lines: 5053

#include "nr_pe_session.hpp"
#include "nr_guide_region.hpp"
#include "nr_pe_bridge.hpp"
#include "nr_pe_d3d12split.hpp"
#include "nr_ini_network.hpp"
#include "nr_pe_config.hpp"   // NetworkSwitch
#include "nr_pe_log.hpp"
#include "nr_pe_vkdevice.hpp"
#include "nr_pipeline_binary.hpp"
#include "nr_log.hpp"
#include "nr_screenshot.hpp"
#include <windows.h>
#include <d3d10.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace nr::pe {
namespace {

using nr::pe::log;

// What the process has left of its own address space.
//
// This exists because of one log line: `neural rendering unavailable:
// std::bad_alloc`, from a 32-bit game at 4K with 11979 MB of VRAM free, 482 ms
// into a build - and nothing else. A `std::bad_alloc` is `operator new`
// refusing, which in a 32-bit process almost always means the 2 GB of virtual
// address space is gone, not that the machine is out of memory; a Vulkan
// failure would have come back through NRVK_CHECK with a VkResult instead. The
// two numbers that tell those apart are the total free and the **largest
// contiguous free block** - a 300 MB request fails against 900 MB of free space
// scattered in 60 MB pieces, and only the second number shows it.
//
// A 64-bit process has terabytes and this reads as such; the walk costs one
// VirtualQuery per region either way, and it is only ever called on a failure.
std::string address_space_report() {
    MEMORY_BASIC_INFORMATION region{};
    unsigned long long free_total = 0, largest = 0;
    auto* address = static_cast<unsigned char*>(nullptr);
    while (VirtualQuery(address, &region, sizeof region) == sizeof region) {
        if (region.State == MEM_FREE) {
            free_total += region.RegionSize;
            if (region.RegionSize > largest) largest = region.RegionSize;
        }
        auto* next = static_cast<unsigned char*>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;   // no progress, or the pointer wrapped
        address = next;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    GlobalMemoryStatusEx(&memory);
    char text[256];
    std::snprintf(text, sizeof text,
                  "address space: %.0f MB free in this process, largest contiguous block %.0f MB "
                  "(of %.0f MB total); %.0f MB of RAM free",
                  double(free_total) / (1 << 20), double(largest) / (1 << 20),
                  double(memory.ullTotalVirtual) / (1 << 20),
                  double(memory.ullAvailPhys) / (1 << 20));
    return text;
}

// What the card and the machine have left, in bytes.
//
// **Both, because on Windows the network is charged twice.** WDDM backs every
// video-memory allocation with system commit (RAM plus page file), so the 3.9
// GB arena of a 3818x1998 build takes 3.9 GB of VRAM budget *and* 3.9 GB of
// commit. A tester's machine ran out of the second with VRAM to spare (package
// test 4: 43286 of 43391 MB committed, the game holding 20.8 GB): DXVK's
// vkEndCommandBuffer failed, Dalamud wrote through a null pointer and the
// desktop lost its icons. Earlier "out of VRAM" failures - DXVK refused 108 MB
// with 2.3 GB of budget left, pipelines failed with OUT_OF_HOST_MEMORY - were
// the same thing.
struct MemoryState {
    uint64_t vram_used{}, vram_budget{}, vram_free{};   // largest device-local heap
    uint64_t commit_free{}, commit_limit{};             // this process's view of system commit
    bool vram_known{};
};

MemoryState read_memory(VkPhysicalDevice physical) {
    MemoryState m;
    if (physical) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        props.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physical, &props);
        // The largest device-local heap's free space, by budget where the driver
        // reports one and by heap size where it does not.
        const auto& mem = props.memoryProperties;
        for (uint32_t i = 0; i < mem.memoryHeapCount; ++i) {
            if (!(mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
            const uint64_t cap = budget.heapBudget[i] ? budget.heapBudget[i] : mem.memoryHeaps[i].size;
            const uint64_t used = budget.heapUsage[i];
            const uint64_t free = cap > used ? cap - used : 0;
            if (!m.vram_known || free > m.vram_free) {
                m.vram_free = free; m.vram_used = used; m.vram_budget = cap; m.vram_known = true;
            }
        }
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) {
        m.commit_free = memory.ullAvailPageFile;
        m.commit_limit = memory.ullTotalPageFile;
    }
    return m;
}

double mb(uint64_t bytes) { return double(bytes) / (1 << 20); }

// Headroom left for the game after a build, and the levels at which a running
// network steps aside. The gap between the two is the hysteresis that keeps a
// build from being made and dropped on alternate seconds.
constexpr uint64_t kVramReserve = 1024ull << 20, kCommitReserve = 3072ull << 20;
constexpr uint64_t kVramLow = 384ull << 20, kCommitLow = 1536ull << 20;

}  // namespace

// What the bridge needs to know about one D3D12 frame, whichever route handed it over.
struct BridgeInputs {
    ID3D12Resource* target{};
    D3D12_RESOURCE_STATES target_state{};
    // Where the colour is read from when it is not `target` itself (same format and extent).
    ID3D12Resource* source{};
    D3D12_RESOURCE_STATES source_state{};
    ID3D12Resource* motion{};
    D3D12_RESOURCE_STATES motion_state{};
    ID3D12Resource* depth{};
    D3D12_RESOURCE_STATES depth_state{};
    Session::Subrect motion_subrect{}, depth_subrect{};
    float motion_scale_x{1.0f}, motion_scale_y{1.0f};
    bool depth_inverted{true};
    bool reset{};
    uint64_t feature{};
    bool linear{};   // the colour is scene-referred linear light (encoded by the runtime)
    // The RenoDX-style option: its processing around the network instead (Session::run_renodx), with the
    // game's exposure texture when the route has one.
    const Session::RenoDXFrame* renodx{};
    ID3D12Resource* exposure{};
    D3D12_RESOURCE_STATES exposure_state{};
};

// The RenoDX-style option on the bridge: the stage recorded in the job in place of the network, on the shared
// images (in place: the copy out takes its answer).
struct BridgeRenoDX {
    renodx::Stage* stage{};
    Controls controls{};
    const Session::RenoDXFrame* frame{};
    bool exposure{};   // the shared exposure image holds the game's exposure texture
};

struct Session::Impl {
    std::string root;
    std::string status;
    // ---- one built network per extent ----------------------------------------
    //
    // There used to be exactly one, rebuilt whenever anything about it changed.
    // OptiScaler keeps neural-rendering features alive at *two* extents at once
    // - pre-upscale at the render resolution and post-upscale at the display
    // one - and creates and destroys them on every toggle, so a single slot
    // thrashed: 1920x1080, then 3840x2160, then 1920x1080 again, a full rebuild
    // each time, with frames passing through unenhanced in between. That is the
    // stall the user reported and it is also why "passes" appeared to do
    // nothing: the network was never finished long enough to run.
    //
    // So built networks are kept, keyed by everything that makes one different,
    // and a switch between two of them is a pointer assignment. They are
    // evicted least-recently-used, and only when the card cannot hold the next
    // one (fits_in_memory) - the arena is 1.05 GB at 1080p and 4.07 GB at 4K, so
    // this is not a cache that can be allowed to grow.
    struct Built {
        std::unique_ptr<Runtime> runtime;
        uint32_t width{}, height{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        bool linear{};
        float scale{1.0f};
        // The pass ceiling this one was built for: it sizes one history image
        // per pass, so a network built for four cannot serve a request for six.
        uint32_t passes{4};
        bool prep{};   // RuntimeConfig::preprocess
        bool native{}; // RuntimeConfig::native_compose
        int net{};     // the network it runs (nr::NetworkChoice)
        uint64_t used{};   // LRU stamp
        std::chrono::steady_clock::time_point last{};   // when it was last selected
        bool light{};      // built inside a recording (inframe): its images in its network memory's heap
    };
    // A cached network taken up again after serving nothing for this long starts its features fresh (as after a
    // Reset), so no frame is blended with a history that old; RenoDX's own look history uses the same bound.
    static constexpr std::chrono::milliseconds kStaleHistory{250};
    std::vector<Built> cache;
    uint64_t use_stamp{};
    // Networks of one [Int4Mixed] kind kept at once (see the adoption in ensure_runtime_), and runtimes: those built
    // from another's network (Runtime::share_for, an extent with the same working extent) hold their own images only.
    static constexpr size_t kMaxResident = 2;
    static constexpr size_t kMaxEntries = 4;
    // 0.5 s: OptiScaler's two extents (NR before and after the upscaler) are both used every frame; a game's dynamic
    // resolution leaves sizes behind several times a second (2 s kept 13 networks, 7.2 GB).
    static constexpr std::chrono::milliseconds kIdleDrop{500};
    static constexpr std::chrono::seconds kNetIdle{5};
    // A whole build (another working extent) starts only for an extent asked for kSettle times, at most kSettleGap
    // evaluations apart: a size that moves on every frame (a game's dynamic resolution) asked for one build per frame,
    // never ready before the size had moved again (608 builds in 200 s, a CPU core busy). Counted in evaluations,
    // not time: a game at 5 fps is as entitled to its network as one at 200.
    // ---- the resize path inside the frame --------------------------------------------------------------------
    // Plans (Runtime::plan): one per working extent and configuration, lowered at the tallest frame height of that
    // working extent so it runs every height of it. With one in the cache and a resident network whose memory holds
    // it, a frame at a new size gets its runtime inside the recording (RuntimeConfig::deferred, ~1 ms) and runs the
    // same frame, as NVIDIA's DLL evaluates one feature at any size. A plan is built in the background (~0.15 s) the
    // first time its working extent is asked for.
    struct PlanEntry {
        VkFormat format{}; bool linear{}; float scale{}; uint32_t passes{}; bool prep{}, native{}; int net{};
        uint32_t ww{}, wh{}, sh{};   // working extent, tallest frame height
        std::shared_ptr<const RuntimeCore> core;
        uint64_t used{};
    };
    std::vector<PlanEntry> plans;
    static constexpr size_t kMaxPlans = 48;
    bool build_plan{};   // the background build is a plan (RuntimeConfig::plan_only)
    uint64_t inframe_built{}, plans_built{};
    double inframe_ms_max{};
    bool same_config_but_format(const Built& e, bool want_linear) const { return same_config(e, e.format, want_linear); }
    bool same_config(const Built& e, VkFormat f, bool want_linear) const {
        if (e.format != f || e.linear != want_linear || e.scale != model_scale || e.passes != max_passes_want ||
            e.prep != prep_want || e.native != (native_compose || renodx_native))
            return false;
        return e.net == net_want;
    }
    // The network a runtime runs (nr::NetworkChoice).
    static int net_of(const Runtime& r) {
#if NR_INT4
        if (r.int4_mixed()) return nr::kSilverAardwoldInt4;
#endif
        return r.network() == NetworkModel::Small ? nr::kSmallFp8 : nr::kSilverAardwold;
    }
    RuntimeConfig make_config(uint32_t w, uint32_t h, VkFormat f, bool want_linear) const;
    HostDevice make_host() const;
    TemporalConfig make_temporal() const {
        TemporalConfig temporal; temporal.enable = true;
        temporal.history_strength = history_strength;
        return temporal;
    }
    const PlanEntry* plan_for(uint32_t w, uint32_t h, VkFormat f, bool want_linear, NetworkModel model);
    // `tried`: the network it would have run on, when building inside the frame failed (a pipeline not made yet):
    // the background build then makes that runtime.
    bool inframe(uint32_t w, uint32_t h, VkFormat f, bool want_linear, std::shared_ptr<const RuntimeCore>* tried);
    bool start_plan(uint32_t w, uint32_t h, VkFormat f, bool want_linear);
    // While a game's dynamic resolution moves the frame size (a resize in the last kDrsActive): the plans of the
    // working extents nearest the latest size, one at a time in the background, so a sweep finds them made.
    static constexpr std::chrono::seconds kDrsActive{2};
    std::chrono::steady_clock::time_point last_resize{};
    uint32_t last_w{}, last_h{};
    VkFormat last_format{VK_FORMAT_UNDEFINED};
    bool last_linear{};
    std::pair<uint32_t, uint32_t> prefetched_around{};   // the size whose neighbourhood is all planned
    bool drs_active() const {
        return last_resize != std::chrono::steady_clock::time_point{} &&
               std::chrono::steady_clock::now() - last_resize < kDrsActive;
    }
    void prefetch();
    // A whole network while the size moves gets this much room by area (1.25 linear): the plans of a sweep up to that
    // run in it instead of each asking for a bigger network.
    static constexpr double kDrsCapacity = 1.5625;
    // The game's guides' allocation (pixels), when the frame has them: a game with dynamic resolution allocates them
    // once at its biggest render size and renders into a corner. A whole network built while the size moves gets
    // room for that (else kDrsCapacity), so one network and one image heap cover the whole range.
    uint64_t size_hint_px{};
    // [Network] Resize History (dlssnr-amd.ini): 0 (the default) a feature at a new size starts its history afresh, as
    // NVIDIA's DLL; 1 its history is carried over, scaled to the new size (Runtime::carry_history). Re-read at most once
    // a second, on a resize.
    bool resize_history{};
    std::chrono::steady_clock::time_point resize_history_read{};
    // The runtime each feature ran on last, and the one before it (the size before a resize).
    std::map<uint64_t, Runtime*> feature_runtime, feature_prev_runtime;
    uint64_t carried{};
    // The last RenoDX-style frame's decisions (renodx::Stage::facts), for the menus' engine-data panel.
    renodx::Stage::Facts renodx_facts{};
    bool renodx_facts_known{};
    bool carry(VkCommandBuffer cmd, uint64_t feature);
    // The feature being evaluated, and the network memory each feature last ran in: a new frame size of a feature
    // takes its memory again when it fits (OptiScaler's NR before and after the upscaler keep one each, and one
    // stream does not move between memories).
    uint64_t current_feature{};
    std::map<uint64_t, const void*> feature_slot;
    // The image heap being made in the background for a network memory (Runtime::image_heap_wanted).
    std::thread heap_thread;
    std::mutex heap_lock;
    bool heap_busy{}, heap_done{};
    const void* heap_for{};
    std::shared_ptr<ImageHeapRef> heap_made;
    void heap_poll();
    // Runtimes built inside frames with images of their own (no heap yet) kept at most this many.
    static constexpr size_t kOwnImages = 2;
    bool inframe_waiting{};
    static constexpr uint32_t kSettle = 3;
    static constexpr uint64_t kSettleGap = 8;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint64_t>> asked;   // extent -> count, last call
    bool settled(uint32_t w, uint32_t h) {
        for (auto it = asked.begin(); it != asked.end();)
            it = calls - it->second.second > kSettleGap ? asked.erase(it) : std::next(it);
        auto& e = asked[{w, h}];
        e.first += 1; e.second = calls;
        return e.first >= kSettle;
    }
    void trim_cache(const Built* adopting = nullptr);
    // Evaluations that ran the network and that passed the frame through (building, freeing, failed): in the log
    // every kPassLog while any pass through, and for the whole session at its end.
    uint64_t ran{}, passed{}, ran_total{}, passed_total{};
    std::chrono::steady_clock::time_point pass_logged{};
    static constexpr std::chrono::seconds kPassLog{10};
    // ---- [Network] Residency: the networks not running ------------------------------------------------------------
    // A network of another kind than the running one (and, with no evaluation for kNrOff, the running one too: NR off)
    // is inactive. resident keeps them; offload moves their weights to RAM and minimal frees them; auto keeps them
    // while the device-local heap has room:
    // free = min(budget, heap) - usage under max(1 GiB, 10 % of the heap) frees the least recently used one (one a
    // check; the budget lags a free, so kFreedQuiet after one); at a switch the left network stays only with 1.5x that
    // free; above 70 % of the budget an inactive one stays at most 60 s, down to 1 s at 100 %. The running network is
    // never freed while NR runs.
    static constexpr std::chrono::seconds kNrOff{10};
    static constexpr std::chrono::milliseconds kResidencyCheck{500};
    static constexpr std::chrono::seconds kFreedQuiet{1};
    std::chrono::steady_clock::time_point last_call{}, residency_checked{}, residency_freed{};
    struct Vram { uint64_t heap{}, budget{}, usage{}; };
    bool vram(Vram& v) const;   // the largest device-local heap
    void residency(bool switched, bool nr_off);
    // The activation arena a whole build of network `net` may take (Runtime::act_arena): one of a network of another
    // kind - the feature's last one first, else the biggest - never one a network of `net` runs in (two networks of
    // one kind can run in one frame: OptiScaler's NR before and after the upscaler). A new arena gets kActSlack room
    // so the other kinds fit it (CC_SMALL_FP8 needs 4.6 % more than CC_SILVER_AARDWOLD at 1080p).
    static constexpr double kActSlack = 1.06;
    std::shared_ptr<ActArenaRef> act_for(int net) const;
    // A network kept as its memory alone (auto): its weights, its share of an activation arena and its pipelines, held
    // by a core (Runtime::share_for); its runtimes and images are freed. Asked for again, a runtime is built from it in
    // the background (RuntimeConfig::share, ~10 ms) instead of a whole build.
    struct Parked {
        std::shared_ptr<const RuntimeCore> core;
        VkFormat format{}; bool linear{}; float scale{}; uint32_t passes{}; bool prep{}, native{}; int net{};
        uint32_t width{}, height{};
        std::chrono::steady_clock::time_point last{};
    };
    std::vector<Parked> parked;
    // Park entry `index`'s network (once per network) and drop every runtime of it.
    void park(size_t index, const char* why);
    // A parked network's weights moved to RAM (offload_network) on a thread of its own, one at a time, once no runtime
    // of it is left (retired ones included: their frames may be in flight). Polled by residency().
    std::thread offload_thread;
    std::atomic<bool> offload_done{false};
    bool offload_ok{};
    std::shared_ptr<const RuntimeCore> offloading;
    std::vector<uint8_t> offload_weights;   // the read-back, released into the network by poll_offload
    // The parked network a background build makes a runtime from (a failed build drops it: its memory may be half back).
    std::shared_ptr<const RuntimeCore> build_share;
    bool start_offload(Parked& p);
    // The parked network a background build is bringing back from RAM (reload_network writes its memory on that
    // thread): not looked at until the build is adopted.
    bool reloading(const Parked& p) const { return building && p.core == build_share; }
    void poll_offload();
    // RAM below max(2 GiB, 10 %) available: offloaded networks are freed, the oldest first.
    static bool ram_short();
    // The RenoDX-style option: one stage per feature, made at the frame's extent, format and HDR judgement.
    // A replaced or released one is kept for kRetireCalls more evaluations and kRetireTime, as networks are.
    struct RenoDXEntry {
        std::unique_ptr<renodx::Stage> stage;
        uint32_t w{}, h{}, nw{}, nh{}, present{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        bool hdr{};
        // RenoDX's CommitNrResolution: the network's extent in use, and one the game's guides moved to waiting
        // out kContractDebounceFrames (a settings change is taken at once).
        uint32_t applied_w{}, applied_h{}, pending_w{}, pending_h{}, pending_frames{};
        float scale{-1.0f};
        bool follow{};
        uint32_t stack{1};   // the model passes its stack last ran: pass k > 0 keeps a history of its own
    };
    std::map<uint64_t, RenoDXEntry> renodx;
    // The history pass k of a feature's stack runs on (pass 0: the feature's own).
    static uint64_t stack_feature(uint64_t feature, uint32_t k) {
        return k == 0 ? feature : (uint64_t(1) << 62) | (feature << 8) | k;
    }
    // Pass 1's name on a route whose frames carry none (feature 0), when it stacks; no game feature is numbered so.
    static constexpr uint64_t kUnnamedStack = 0xFFFFFFu;
    void release_everywhere(uint64_t feature) {
        for (auto& e : cache) e.runtime->release_feature(feature);
    }
    // RenoDX's neutral guides (NRPresentGuides): zero motion vectors at the frame's extent, for a Present-path frame
    // without the game's (Optional) or always (Never). Made and cleared on first use, kept.
    VkImage neutral_mv{};
    VkDeviceMemory neutral_mv_memory{};
    uint32_t neutral_w{}, neutral_h{};
    bool neutral_cleared{};
    bool neutral_motion(VkCommandBuffer cmd, uint32_t w, uint32_t h, ColourFrame* out);
    // Whether a RenoDX frame of this route runs without the game's motion vectors (the Present path's Optional and
    // Never; Required, and every other route, need them).
    static bool renodx_needs_motion(const Session::RenoDXFrame& rf) {
        return !rf.present || rf.settings.present_guides == 1u;
    }
    // The RenoDX-style option's cost for NR Control and the log: a timestamp pair around the whole stage and one
    // around each pass's network, in the same frame, a ring of frames read back without waiting, averaged as the
    // runtime averages its own (the runtime's own timing covers one pass at a time).
    static constexpr uint32_t kStageSlots = 8, kStageQueries = 2 + 2 * renodx::kMaxUnlockedPasses;
    VkQueryPool stage_timing{};
    bool stage_timing_tried{};
    double stage_timestamp_ns{};
    uint32_t stage_slot{};
    uint32_t stage_slot_passes[kStageSlots]{};
    uint64_t stage_recorded{};
    float stage_ms_avg{}, stage_net_ms_avg{};
    bool renodx_timed{};
    void read_stage_timing() {
        if (stage_recorded <= kStageSlots) return;
        const uint32_t n = 2 + 2 * stage_slot_passes[stage_slot];
        uint64_t stamps[kStageQueries] = {};
        if (vkGetQueryPoolResults(handles.device, stage_timing, stage_slot * kStageQueries, n, n * sizeof(uint64_t), stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || stamps[1] <= stamps[0])
            return;   // still running: next frame
        uint64_t net = 0;
        for (uint32_t p = 0; p < stage_slot_passes[stage_slot]; ++p)
            if (stamps[3 + 2 * p] > stamps[2 + 2 * p]) net += stamps[3 + 2 * p] - stamps[2 + 2 * p];
        const float ms = float(double(stamps[1] - stamps[0]) * stage_timestamp_ns / 1e6);
        const float net_ms = float(double(net) * stage_timestamp_ns / 1e6);
        const bool first = stage_ms_avg <= 0.0f;
        stage_ms_avg = first ? ms : stage_ms_avg + (ms - stage_ms_avg) * 0.1f;
        stage_net_ms_avg = first ? net_ms : stage_net_ms_avg + (net_ms - stage_net_ms_avg) * 0.1f;
    }
    bool stage_timing_ready() {
        if (stage_timing_tried) return stage_timing != VK_NULL_HANDLE;
        stage_timing_tried = true;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(handles.physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(handles.physical, &n, families.data());
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(handles.physical, &props);
        if (access.family >= n || !families[access.family].timestampValidBits || props.limits.timestampPeriod <= 0.0f) return false;
        stage_timestamp_ns = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kStageSlots * kStageQueries;
        if (vkCreateQueryPool(handles.device, &qi, nullptr, &stage_timing) != VK_SUCCESS) stage_timing = VK_NULL_HANDLE;
        return stage_timing != VK_NULL_HANDLE;
    }
    // RenoDX's NR screenshot (nr_screenshot.hpp): one capture's two readbacks in one host-visible buffer (the image
    // before NR at 0, after it at `bytes`), filled by copies recorded in the frame's own command buffer; an event set
    // after them is the proof the GPU ran them (an event starts unset; a new query pool's queries are undefined until
    // the command buffer resetting them has run, and RADV reads them as available). Handed to the worker once it has,
    // freed once the worker has read it; a capture never proven run is kept until the session goes (its command buffer
    // may still run).
    struct Shot {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        uint64_t bytes{};
        VkEvent done{};
        screenshot::Pair pair;
        uint32_t frames{};
        bool handed{};
        std::atomic_bool read{false};
    };
    static constexpr uint32_t kShotSettleFrames = 240;   // RenoDX's kExactSettlePresents
    std::unique_ptr<Shot> shot;                          // recorded, waiting for the GPU
    std::vector<std::unique_ptr<Shot>> shots_out;        // with the worker, or kept
    void shot_free(Shot& sh) {
        if (sh.mapped) vkUnmapMemory(handles.device, sh.memory);
        if (sh.buffer) vkDestroyBuffer(handles.device, sh.buffer, nullptr);
        if (sh.memory) vkFreeMemory(handles.device, sh.memory, nullptr);
        if (sh.done) vkDestroyEvent(handles.device, sh.done, nullptr);
        sh.mapped = nullptr; sh.buffer = VK_NULL_HANDLE; sh.memory = VK_NULL_HANDLE; sh.done = VK_NULL_HANDLE;
    }
    // Readbacks for a w x h pair of `format`, stamped (nr_screenshot.hpp); null when anything fails.
    std::unique_ptr<Shot> shot_prepare(uint32_t w, uint32_t h, VkFormat format) {
        auto sh = std::make_unique<Shot>();
        sh->bytes = uint64_t(w) * h * screenshot::bytes_per_pixel(format);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = sh->bytes * 2; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkMemoryRequirements req{};
        VkPhysicalDeviceMemoryProperties mem{};
        uint32_t type = ~0u;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        if (!sh->bytes || vkCreateBuffer(handles.device, &bi, nullptr, &sh->buffer) != VK_SUCCESS) goto fail;
        vkGetBufferMemoryRequirements(handles.device, sh->buffer, &req);
        vkGetPhysicalDeviceMemoryProperties(handles.physical, &mem);
        for (int pass = 0; pass < 2 && type == ~0u; ++pass)   // host-cached first: the worker reads it all
            for (uint32_t i = 0; i < mem.memoryTypeCount && type == ~0u; ++i) {
                const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits & (1u << i)) && (f & want) == want &&
                    (pass == 1 || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)))
                    type = i;
            }
        if (type == ~0u) goto fail;
        {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size; ai.memoryTypeIndex = type;
            if (vkAllocateMemory(handles.device, &ai, nullptr, &sh->memory) != VK_SUCCESS ||
                vkBindBufferMemory(handles.device, sh->buffer, sh->memory, 0) != VK_SUCCESS ||
                vkMapMemory(handles.device, sh->memory, 0, VK_WHOLE_SIZE, 0, &sh->mapped) != VK_SUCCESS) {
                sh->mapped = nullptr;
                goto fail;
            }
        }
        if (vkCreateEvent(handles.device, &ei, nullptr, &sh->done) != VK_SUCCESS) goto fail;
        screenshot::stamp(sh->mapped, sh->bytes);
        screenshot::stamp(static_cast<uint8_t*>(sh->mapped) + sh->bytes, sh->bytes);
        sh->pair.width = w; sh->pair.height = h; sh->pair.format = format;
        sh->pair.row_pitch = w * screenshot::bytes_per_pixel(format);
        sh->pair.bytes = sh->bytes;
        sh->pair.pre = sh->mapped;
        sh->pair.post = static_cast<uint8_t*>(sh->mapped) + sh->bytes;
        return sh;
    fail:
        shot_free(*sh);
        return nullptr;
    }
    // `image` (in `layout`, last used by `stage` / `access`) into the readback at `offset`, and back to `layout`.
    static void shot_copy(VkCommandBuffer cmd, Shot& sh, VkImage image, VkImageLayout layout, VkPipelineStageFlags stage,
                          VkAccessFlags access, VkDeviceSize offset) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = access; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {sh.pair.width, sh.pair.height, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sh.buffer, 1, &region);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = layout;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, stage | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
    }
    // Both copies recorded: the proof after them, and the capture is no longer armed.
    void shot_recorded(VkCommandBuffer cmd, std::unique_ptr<Shot> sh) {
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = sh->buffer; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        vkCmdSetEvent(cmd, sh->done, VK_PIPELINE_STAGE_TRANSFER_BIT);
        screenshot::recorded();
        shot = std::move(sh);
    }
    // Once a frame: a recorded capture the GPU has run goes to the worker; one the worker has read is freed.
    void shot_poll() {
        // Freed: one the worker has read, and an abandoned one (never handed) whose copies the GPU has since run.
        for (auto it = shots_out.begin(); it != shots_out.end();)
            if ((*it)->handed ? (*it)->read.load(std::memory_order_acquire)
                              : vkGetEventStatus(handles.device, (*it)->done) == VK_EVENT_SET) {
                shot_free(**it); it = shots_out.erase(it);
            } else ++it;
        if (!shot) return;
        if (vkGetEventStatus(handles.device, shot->done) == VK_EVENT_SET) {
            Shot* sh = shot.get();
            sh->handed = true;
            sh->pair.owner = sh;
            sh->pair.release = [](void* owner) { static_cast<Shot*>(owner)->read.store(true, std::memory_order_release); };
            screenshot::Pair pair = sh->pair;
            shots_out.push_back(std::move(shot));
            screenshot::deliver(std::move(pair));
        } else if (++shot->frames > kShotSettleFrames) {
            screenshot::abandon("gpu_unconfirmed", "the command buffer carrying the copies did not run within 240 frames "
                                "(buffers kept)");
            shots_out.push_back(std::move(shot));
        }
    }
    // NR_DIAG_EXPOSURE=1, a diagnostic (off by default): on the RenoDX-style option, texel (0,0) of the game's exposure
    // texture and 64 points (an 8 x 8 grid) of the frame before NR, the network's input and output (pass 1) and the
    // frame after NR, copied into a host-visible slot in the frame's own command buffer, with DLSS.Pre.Exposure,
    // DLSS.Exposure.Scale and the style. Logged once its event is set: every 30th frame, and every frame for 240 frames
    // after the style or the intensity changed. A slot whose commands never ran stays allocated.
    struct DiagSlot {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        VkEvent done{};
        bool pending{};
        uint64_t frame{};
        uint32_t style{};
        float intensity{}, pre{}, scale{};
        bool reset{};
        VkFormat format[5]{};
        bool have[5]{};
    };
    static constexpr uint32_t kDiagSlots = 16, kDiagPoints = 64, kDiagStride = 16;
    std::vector<DiagSlot> diag;
    DiagSlot* diag_slot = nullptr;   // this frame's, while it is being recorded
    uint64_t diag_frame = 0, diag_burst_until = 0;
    uint32_t diag_style = ~0u;
    float diag_intensity = -1.0f;
    static bool diag_on() {
        static const bool on = [] { const char* e = std::getenv("NR_DIAG_EXPOSURE"); return e && e[0] == '1'; }();
        return on;
    }
    static uint32_t diag_bytes(VkFormat f) {
        switch (f) {
            case VK_FORMAT_R16_SFLOAT: return 2;
            case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: return 4;
            case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
            case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
            default: return 0;
        }
    }
    // One texel as linear-looking RGB (8-bit and 10-bit formats: their codes over the maximum, not decoded).
    static bool diag_texel(VkFormat f, const uint8_t* p, float rgb[3]) {
        auto half = [](uint16_t h) {
            const uint32_t e = (h >> 10) & 31u, m = h & 1023u;
            const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.f : std::ldexp(float(m | 1024u), int(e) - 25);
            return (h & 0x8000u) ? -v : v;
        };
        auto ufloat = [](uint32_t bits, int mbits) {   // no sign, 5-bit exponent
            con…51248 tokens truncated…png=rgb8-srgb-clipped-preview"
                                "\nencoding=srgb\nunits=model-proxy-relative"
                                "\nstage=before-look-transfer-and-game-tonemap"
                                "\nexcluded=look,transfer,pedestal,game-upscale,game-tonemap,UI"
                                "\ninput_includes_previous_stack_passes=" + std::string(k ? "1" : "0") +
                                "\nstack_pass=" + std::to_string(k + 1) + "\nstack=" + std::to_string(passes) +
                                "\nwidth=" + std::to_string(proxy.width) + "\nheight=" + std::to_string(proxy.height) +
                                "\nsource_format=" + std::to_string(int(proxy.format)) + "\n";
                shot_copy(cmd, *sh, proxy.image, proxy.after, proxy.after_stage, proxy.after_access, 0);
                shot_copy(cmd, *sh, neural.image, neural.after, neural.after_stage, neural.after_access, sh->bytes);
                shot_recorded(cmd, std::move(sh));
            } else {
                screenshot::abandon("allocation_failed", "model diagnostic readback buffers could not be allocated");
            }
        }
    };
    if (timed) {
        vkCmdResetQueryPool(cmd, stage_timing, base, kStageQueries);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, stage_timing, base);
    }
    try {
        stage.record(cmd, sf, network);
        renodx_facts = stage.facts();
        renodx_facts_known = true;
        diag_copy(cmd, 4, output.image, output.format, output.width, output.height, output.usage, output.after,
                  output.after_stage, output.after_access);
        diag_end(cmd);
        if (capture) {
            shot_copy(cmd, *capture, output.image, output.after, output.after_stage, output.after_access, capture->bytes);
            shot_recorded(cmd, std::move(capture));
        }
        if (timed) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stage_timing, base + 1);
            stage_slot_passes[slot] = timed_passes;
            ++stage_recorded;
            stage_slot = (slot + 1) % kStageSlots;
            read_stage_timing();
        }
        renodx_timed = timed;
    } catch (const std::exception& x) {
        if (capture) {   // its pre-NR copy may be in the command buffer: kept until the session goes
            screenshot::abandon("failed", "RenoDX-style processing failed during the capture");
            shots_out.push_back(std::move(capture));
        }
        status = std::string("RenoDX-style processing failed: ") + x.what();
        failed = true;
        log("[nr] %s", status.c_str());
        return false;
    }
    return true;
}

bool Session::run_renodx(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                         ID3D12Resource* output, D3D12_RESOURCE_STATES output_state,
                         const EngineResources& resources, const Controls& controls,
                         ID3D12Resource* input, D3D12_RESOURCE_STATES input_state, const RenoDXFrame& rf) {
    auto& s = *impl_;
    s.current_feature = resources.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !device || !list || !output || !input) return false;
    if (!resources.motion && Impl::renodx_needs_motion(rf)) return false;
    s.status.clear();
    if (bridged(device)) {
        // Native D3D12: as run_after's bridge path - the list is cut at the evaluate, the input, guides and exposure
        // copied into the shared images, RenoDX's processing and the network run between the halves on our device,
        // the answer copied into the output.
        if (!s.ensure_bridge(device->GetAdapterLuid())) return false;
        BridgeInputs in{};
        in.target = output; in.target_state = output_state;
        if (input != output) { in.source = input; in.source_state = input_state; }
        in.motion = resources.motion; in.motion_state = resources.motion_state;
        in.depth = resources.depth; in.depth_state = resources.depth_state;
        in.motion_subrect = resources.motion_subrect; in.depth_subrect = resources.depth_subrect;
        in.motion_scale_x = resources.motion_scale_x; in.motion_scale_y = resources.motion_scale_y;
        in.depth_inverted = resources.depth_inverted;
        in.reset = resources.reset;
        in.feature = resources.feature;
        in.renodx = &rf;
        in.exposure = rf.exposure; in.exposure_state = rf.exposure_state;
        std::shared_ptr<split::Snapshot> state;
        std::string why;
        const auto before_copy = [&] {
            if (!split::prepare(list, &why)) { s.status = "the command list cannot be cut yet: " + why; return false; }
            state = split::snapshot(list);
            if (!state) { s.status = "the command list's state is not known"; return false; }
            return true;
        };
        bridge::Job* job = s.bridge_prepare_d3d12(device, list, in, controls, before_copy);
        if (!job) return false;
        bridge::Device* d = s.bdev;
        const uint64_t gen = d->generation(job);
        split::Job between;
        between.run = [d, job, gen](ID3D12CommandQueue* queue) { d->run_between(queue, job, gen); };
        between.discard = [d, job, gen] { d->discard(job, gen); };
        if (!split::cut(list, std::move(between), state, &why)) {
            d->discard(job, gen);
            split::restore(list, state);
            s.status = "the command list could not be cut: " + why;
            thread_local std::string said;
            if (said != s.status) { said = s.status; log("[nr] bridge: %s", s.status.c_str()); }
            return false;
        }
        s.record_copy_out(list, output, output_state);
        static bool first = true;
        if (first) {
            first = false;
            log("[nr] bridge: first RenoDX-style frame cut at the evaluate and handed to the network (%ux%u)",
                unsigned(output->GetDesc().Width), output->GetDesc().Height);
        }
        return true;
    }
    if (!s.ensure_d3d12_device(device)) return false;
    const VkCommandBuffer cmd = command_buffer(list);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, output, output_state);
    if (!target.usable()) { s.status = describe_rejection(target, "output"); return false; }
    const auto source = input == output ? target : colour_handle(device, input, input_state);
    if (!source.usable()) { s.status = describe_rejection(source, "colour"); return false; }
    if (source.format != target.format || source.width != target.width || source.height != target.height) {
        s.status = "RenoDX-style: the colour and the output differ in format or extent";
        return false;
    }
    const uint32_t w = target.width, h = target.height;
    const auto motion_vk = resources.motion ? resource_handle(device, resources.motion, resources.motion_state)
                                            : decltype(resource_handle(device, resources.motion, resources.motion_state)){};
    uint32_t nw = w, nh = h;
    s.renodx_extent(resources.feature, w, h,
                    resources.motion_subrect.width ? resources.motion_subrect.width : motion_vk.width,
                    resources.motion_subrect.height ? resources.motion_subrect.height : motion_vk.height,
                    rf.settings, &nw, &nh);
    Controls c;
    // NR off (NeuralUplift): the stage copies the frame through and never asks for the network - none is made or
    // kept in use for it, so [Network] Residency sees NR off
    if (rf.settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &c)) return false;
    renodx::Stage* stage = s.renodx_stage(resources.feature, w, h, nw, nh, target.format, rf);
    if (!stage) return false;
    ColourFrame colour{}, out{};
    fill(&colour, source);
    fill(&out, target);
    EngineFrame engine{};
    engine.feature = resources.feature;
    engine.reset = resources.reset;
    engine.motion_scale_x = resources.motion_scale_x;
    engine.motion_scale_y = resources.motion_scale_y;
    engine.depth_inverted = resources.depth_inverted;
    if (resources.motion) {
        if (!fill(&engine.motion, motion_vk)) return false;
        engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, resources.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (resources.depth &&
        fill(&engine.depth, resource_handle(device, resources.depth, resources.depth_state)))
        apply_guide_subrect(&engine.depth, resources.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
    // As run_after: full barriers on both sides, vkd3d's tracked compute state forgotten afterwards.
    const auto everything = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = src_access; mb.dstAccessMask = dst_access;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };
    ColourFrame exposure{};
    ResourceHandle exposure_vk = rf.exposure ? resource_handle(device, rf.exposure, rf.exposure_state) : ResourceHandle{};
    // An exposure is a float (DLSS's ExposureTexture, as OptiScaler reads it); a typeless one is read as its float
    // format. Safe for any VkFormat vkd3d-proton made it (R32_TYPELESS: R32_UINT): the stage copies its texel
    // (vkCmdCopyImage, same size class) into its own image of this format.
    bool exposure_typeless = false;
    if (exposure_vk.image && exposure_vk.format == VK_FORMAT_UNDEFINED) {
        switch (exposure_vk.dxgi) {
            case DXGI_FORMAT_R32_TYPELESS: exposure_vk.format = VK_FORMAT_R32_SFLOAT; break;
            case DXGI_FORMAT_R16_TYPELESS: exposure_vk.format = VK_FORMAT_R16_SFLOAT; break;
            case DXGI_FORMAT_R32G32B32A32_TYPELESS: exposure_vk.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
            default: break;
        }
        exposure_typeless = exposure_vk.format != VK_FORMAT_UNDEFINED;
    }
    const bool have_exposure = rf.exposure && fill(&exposure, exposure_vk);
    {   // what became of the game's exposure texture, once per outcome (the stage's feed line says only "none")
        static int logged = -1;
        const int outcome = !rf.exposure ? 0 : have_exposure ? 1 : 2;
        if (outcome != logged) {
            logged = outcome;
            if (outcome == 0) log("[nr] RenoDX stage: OptiScaler handed no exposure texture with this frame");
            else if (outcome == 1) log("[nr] RenoDX stage: the game's exposure texture read (DXGI format %u, %ux%u%s)",
                                       unsigned(exposure_vk.dxgi), exposure_vk.width, exposure_vk.height,
                                       exposure_typeless ? ", typeless: read as float" : "");
            else log("[nr] RenoDX stage: the game's exposure texture could not be read (DXGI format %u, %ux%u, %s)",
                     unsigned(exposure_vk.dxgi), exposure_vk.width, exposure_vk.height,
                     exposure_vk.image ? "a format not mapped" : "no Vulkan image");
        }
    }
    everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    if (!s.record_renodx(cmd, *stage, colour, out, engine, c, rf, have_exposure ? &exposure : nullptr)) return false;
    everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    list->SetPipelineState(nullptr);
    list->SetComputeRootSignature(nullptr);
    s.forget_bound_pipeline(device, list);
    return true;
}

bool Session::run_present(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                          ID3D12Resource* back_buffer, unsigned width, unsigned height,
                          const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !device || !list || !back_buffer) return false;
    s.status.clear();
    if (!s.ensure_d3d12_device(device)) return false;
    const VkCommandBuffer cmd = command_buffer(list);
    if (!cmd) { s.status = "the command list exposes no Vulkan handle"; s.failed = true; return false; }
    const auto target = colour_handle(device, back_buffer, D3D12_RESOURCE_STATE_PRESENT);
    if (!target.usable()) { s.status = describe_rejection(target, "back buffer"); return false; }
    if (!s.ensure_runtime(controls, target.width, target.height, target.format, false)) return false;

    // Here the network works on the game's own buffer: there is nothing to hand
    // anyone afterwards, the frame just has to come out enhanced.
    ColourFrame frame{};
    frame.image = target.image;
    frame.format = target.format;
    frame.width = target.width; frame.height = target.height;
    frame.before = frame.after = target.layout;
    try {
        TemporalFrame temporal{};
        s.runtime->record_temporal(cmd, frame, controls, temporal);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }
    return true;
}

bool Session::run_d3d11(const D3D11Frame& frame, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !frame.device || !frame.target) return false;
    s.status.clear();

    // The native D3D11 runtime: the bridge, on a Vulkan device of our own.
    if (!s.dxvk && s.mode != Impl::Mode::Interop) {
        IDXGIVkInteropDevice* probe = nullptr;
        const bool interop = d3d11_handles(frame.device, &probe).valid();
        if (probe) probe->Release();
        if (s.decide_mode(interop)) {
            ID3D11Device* device = nullptr;
            if (FAILED(frame.device->QueryInterface(IID_PPV_ARGS(&device))) || !device) {
                s.status = "the D3D11 device is not a D3D11 device";
                return false;
            }
            const bool ran = s.bridge_d3d11(device, frame, controls);
            device->Release();
            return ran;
        }
    }
    // DXVK's interop device is the whole of this path: it is where the Vulkan
    // handles, the submission queue and the queue lock all come from.
    if (!s.dxvk) {
        log("[nr] d3d11: asking DXVK for its Vulkan handles");
        s.handles = d3d11_handles(frame.device, &s.dxvk);
        log("[nr] d3d11: instance %p physical %p device %p interop %p",
            static_cast<void*>(s.handles.instance), static_cast<void*>(s.handles.physical),
            static_cast<void*>(s.handles.device), static_cast<void*>(s.dxvk));
        if (!s.handles.valid() || !s.dxvk) {
            if (s.dxvk) { s.dxvk->Release(); s.dxvk = nullptr; }
            s.status = "no Vulkan device underneath; this D3D11 game is not on DXVK";
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return false;
        }
        if (!d3d11_queue_access(s.dxvk, &s.access)) {
            s.status = "DXVK would not hand out its submission queue";
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return false;
        }
    }

    // Said once per target, not once per frame: this pair used to be two of the
    // fifteen flushed lines every frame wrote (see first_sight in
    // nr_pe_interop.cpp). What the target is only changes when it changes.
    static VkImage traced_target = VK_NULL_HANDLE;
    const auto target = d3d11_image(frame.target, frame.device);
    if (!target) {
        s.status = "the D3D11 texture exposes no Vulkan image";
        return false;
    }
    if (target.image != traced_target) {
        traced_target = target.image;
        log("[nr] d3d11: queue family %u; target %ux%u format %u layout %u usage 0x%x",
            s.access.family, target.width, target.height, unsigned(target.format),
            unsigned(target.layout), unsigned(target.usage));
    }
    // The pass transfers into and out of whatever image it is given. DXVK tells
    // us what the image may be used for, so this is a check rather than a hope -
    // and a texture without these bits gets declined with a reason instead of
    // producing validation errors inside somebody's game.
    constexpr VkImageUsageFlags required =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((target.usage & required) != required) {
        s.status = "the game's texture cannot be copied to and from; nothing to run on";
        return false;
    }
    Controls renodx_controls{};
    renodx::Stage* renodx_stage = nullptr;
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return false;
        }
        const auto mv = frame.motion ? d3d11_image(frame.motion, frame.device) : decltype(d3d11_image(frame.motion, frame.device)){};
        uint32_t nw = target.width, nh = target.height;
        s.renodx_extent(0, target.width, target.height, mv.width, mv.height, frame.renodx->settings, &nw, &nh);
        if (frame.renodx->settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &renodx_controls)) return false;
        if (!(renodx_stage = s.renodx_stage(0, target.width, target.height, nw, nh, target.format, *frame.renodx)))
            return false;
    } else if (!s.ensure_runtime(controls, target.width, target.height, target.format,
                                 (frame.upscaler_input || frame.linear_hdr) && !frame.estimate_motion &&
                                 Session::Impl::is_linear_format(target.format))) return false;
    if (!s.ensure_own_command_buffer()) return false;
    if (!s.wait_for_own_cmd()) return false;

    ColourFrame colour{};
    colour.image = target.image;
    colour.format = target.format;
    colour.width = target.width; colour.height = target.height;
    // Transition from the layout DXVK believes the image is in, and put it back:
    // the game's next use of its own texture must see what it left there.
    colour.before = colour.after = target.layout;
    // What DXVK says the image may be used for: with SAMPLED and STORAGE the runtime reads and
    // writes it in place at Model resolution below 100% instead of copying it out and back.
    colour.usage = target.usage;

    EngineFrame engine{};
    engine.colour = colour;
    engine.reset = frame.reset || s.resized(0, colour.width, colour.height);
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    const bool motion = frame.motion && fill(&engine.motion, d3d11_image(frame.motion, frame.device));
    if (frame.depth) fill(&engine.depth, d3d11_image(frame.depth, frame.device));

    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(s.own_cmd, 0);
        vkBeginCommandBuffer(s.own_cmd, &bi);
        if (renodx_stage) {
            if (!motion && Impl::renodx_needs_motion(*frame.renodx)) { vkEndCommandBuffer(s.own_cmd); return false; }
            if (!s.record_renodx(s.own_cmd, *renodx_stage, colour, colour, engine, renodx_controls, *frame.renodx)) {
                vkEndCommandBuffer(s.own_cmd);
                return false;
            }
        } else if (motion) s.runtime->record_engine(s.own_cmd, engine, controls);
        else if (frame.estimate_motion) {
            TemporalFrame temporal{}; temporal.reset = frame.reset;
            s.runtime->record_temporal(s.own_cmd, colour, controls, temporal);
        } else {
            s.runtime->record(s.own_cmd, colour, controls, frame.reset);
        }
        vkEndCommandBuffer(s.own_cmd);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }

    // Everything the game has recorded goes in front of us - that is what the
    // flush inside the lock is for - and then our submit happens with the queue
    // held, because DXVK shares one VkQueue with everything else in the process.
    VkResult r;
    {
        QueueAccess::Held held(s.access);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &s.own_cmd;
        r = vkQueueSubmit(s.access.queue, 1, &si, s.own_fence);
        if (r == VK_SUCCESS) s.own_cmd_pending = true;
    }
    if (r != VK_SUCCESS) {
        s.status = "the submit on the game's queue failed";
        return false;
    }
    return true;
}

bool Session::run_present_vulkan(const DeviceHandles& handles, VkQueue queue, uint32_t family,
                                 VkImage image, VkFormat format, uint32_t width, uint32_t height,
                                 uint32_t slot_index, uint32_t slot_count,
                                 const VkSemaphore* wait, uint32_t wait_count,
                                 VkSemaphore* signalled, bool temporal,
                                 const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = 0;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !image || !queue || !handles.valid() || !signalled) return false;
    s.status.clear();
    s.handles = handles;
    s.access.queue = queue;
    s.access.family = family;
    s.access.lock = nullptr;      // this queue is the game's and we are inside its present
    s.access.unlock = nullptr;

    // A game that alternates between two swapchain extents - 007 First Light
    // cycles 3840x2160 and 3840x2096 at startup and on every mode change - would
    // otherwise rebuild the whole network on each one: four gigabytes
    // reallocated, the weights re-uploaded, and the history thrown away. A
    // discarded history closes the temporal gate for that frame, which changes
    // the picture visibly, and doing it every other frame is exactly what a
    // flicker is.
    //
    // So an extent that does not match the built network does not rebuild it: it
    // is counted, the frame is left alone, and the rebuild happens only once the
    // new extent has actually settled. A real resolution change costs half a
    // second of untouched frames; an alternation costs nothing at all.
    if (s.runtime && (s.width != width || s.height != height) && !s.have_runtime(width, height)) {
        if (s.pending_w != width || s.pending_h != height) {
            s.pending_w = width; s.pending_h = height; s.pending_frames = 0;
        }
        if (++s.pending_frames < 30) return false;
        log("[nr] the swapchain has been %ux%u for %u frames; rebuilding from %ux%u",
            width, height, s.pending_frames, s.width, s.height);
    }
    s.pending_frames = 0;

    if (!s.ensure_slots(slot_count)) return false;
    if (slot_index >= s.slots.size()) return false;
    auto& slot = s.slots[slot_index];

    // Every slot, not just this one.
    //
    // The runtime's contract is explicit that the caller serialises uses of its
    // scratch arena, and the per-image slots defeat exactly that: with three
    // swapchain images, three of our command buffers can be executing at once,
    // all reading and writing one activation arena and one history image. The
    // result is frames that alternate between right and wrong, which is what a
    // flickering picture is.
    //
    // So the slots stay - a binary semaphore still needs one per image, and the
    // present depends on that - but only one pass is ever in flight. It costs a
    // pipeline bubble, which is the price of the arena being shared.
    for (auto& other : s.slots) if (!s.wait_slot(other)) return false;
    // Only after the slot is free: rebuilding the runtime destroys images this
    // slot's command buffer may still be reading.
    if (!s.ensure_runtime(controls, width, height, format, false)) return false;

    // A swapchain image at present time is in PRESENT_SRC. The pass transfers in
    // and out of it, and leaves it exactly as it found it: the present that
    // follows is entitled to the layout it was promised.
    ColourFrame colour{};
    colour.image = image;
    colour.format = format;
    colour.width = width; colour.height = height;
    colour.before = colour.after = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    static bool first = true;
    if (first) log("[nr] first fallback frame: %ux%u format %d on queue family %u, image %u of %u",
                   width, height, int(format), family, slot_index, slot_count);
    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(slot.cmd, 0);
        vkBeginCommandBuffer(slot.cmd, &bi);
        if (temporal) {
            TemporalFrame frame{};
            s.runtime->record_temporal(slot.cmd, colour, controls, frame);
        } else {
            s.runtime->record(slot.cmd, colour, controls);
        }
        vkEndCommandBuffer(slot.cmd);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return false;
    }

    // One wait stage per semaphore, as the spec requires.
    std::vector<VkPipelineStageFlags> stages(wait_count, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait_count;
    si.pWaitSemaphores = wait_count ? wait : nullptr;
    si.pWaitDstStageMask = wait_count ? stages.data() : nullptr;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &slot.done;
    if (vkQueueSubmit(queue, 1, &si, slot.fence) != VK_SUCCESS) {
        s.status = "the submit before present failed";
        return false;
    }
    slot.pending = true;
    *signalled = slot.done;
    if (first) { first = false; log("[nr] first fallback frame submitted"); }
    return true;
}

void Session::set_vulkan_queue(VkQueue queue, uint32_t family) {
    if (!queue) return;
    impl_->access.queue = queue;
    impl_->access.family = family;
    // No lock: this queue belongs to the game and nothing else in this process
    // shares it through an interop layer, so there is no mutex to take. The
    // caller records into the command buffer the upscaler handed over, which the
    // game submits itself; only the one-off weight upload submits here.
    impl_->access.lock = nullptr;
    impl_->access.unlock = nullptr;
}

VkImageView Session::vulkan_output_view() const { return impl_->vk_output_view; }

VkImage Session::run_vulkan(const DeviceHandles& handles, VkCommandBuffer cmd,
                            const VulkanFrame& frame, const Controls& controls) {
    auto& s = *impl_;
    s.current_feature = frame.feature;   // which network memory a new frame size prefers (Impl::inframe)
    if (s.failed || !cmd || !frame.colour || !handles.valid()) return VK_NULL_HANDLE;
    s.status.clear();
    s.handles = handles;

    // A Vulkan game gives us no D3D12 resource to hand back, so the output is a
    // plain VkImage of our own rather than one borrowed from a D3D12 allocation -
    // unless the caller takes the answer in its own image (VulkanFrame::in_place).
    Controls renodx_controls{};
    renodx::Stage* renodx_stage = nullptr;
    if (frame.renodx) {
        if (!frame.motion && Impl::renodx_needs_motion(*frame.renodx)) {
            s.status = "the RenoDX-style option needs the motion vectors";
            return VK_NULL_HANDLE;
        }
        uint32_t nw = frame.width, nh = frame.height;
        s.renodx_extent(frame.feature, frame.width, frame.height,
                        frame.motion_subrect.width ? frame.motion_subrect.width : frame.motion_width,
                        frame.motion_subrect.height ? frame.motion_subrect.height : frame.motion_height,
                        frame.renodx->settings, &nw, &nh);
        if (frame.renodx->settings.nr_enabled && !s.renodx_runtime(controls, nw, nh, &renodx_controls))
            return VK_NULL_HANDLE;
        if (!(renodx_stage = s.renodx_stage(frame.feature, frame.width, frame.height, nw, nh, frame.colour_format,
                                            *frame.renodx)))
            return VK_NULL_HANDLE;
    } else {
    if (!frame.in_place && !s.vk_output_valid(frame.width, frame.height, frame.colour_format)) {
        if (!s.ensure_vk_output(frame.width, frame.height, frame.colour_format))
            return VK_NULL_HANDLE;
    }
    s.size_hint_px = std::max(uint64_t(frame.motion ? frame.motion_width : 0) * (frame.motion ? frame.motion_height : 0),
                              uint64_t(frame.depth ? frame.depth_width : 0) * (frame.depth ? frame.depth_height : 0));
    if (!s.ensure_runtime(controls, frame.width, frame.height, frame.colour_format,
                          (frame.upscaler_input || frame.linear_hdr) &&
                              Session::Impl::is_linear_format(frame.colour_format)))
        return VK_NULL_HANDLE;
    }

    ColourFrame colour{};
    colour.image = frame.in_place ? frame.colour : s.vk_output;
    colour.format = frame.colour_format;
    colour.width = frame.width; colour.height = frame.height;
    colour.before = colour.after = frame.in_place ? frame.colour_layout : VK_IMAGE_LAYOUT_GENERAL;
    if (frame.in_place) colour.usage = frame.colour_usage;

    EngineFrame engine{};
    engine.colour = colour;
    // The caller's colour read in place and its output written directly (the engine path with
    // the model applied): the copy into vk_output, the copy into the runtime's input and the
    // write-back all go. Anything else keeps the copies below.
    const bool direct = !frame.in_place && frame.output && frame.motion && s.runtime && s.runtime->takes_target(controls) &&
                        (frame.colour_usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
                        !(std::getenv("NR_VK_COPY") && std::atoi(std::getenv("NR_VK_COPY")));
    if (direct) {
        engine.colour.image = frame.colour;
        engine.colour.before = engine.colour.after = frame.colour_layout;
        engine.colour.usage = frame.colour_usage;
        engine.target = colour;
        engine.target.image = frame.output;
        engine.target.before = engine.target.after = frame.output_layout;
        engine.target.usage = frame.output_usage;
    }
    engine.feature = frame.feature;
    engine.reset = frame.reset;
    if (s.resized(frame.feature, frame.width, frame.height) && !s.carry(cmd, frame.feature)) engine.reset = true;
    engine.motion_scale_x = frame.motion_scale_x;
    engine.motion_scale_y = frame.motion_scale_y;
    engine.depth_inverted = frame.depth_inverted;
    if (frame.motion) {
        engine.motion.image = frame.motion;
        engine.motion.format = frame.motion_format;
        engine.motion.width = frame.motion_width;
        engine.motion.height = frame.motion_height;
        engine.motion.before = engine.motion.after = frame.motion_layout;
        engine.motion.usage = frame.motion_usage;
        engine.motion_texture_width = engine.motion.width; engine.motion_texture_height = engine.motion.height;
        apply_guide_subrect(&engine.motion, frame.motion_subrect, "motion-vector", &engine.motion_x, &engine.motion_y);
    }
    if (frame.depth) {
        engine.depth.image = frame.depth;
        engine.depth.format = frame.depth_format;
        engine.depth.width = frame.depth_width;
        engine.depth.height = frame.depth_height;
        engine.depth.before = engine.depth.after = frame.depth_layout;
        engine.depth.usage = frame.depth_usage;
        apply_guide_subrect(&engine.depth, frame.depth_subrect, "depth", &engine.depth_x, &engine.depth_y);
    }

    if (renodx_stage) {
        ColourFrame in{}, out{};
        in.image = frame.colour; in.format = frame.colour_format; in.width = frame.width; in.height = frame.height;
        in.before = in.after = frame.colour_layout; in.usage = frame.colour_usage;
        const bool to_output = !frame.in_place && frame.output;
        out = in;
        if (to_output) {
            out.image = frame.output; out.before = out.after = frame.output_layout; out.usage = frame.output_usage;
        }
        ColourFrame exposure{};
        const ColourFrame* ex = nullptr;
        if (frame.renodx->vk_exposure) {
            exposure.image = frame.renodx->vk_exposure; exposure.format = frame.renodx->vk_exposure_format;
            exposure.width = frame.renodx->vk_exposure_width; exposure.height = frame.renodx->vk_exposure_height;
            exposure.before = exposure.after = VK_IMAGE_LAYOUT_GENERAL;
            ex = &exposure;
        }
        if (!s.record_renodx(cmd, *renodx_stage, in, out, engine, renodx_controls, *frame.renodx, ex)) return VK_NULL_HANDLE;
        return out.image;
    }
    if (direct) {
        try {
            s.runtime->record_engine(cmd, engine, controls);
        } catch (const std::exception& e) {
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return VK_NULL_HANDLE;
        }
        return frame.output;
    }
    if (frame.in_place) {
        try {
            if (frame.motion) s.runtime->record_engine(cmd, engine, controls);
            else s.runtime->record(cmd, colour, controls, engine.reset);
        } catch (const std::exception& e) {
            s.status = std::string("neural rendering failed: ") + e.what();
            s.failed = true;
            log("[nr] %s", s.status.c_str());
            return VK_NULL_HANDLE;
        }
        return frame.colour;
    }
    try {
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {frame.width, frame.height, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = frame.colour_layout; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = frame.colour;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyImage(cmd, frame.colour, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s.vk_output, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        std::swap(b.oldLayout, b.newLayout);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        if (frame.motion) s.runtime->record_engine(cmd, engine, controls);
        else s.runtime->record(cmd, colour, controls, engine.reset);
    } catch (const std::exception& e) {
        s.status = std::string("neural rendering failed: ") + e.what();
        s.failed = true;
        log("[nr] %s", s.status.c_str());
        return VK_NULL_HANDLE;
    }
    return s.vk_output;
}

}  // namespace nr::pe
