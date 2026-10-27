// CPU fault injection, not a GPU/API conformance or image-quality test.
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using VkCommandBuffer = uint64_t;
using VkCommandPool = uint64_t;
using VkFence = uint64_t;
using VkImage = uint64_t;
using VkDeviceMemory = uint64_t;
using VkSemaphore = uint64_t;
using VkPipelineStageFlags = uint32_t;
using VkResult = int;
constexpr int VK_SUCCESS = 0, VK_ERROR_DEVICE_LOST = -4;
constexpr int VK_STRUCTURE_TYPE_SUBMIT_INFO = 1;
constexpr int VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO = 2;
constexpr int VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR = 3;
constexpr VkPipelineStageFlags VK_PIPELINE_STAGE_ALL_COMMANDS_BIT = 0x10000;
struct VkTimelineSemaphoreSubmitInfo {
    int sType; const void* pNext{};
    uint32_t waitSemaphoreValueCount{}; const uint64_t* pWaitSemaphoreValues{};
    uint32_t signalSemaphoreValueCount{}; const uint64_t* pSignalSemaphoreValues{};
};
struct VkWin32KeyedMutexAcquireReleaseInfoKHR {
    int sType; const void* pNext{};
    uint32_t acquireCount{}; const VkDeviceMemory* pAcquireSyncs{};
    const uint64_t* pAcquireKeys{}; const uint32_t* pAcquireTimeouts{};
    uint32_t releaseCount{}; const VkDeviceMemory* pReleaseSyncs{};
    const uint64_t* pReleaseKeys{};
};
struct VkSubmitInfo {
    int sType; const void* pNext{};
    uint32_t waitSemaphoreCount{}; const VkSemaphore* pWaitSemaphores{};
    const VkPipelineStageFlags* pWaitDstStageMask{};
    uint32_t commandBufferCount{}; const VkCommandBuffer* pCommandBuffers{};
    uint32_t signalSemaphoreCount{}; const VkSemaphore* pSignalSemaphores{};
};
template<class... T> void log(const char*, T...) {}
struct Image { VkDeviceMemory memory{}; };
#include "bridge_job.inc"

using HRESULT = int32_t;
constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED = -10, DXGI_ERROR_DEVICE_RESET = -11;
constexpr bool FAILED(HRESULT hr) { return hr < 0; }
constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
struct IUnknown {
    int refs = 1;
    void AddRef() { ++refs; }
    void Release() { --refs; }
};
struct ID3D12Fence : IUnknown { uint64_t GetCompletedValue() { return 0; } };
struct ID3D11Fence : IUnknown {};
struct QueueCalls : IUnknown {
    std::vector<uint64_t> signals, waits;
    HRESULT signal_result{}, wait_result{};
    bool fail_second_signal = false;
    int flushes{};
    template<class Fence> HRESULT Signal(Fence*, uint64_t value) {
        signals.push_back(value);
        return fail_second_signal && signals.size() == 2 ? -1 : signal_result;
    }
    template<class Fence> HRESULT Wait(Fence*, uint64_t value) {
        waits.push_back(value); return wait_result;
    }
    void Flush() { ++flushes; }
};
struct ID3D12CommandQueue : QueueCalls {};
struct ID3D11DeviceContext : QueueCalls {
    template<class T> HRESULT QueryInterface(T** out) { *out = static_cast<T*>(this); AddRef(); return 0; }
};
struct ID3D11DeviceContext4 : ID3D11DeviceContext {};
#define IID_PPV_ARGS(pp) pp

struct Device {
    bool lost_{};
    std::mutex queue_mutex_;
    uint64_t queue_ = 1;
#include "bridge_members.inc"
#include "bridge_clock.inc"
    void drop_locked(Job*, std::vector<std::function<void()>>&);
    void discard(Job*, uint64_t);
    void cancel(const void*);
    bool submit(Job*, uint64_t, VkSemaphore, uint64_t, VkSemaphore, uint64_t);
    bool submit_keyed(Job*, uint64_t, const std::vector<const Image*>&, uint64_t, uint64_t);
    bool run_between(ID3D12CommandQueue*, Job*, uint64_t);
    bool run_between(ID3D11DeviceContext*, Job*, uint64_t);
    void lost(const char*);
    std::mutex sync_mutex_;
    IUnknown* hand_over_queue_{};
    struct Timeline { VkSemaphore semaphore = 1; IUnknown* fence{}; uint64_t value{}; } timeline_;
    Timeline* timeline_d3d12(ID3D12CommandQueue*) { return &timeline_; }
    Timeline* timeline_d3d11(ID3D11DeviceContext*) { return &timeline_; }
    std::vector<int> buried_;
    void sweep_buried_locked(uint64_t) {}
    Job* ready(uint64_t serial = 1) {
        auto j = std::make_unique<Job>();
        j->state = Job::Ready; j->serial = serial; j->gen = 1;
        j->cmd = 7; j->fence = 8; j->owner = this;
        jobs_.push_back(std::move(j));
        clock() = serial;
        return jobs_.back().get();
    }
};

static int checks{}, failures{}, submits{};
static VkResult next_result{};
static std::function<void(const VkSubmitInfo&)> inspect_submit;
static void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
// Probe from another thread: try_lock by the owning thread is not a valid test.
static bool unlocked(std::mutex& m) {
    bool result = false;
    std::thread t([&] { result = m.try_lock(); if (result) m.unlock(); });
    t.join();
    return result;
}
static VkResult vkQueueSubmit(uint64_t, uint32_t count, const VkSubmitInfo* si, VkFence) {
    ++submits;
    check(count == 1 && si->commandBufferCount == 1 && *si->pCommandBuffers == 7,
          "submit carries the recorded command buffer");
    if (inspect_submit) inspect_submit(*si);
    return next_result;
}
#include "bridge_methods.inc"

static bool run(Device& d, Job* j, bool keyed, uint64_t generation = 1) {
    const Image a{11}, b{12}, empty{};
    return keyed ? d.submit_keyed(j, generation, {&a, nullptr, &empty, &b}, 3, 4)
                 : d.submit(j, generation, 21, 31, 22, 32);
}

int main() {
    for (bool keyed : {false, true}) {
        for (bool callback : {false, true}) {
            for (bool stale : {false, true}) {
                Device d;
                Job* j = d.ready();
                int drops = 0;
                if (callback) j->dropped = [&] {
                    ++drops;
                    check(unlocked(d.jobs_mutex_) && unlocked(d.queue_mutex_), "drop callback runs outside locks");
                };
                if (stale) d.clock() = j->serial + kStaleAfter + 1;
                else d.lost_ = true;
                const int before = submits;
                check(!run(d, j, keyed), "lost/stale job must be rejected even without a callback");
                check(submits == before, "rejected job must never reach Vulkan");
                check(j->state == Job::Free && !j->owner, "rejected job is retired");
                check(drops == int(callback), "drop notification occurs exactly once");
            }
        }
        {
            Device d;
            Job* j = d.ready();
            d.clock() += kStaleAfter; // boundary is still safe
            inspect_submit = [&](const VkSubmitInfo& si) {
                check(!unlocked(d.jobs_mutex_), "job stays reserved throughout Vulkan submission");
                check(!unlocked(d.queue_mutex_), "Vulkan queue is externally synchronized");
                if (keyed) {
                    auto& km = *static_cast<const VkWin32KeyedMutexAcquireReleaseInfoKHR*>(si.pNext);
                    check(km.acquireCount == 2 && km.releaseCount == 2 &&
                          km.pAcquireSyncs[0] == 11 && km.pAcquireSyncs[1] == 12 &&
                          km.pAcquireKeys[0] == 3 && km.pReleaseKeys[1] == 4 &&
                          km.pAcquireTimeouts[0] == 1000, "keyed mutex handover is preserved");
                } else {
                    auto& ts = *static_cast<const VkTimelineSemaphoreSubmitInfo*>(si.pNext);
                    check(si.waitSemaphoreCount == 1 && *si.pWaitSemaphores == 21 &&
                          si.signalSemaphoreCount == 1 && *si.pSignalSemaphores == 22 &&
                          *ts.pWaitSemaphoreValues == 31 && *ts.pSignalSemaphoreValues == 32 &&
                          *si.pWaitDstStageMask == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          "timeline handover is preserved");
                }
            };
            check(run(d, j, keyed), "valid boundary job succeeds");
            inspect_submit = nullptr;
            check(j->state == Job::Submitted && j->submitted != 0, "successful job becomes submitted");
            d.discard(j, 1);
            d.cancel(j->owner);
            check(j->state == Job::Submitted, "submitted job cannot be cancelled or recycled");
            const int before = submits;
            check(!run(d, j, keyed) && submits == before, "duplicate submission is rejected");
        }
        for (VkResult failure : {-1, VK_ERROR_DEVICE_LOST}) {
            Device d;
            Job* j = d.ready();
            int drops = 0;
            j->dropped = [&] {
                ++drops;
                check(unlocked(d.jobs_mutex_) && unlocked(d.queue_mutex_), "failure callback can reenter lifecycle");
                d.discard(j, 1);
            };
            next_result = failure;
            check(!run(d, j, keyed), "Vulkan failure propagates");
            next_result = VK_SUCCESS;
            check(j->state == Job::Free && drops == 1, "failed job drops once");
            check(d.lost_ == (failure == VK_ERROR_DEVICE_LOST), "device loss is latched on both routes");
            if (failure == VK_ERROR_DEVICE_LOST) {
                auto* next = d.ready(2);
                const int before = submits;
                check(!run(d, next, keyed) && submits == before, "device loss blocks future submissions");
            }
        }
        {
            Device d;
            Job* j = d.ready();
            const int before = submits;
            check(!run(d, j, keyed, 2) && submits == before && j->state == Job::Ready,
                  "old generation cannot submit or discard a new recording");
            d.discard(j, 1);
            check(!run(d, j, keyed) && submits == before, "discarded job cannot be submitted");
        }
        {
            Device d, other;
            auto* j = d.ready();
            int drops = 0;
            j->dropped = [&] { ++drops; };
            other.clock() = kStaleAfter + 100;
            check(run(d, j, keyed) && drops == 0, "activity on another adapter cannot age this device's job");
        }
    }
    {
        Device d;
        auto* older = d.ready(1);
        auto* newer = d.ready(2);
        int drops = 0;
        newer->dropped = [&] { ++drops; check(unlocked(d.jobs_mutex_), "order callback is outside lock"); };
        check(run(d, newer, false) && drops == 1 && older->state == Job::Ready,
              "out-of-order submission still requests a history restart");
    }
    // A successful Vulkan submit is insufficient: DirectX must accept the wait
    // before the caller can safely copy the network's result back to the game.
    for (bool d3d11 : {false, true}) {
        for (int scenario = 0; scenario != 5; ++scenario) {
            Device d;
            ID3D12CommandQueue q12;
            ID3D11DeviceContext4 q11;
            ID3D12Fence f12;
            ID3D11Fence f11;
            QueueCalls& q = d3d11 ? static_cast<QueueCalls&>(q11) : static_cast<QueueCalls&>(q12);
            d.timeline_.fence = d3d11 ? static_cast<IUnknown*>(&f11) : static_cast<IUnknown*>(&f12);
            Job* j = d.ready();
            // 0: normal, 1: wait refused, 2: ready signal refused,
            // 3: NR submit failed, 4: NR failed and fallback done signal refused.
            if (scenario == 1) q.wait_result = -1;
            if (scenario == 2) q.signal_result = -1;
            if (scenario >= 3) next_result = -1;
            q.fail_second_signal = scenario == 4;
            const bool ran = d3d11 ? d.run_between(&q11, j, 1) : d.run_between(&q12, j, 1);
            next_result = VK_SUCCESS;
            check(ran == (scenario == 0), "handover succeeds only when submission AND DirectX wait succeed");
            check(d.lost_ == (scenario == 1), "refused DirectX wait stops further network work");
            check(q.signals == (scenario >= 3 ? std::vector<uint64_t>{1, 2} : std::vector<uint64_t>{1}),
                  "failed NR work signals done from DirectX to avoid a permanent wait");
            const bool should_wait = scenario != 2 && scenario != 4;
            check(q.waits == (should_wait ? std::vector<uint64_t>{2} : std::vector<uint64_t>{}),
                  "never wait for a value that neither API can signal");
            if (d3d11) check(q.flushes == (scenario == 2 ? 0 : 1) && q11.refs == 2,
                             "D3D11 publishes ready and releases its temporary context reference");
        }
    }
    std::cout << (failures ? "FAIL: " : "PASS: ") << checks << " bridge checks, " << failures
              << " failures (fake Vulkan queue; no GPU inference)\n";
    return failures ? 1 : 0;
}
