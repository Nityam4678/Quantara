// ---------------------------------------------------------------------------
// Limit Order Book -- latency and throughput benchmark
//
// Events are generated up front into a flat vector and the clock is started
// only once generation is finished, so the RNG and the normal distribution
// never appear in the measurement.
//
// Two numbers are reported for every workload:
//   * BULK      -- one steady_clock reading either side of the whole loop.
//                  No per-event instrumentation, so nothing distorts it.
//                  This is the honest throughput and mean-latency figure.
//   * PER-EVENT -- a second pass timing each message with RDTSC to get the
//                  distribution. RDTSC is cheap but not free; the measured
//                  cost of the instrumentation itself is printed so the tail
//                  numbers can be read with the right amount of salt.
//
// A mean latency on its own says very little about a matching engine -- the
// p99.9 is what decides whether you are filled or adversely selected. Both
// are printed.
// ---------------------------------------------------------------------------
#include "lob/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
  #define LOB_HAS_RDTSC 1
  #include <x86intrin.h>
#else
  #define LOB_HAS_RDTSC 0
#endif

using namespace lob;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Event stream
// ---------------------------------------------------------------------------
enum class EventType : std::uint8_t { Add = 0, Cancel = 1 };

// 24 bytes, trivially copyable: ~2.7 events per cache line, so replaying the
// stream is a linear scan the hardware prefetcher handles perfectly. The cost
// of feeding the book must not show up in the book's own numbers.
struct Event {
    OrderId   id;
    Price     price;
    Quantity  quantity;
    Side      side;
    EventType type;
};
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(sizeof(Event) == 24);

struct Workload {
    const char* name;
    const char* description;
    bool        passive_skew;   // price on the resting side of the mid
    int         cancel_pct;
    int         aggressive_pct; // only used when passive_skew is true
};

struct GenConfig {
    std::size_t   count   = 2'000'000;
    std::uint64_t seed    = 0x9E3779B97F4A7C15ULL;
    Price         mid     = 10'000;
    double        sigma   = 12.0;   // ticks
    Quantity      max_qty = 100;
};

// ---------------------------------------------------------------------------
// Generate the whole stream before any clock starts.
//
// Cancels are targeted using a SHADOW OrderBook that the generator runs as it
// goes, so every cancel names an order that is genuinely resting at that point
// in the stream. Without this, cancels drawn at random from recently-issued
// ids mostly name orders that were already filled on arrival -- they take the
// "unknown id" early-out and the benchmark reports a cancel path that never
// actually cancels anything.
//
// The shadow book exists only here. The measured replay starts from an empty
// book and processes the identical sequence, so it reaches identical state,
// and none of this work is inside the timed region.
// ---------------------------------------------------------------------------
std::vector<Event> generate(const GenConfig& cfg, const Workload& w) {
    std::vector<Event> events;
    events.reserve(cfg.count);

    std::mt19937_64                         rng(cfg.seed);
    std::normal_distribution<double>        price_dist(static_cast<double>(cfg.mid), cfg.sigma);
    std::uniform_int_distribution<int>      pct(0, 99);
    std::uniform_int_distribution<Quantity> qty_dist(1, cfg.max_qty);

    OrderBook            shadow(cfg.count);
    std::vector<OrderId> live;          // ids believed to be resting
    live.reserve(cfg.count / 4);
    OrderId next_id = 1;

    const double lo = static_cast<double>(cfg.mid) - 10.0 * cfg.sigma;
    const double hi = static_cast<double>(cfg.mid) + 10.0 * cfg.sigma;

    for (std::size_t i = 0; i < cfg.count; ++i) {
        const int roll = pct(rng);

        if (roll < w.cancel_pct && !live.empty()) {
            // Pick a candidate, discarding ids that have since been filled.
            // Swap-and-pop, so each id is offered at most once: amortised O(1).
            OrderId target = 0;
            while (!live.empty()) {
                const std::size_t k = static_cast<std::size_t>(rng() % live.size());
                const OrderId     id = live[k];
                live[k] = live.back();
                live.pop_back();
                if (shadow.find_order(id) != nullptr) { target = id; break; }
            }
            if (target != 0) {
                events.push_back(Event{.id       = target,
                                       .price    = 0,
                                       .quantity = 0,
                                       .side     = Side::BUY,
                                       .type     = EventType::Cancel});
                shadow.cancel_order(target);
                continue;
            }
            // Nothing live left: fall through and emit an add instead.
        }

        const Side side = (rng() & 1U) ? Side::BUY : Side::SELL;

        // Prices cluster around the mid so the two sides overlap and trades
        // actually happen. Clamped so a bad tail draw cannot wander off into
        // an absurd tick.
        double raw = price_dist(rng);
        if (w.passive_skew) {
            // Quote on your own side of the mid -- buys below, sells above --
            // with a slice of deliberately aggressive orders crossing it.
            const double offset     = std::abs(raw - static_cast<double>(cfg.mid)) + 1.0;
            const bool   aggressive = roll >= (100 - w.aggressive_pct);
            const double signed_off = aggressive ? -offset : offset;
            raw = (side == Side::BUY) ? static_cast<double>(cfg.mid) - signed_off
                                      : static_cast<double>(cfg.mid) + signed_off;
        }
        Price price = static_cast<Price>(std::llround(std::clamp(raw, lo, hi)));
        if (price == 0) price = 1;

        const Event e{.id       = next_id++,
                      .price    = price,
                      .quantity = qty_dist(rng),
                      .side     = side,
                      .type     = EventType::Add};
        events.push_back(e);

        shadow.add_order(e.id, e.side, e.price, e.quantity);
        if (shadow.last_report().resting > 0) {
            live.push_back(e.id);
        }
    }

    return events;
}

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------
#if LOB_HAS_RDTSC
inline std::uint64_t tsc_before() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
inline std::uint64_t tsc_after() noexcept {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);  // waits on prior instructions
    _mm_lfence();
    return t;
}
#else
inline std::uint64_t tsc_before() noexcept {
    return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
}
inline std::uint64_t tsc_after() noexcept { return tsc_before(); }
#endif

// TSC ticks per nanosecond, measured against steady_clock.
double calibrate_ticks_per_ns() {
#if LOB_HAS_RDTSC
    using namespace std::chrono;
    const auto          t0 = Clock::now();
    const std::uint64_t c0 = tsc_before();
    while (Clock::now() - t0 < milliseconds(200)) { /* spin */ }
    const auto          t1 = Clock::now();
    const std::uint64_t c1 = tsc_after();
    return static_cast<double>(c1 - c0) / duration<double, std::nano>(t1 - t0).count();
#else
    return 1.0;  // fallback path already counts in clock ticks
#endif
}

// Cost of the timing instrumentation itself, in ticks (median of many pairs).
std::uint64_t measure_timer_overhead() {
    constexpr int          kSamples = 20'000;
    std::vector<std::uint64_t> samples;
    samples.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const std::uint64_t a = tsc_before();
        const std::uint64_t b = tsc_after();
        samples.push_back(b - a);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

// ---------------------------------------------------------------------------
// Percentiles
// ---------------------------------------------------------------------------
struct Percentiles {
    double min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0, mean = 0;
    std::size_t n = 0;
};

// `samples` is sorted in place.
Percentiles percentiles(std::vector<std::uint32_t>& samples, double ticks_per_ns) {
    Percentiles p;
    p.n = samples.size();
    if (samples.empty()) return p;

    std::sort(samples.begin(), samples.end());

    const auto at = [&](double q) {
        const std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(samples.size() - 1));
        return static_cast<double>(samples[idx]) / ticks_per_ns;
    };

    long double sum = 0;
    for (const std::uint32_t s : samples) sum += s;

    p.min   = static_cast<double>(samples.front()) / ticks_per_ns;
    p.p50   = at(0.50);
    p.p90   = at(0.90);
    p.p99   = at(0.99);
    p.p999  = at(0.999);
    p.p9999 = at(0.9999);
    p.max   = static_cast<double>(samples.back()) / ticks_per_ns;
    p.mean  = static_cast<double>(sum / static_cast<long double>(samples.size())) / ticks_per_ns;
    return p;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------
struct RunStats {
    double        seconds     = 0;
    std::size_t   events      = 0;
    std::size_t   resting_end = 0;
    std::size_t   levels_end  = 0;
    std::uint64_t checksum    = 0;
};

// Bulk pass: no per-event timing at all.
RunStats replay(OrderBook& book, const std::vector<Event>& events) {
    RunStats s;
    s.events = events.size();

    const auto t0 = Clock::now();
    for (const Event& e : events) {
        if (e.type == EventType::Add) {
            book.add_order(e.id, e.side, e.price, e.quantity);
        } else {
            book.cancel_order(e.id);
        }
        // Consuming the report keeps the optimiser from deleting the call and
        // costs one add on a value that is already hot in cache.
        s.checksum += book.last_report().filled;
    }
    const auto t1 = Clock::now();

    s.seconds     = std::chrono::duration<double>(t1 - t0).count();
    s.resting_end = book.live_order_count();
    s.levels_end  = book.bid_level_count() + book.ask_level_count();
    return s;
}

// Instrumented pass: one RDTSC pair per event, split by message type.
struct LatencyRun {
    std::vector<std::uint32_t> add_ticks;
    std::vector<std::uint32_t> cancel_ticks;
    std::size_t                trades      = 0;
    std::uint64_t              volume      = 0;
    std::size_t                cancel_hits = 0;
    std::size_t                rejects     = 0;
    std::uint64_t             checksum    = 0;
};

LatencyRun replay_instrumented(OrderBook& book, const std::vector<Event>& events) {
    LatencyRun r;
    // Reserved before timing starts -- push_back must never allocate mid-run.
    r.add_ticks.reserve(events.size());
    r.cancel_ticks.reserve(events.size());

    for (const Event& e : events) {
        if (e.type == EventType::Add) {
            const std::uint64_t a = tsc_before();
            book.add_order(e.id, e.side, e.price, e.quantity);
            const std::uint64_t b = tsc_after();
            r.add_ticks.push_back(static_cast<std::uint32_t>(b - a));

            const ExecutionReport& rep = book.last_report();
            if (!rep.ok()) ++r.rejects;
            r.checksum += rep.filled;
            r.trades   += book.last_trades().size();
            for (const Trade& t : book.last_trades()) r.volume += t.quantity;
        } else {
            const std::uint64_t a = tsc_before();
            book.cancel_order(e.id);
            const std::uint64_t b = tsc_after();
            r.cancel_ticks.push_back(static_cast<std::uint32_t>(b - a));

            if (book.last_report().ok()) ++r.cancel_hits;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
void rule(char c = '-') {
    for (int i = 0; i < 78; ++i) std::putchar(c);
    std::putchar('\n');
}

void print_percentile_row(const char* label, const Percentiles& p) {
    if (p.n == 0) {
        std::printf("  %-10s %s\n", label, "(no samples)");
        return;
    }
    std::printf("  %-10s %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f %9.0f\n",
                label, p.mean, p.p50, p.p90, p.p99, p.p999, p.p9999, p.max);
}

void run_workload(const Workload& w, const GenConfig& cfg,
                  double ticks_per_ns, std::uint64_t timer_overhead_ticks) {
    rule('=');
    std::printf("WORKLOAD: %s\n%s\n", w.name, w.description);
    rule('=');

    // ---- generation (untimed) ------------------------------------------
    const auto gen_t0 = Clock::now();
    const std::vector<Event> events = generate(cfg, w);
    const auto gen_t1 = Clock::now();
    std::printf("  generated %zu events in %.2f s  (%.1f MB, not timed below)\n",
                events.size(),
                std::chrono::duration<double>(gen_t1 - gen_t0).count(),
                static_cast<double>(events.size() * sizeof(Event)) / (1024.0 * 1024.0));

    std::size_t adds = 0;
    for (const Event& e : events) if (e.type == EventType::Add) ++adds;
    std::printf("  mix: %zu adds (%.1f%%), %zu cancels (%.1f%%)\n\n",
                adds, 100.0 * static_cast<double>(adds) / static_cast<double>(events.size()),
                events.size() - adds,
                100.0 * static_cast<double>(events.size() - adds) / static_cast<double>(events.size()));

    // ---- warm-up -------------------------------------------------------
    // A throwaway book over the first slice: warms the instruction cache and
    // the branch predictors so the measured run is not paying for cold code.
    {
        OrderBook warm(cfg.count);
        const std::size_t n = std::min<std::size_t>(events.size() / 10, 200'000);
        for (std::size_t i = 0; i < n; ++i) {
            const Event& e = events[i];
            if (e.type == EventType::Add) warm.add_order(e.id, e.side, e.price, e.quantity);
            else                          warm.cancel_order(e.id);
        }
    }

    // ---- pass 1: bulk throughput ---------------------------------------
    OrderBook book(cfg.count);          // pool prefaulted in the constructor
    const RunStats s = replay(book, events);

    const double ns_per_event = s.seconds * 1e9 / static_cast<double>(s.events);
    const double per_sec      = static_cast<double>(s.events) / s.seconds;

    rule();
    std::printf("BULK  (no per-event instrumentation)\n");
    rule();
    std::printf("  total time            %12.4f s\n", s.seconds);
    std::printf("  events processed      %12zu\n", s.events);
    std::printf("  throughput            %12.0f events/sec  (%.2f M/s)\n", per_sec, per_sec / 1e6);
    std::printf("  average latency       %12.1f ns/event\n", ns_per_event);
    std::printf("  book at end           %12zu resting orders across %zu levels\n",
                s.resting_end, s.levels_end);
    // The resting book IS the working set for cancels: a cancel is a hash
    // lookup followed by a pointer chase into one 64-byte Order. Once that set
    // outgrows L3, cancel latency is bounded by memory, not by the code.
    {
        const std::size_t bytes = s.resting_end * sizeof(Order);
        std::printf("  order working set     %12.1f MB%s\n",
                    static_cast<double>(bytes) / (1024.0 * 1024.0),
                    bytes > (32u << 20) ? "   (past L3 -- cancels are memory-bound)" : "");
    }

    // ---- pass 2: latency distribution ----------------------------------
    OrderBook book2(cfg.count);
    LatencyRun lr = replay_instrumented(book2, events);

    Percentiles add_p    = percentiles(lr.add_ticks, ticks_per_ns);
    Percentiles cancel_p = percentiles(lr.cancel_ticks, ticks_per_ns);

    std::vector<std::uint32_t> all;
    all.reserve(lr.add_ticks.size() + lr.cancel_ticks.size());
    all.insert(all.end(), lr.add_ticks.begin(), lr.add_ticks.end());
    all.insert(all.end(), lr.cancel_ticks.begin(), lr.cancel_ticks.end());
    Percentiles all_p = percentiles(all, ticks_per_ns);

    std::putchar('\n');
    rule();
    std::printf("PER-EVENT LATENCY (ns)   [RDTSC; timer overhead %.1f ns is INCLUDED below]\n",
                static_cast<double>(timer_overhead_ticks) / ticks_per_ns);
    rule();
    std::printf("  %-10s %9s %9s %9s %9s %9s %9s %9s\n",
                "", "mean", "p50", "p90", "p99", "p99.9", "p99.99", "max");
    print_percentile_row("add",    add_p);
    print_percentile_row("cancel", cancel_p);
    print_percentile_row("all",    all_p);

    std::putchar('\n');
    std::printf("  trades executed       %12zu  (%.2f per add)\n",
                lr.trades, static_cast<double>(lr.trades) / static_cast<double>(adds));
    std::printf("  volume traded         %12llu units\n",
                static_cast<unsigned long long>(lr.volume));
    std::printf("  cancel hit rate       %12.1f%%  (misses = order already filled)\n",
                lr.cancel_ticks.empty() ? 0.0
                    : 100.0 * static_cast<double>(lr.cancel_hits)
                            / static_cast<double>(lr.cancel_ticks.size()));
    std::printf("  rejected adds         %12zu\n", lr.rejects);
    std::printf("  book consistent       %12s\n", book2.verify() ? "yes" : "NO -- BUG");
    std::printf("  checksum              %12llu / %llu\n",
                static_cast<unsigned long long>(s.checksum),
                static_cast<unsigned long long>(lr.checksum));
    std::putchar('\n');
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    GenConfig cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--events=", 9) == 0) {
            cfg.count = std::strtoull(argv[i] + 9, nullptr, 10);
        } else if (std::strncmp(argv[i], "--seed=", 7) == 0) {
            cfg.seed = std::strtoull(argv[i] + 7, nullptr, 10);
        } else if (std::strncmp(argv[i], "--sigma=", 8) == 0) {
            cfg.sigma = std::strtod(argv[i] + 8, nullptr);
        } else {
            std::printf("usage: %s [--events=N] [--seed=N] [--sigma=F]\n", argv[0]);
            return 2;
        }
    }
    if (cfg.count == 0) { std::puts("--events must be > 0"); return 2; }

    rule('=');
    std::puts("LIMIT ORDER BOOK BENCHMARK");
    rule('=');
    std::printf("  compiler              %s\n",
#if defined(__clang__)
                "clang " __clang_version__
#elif defined(__GNUC__)
                "gcc " __VERSION__
#else
                "unknown"
#endif
    );
    std::printf("  asserts               %s\n",
#ifdef NDEBUG
                "off (NDEBUG)"
#else
                "ON -- numbers are NOT representative, rebuild with -DNDEBUG"
#endif
    );
    std::printf("  sizeof(Order)         %12zu bytes\n", sizeof(Order));
    std::printf("  mid price             %12llu ticks, sigma %.1f\n",
                static_cast<unsigned long long>(cfg.mid), cfg.sigma);

    const double        ticks_per_ns = calibrate_ticks_per_ns();
    const std::uint64_t overhead     = measure_timer_overhead();
#if LOB_HAS_RDTSC
    std::printf("  TSC                   %12.4f GHz (calibrated), timer pair %.1f ns\n",
                ticks_per_ns, static_cast<double>(overhead) / ticks_per_ns);
#else
    std::puts("  TSC                   unavailable; using steady_clock (higher overhead)");
#endif
    std::putchar('\n');

    const Workload workloads[] = {
        {"crossing flow",
         "  Prices ~ N(mid, sigma) on a random side, 20% cancels. Both sides\n"
         "  overlap the mid, so a large share of orders trade on arrival and\n"
         "  the resting book stays thin.",
         false, 20, 0},
        {"deep book",
         "  Same distribution, but passive orders quote on their own side of\n"
         "  the mid and 10% cross deliberately. Depth accumulates, so the map\n"
         "  lookups and the matching loop do real work.\n"
         "  NOTE: at 80% adds / 20% cancels this book never reaches steady\n"
         "  state -- it grows for the whole run and ends far deeper than any\n"
         "  real venue. Read its cancel numbers as a memory-bound worst case.",
         true, 20, 10},
    };

    for (const Workload& w : workloads) {
        run_workload(w, cfg, ticks_per_ns, overhead);
    }

    rule('=');
    std::puts("Caveats: single run, no core pinning, no frequency lock. Percentile");
    std::puts("tails include OS scheduling noise and the RDTSC pair itself.");
    rule('=');
    return 0;
}
