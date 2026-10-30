#include "nr_gpu_completion.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace nr::gpu;
    bool done = false;
    auto ticket = std::make_shared<Recording>();
    ticket->submitted([&] { return done; });
    ticket->seal();
    Uses uses;
    { Scope scope(ticket); uses.touch(); }
    assert(!uses.ready());
    // Long-running known work must remain retained even after the legacy fallback thresholds.
    submissions += 100;
    ticket->touched -= std::chrono::seconds(3);
    assert(!uses.ready());
    done = true;
    assert(uses.ready());

    int primary_key, secondary_key;
    auto secondary = recording(&secondary_key);
    inherit(&primary_key, &secondary_key);
    auto primary = find(&primary_key);
    done = false;
    primary->submitted([&] { return done; });
    reset(&secondary_key);
    reset(&primary_key);
    submissions += 100;
    primary->touched -= std::chrono::seconds(3);
    secondary->touched -= std::chrono::seconds(3);
    assert(!secondary->ready());
    done = true;
    assert(secondary->ready());

    // A cancelled, never-submitted one-shot recording can release its resources immediately.
    auto cancelled = std::make_shared<Recording>();
    cancelled->seal();
    assert(cancelled->ready());
    struct Slot { Ticket gpu; };
    std::vector<Slot> ring(8192);
    done = false;
    auto pending = std::make_shared<Recording>();
    pending->submitted([&] { return done; }); pending->seal();
    for (auto& item : ring) item.gpu = pending;
    bool refused = false;
    try { (void)slot(ring, 0); } catch (const std::runtime_error&) { refused = true; }
    assert(refused);
    done = true;
    assert(&slot(ring, 0) == &ring[0]);
    std::cout << "PASS: GPU receipt retention, nested recordings, cancellation and ring exhaustion\n";
}
