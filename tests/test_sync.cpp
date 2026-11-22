// Host tests for src/sync.h: bounded seqlock reads and the channel guard.
#include "sync.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

static int gFailures = 0;
static int gChecks   = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        gChecks++;                                                                        \
        if (!(cond)) {                                                                    \
            gFailures++;                                                                  \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

static DriftParams make(float k) {
    DriftParams p;
    p.deadzone      = k;
    p.outer         = k + 0.5f;
    p.sensitivity   = 1.0f + k;
    p.offsetX       = k;
    p.offsetY       = -k;
    p.offsetEnabled = 1;
    return p;
}

static bool consistent(const DriftParams &p) {
    return p.outer == p.deadzone + 0.5f && p.sensitivity == 1.0f + p.deadzone &&
           p.offsetX == p.deadzone && p.offsetY == -p.deadzone && p.offsetEnabled == 1;
}

static void test_roundtrip() {
    ParamSnapshot snap;
    snap.publish(make(0.25f));
    DriftParams out{};
    CHECK(snap.tryRead(out));
    CHECK(consistent(out) && out.deadzone == 0.25f);
}

static void test_interrupted_writer_is_bounded() {
    ParamSnapshot snap;
    snap.publish(make(0.10f));
    DriftParams lastGood{};
    CHECK(snap.readWithFallback(lastGood));
    CHECK(lastGood.deadzone == 0.10f);

    // Simulate a writer preempted (or killed) half-way through an update.
    snap.beginWrite();
    snap.writeFields(make(0.40f));

    const auto t0   = std::chrono::steady_clock::now();
    DriftParams out = make(0.99f);
    const bool ok   = snap.tryRead(out);
    const auto dt   = std::chrono::steady_clock::now() - t0;
    CHECK(!ok);                                // no torn snapshot is ever returned
    CHECK(out.deadzone == 0.99f);              // output untouched on failure
    CHECK(dt < std::chrono::milliseconds(50)); // returned promptly, no unbounded spin

    // Fallback keeps the last consistent snapshot.
    CHECK(!snap.readWithFallback(lastGood));
    CHECK(consistent(lastGood) && lastGood.deadzone == 0.10f);

    // Writer finishes -> readers see the new value.
    snap.endWrite();
    CHECK(snap.readWithFallback(lastGood));
    CHECK(consistent(lastGood) && lastGood.deadzone == 0.40f);
}

static void test_concurrent_readers_never_see_torn_data() {
    ParamSnapshot snap;
    snap.publish(make(0.0f));
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<long> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; r++) {
        readers.emplace_back([&] {
            DriftParams lastGood = make(0.0f);
            while (!stop.load()) {
                snap.readWithFallback(lastGood);
                if (!consistent(lastGood)) torn++;
                reads++;
            }
        });
    }
    std::thread writer([&] {
        for (int i = 1; i <= 200000; i++) snap.publish(make((float) (i % 1000) / 1000.0f));
        stop.store(true);
    });
    writer.join();
    for (auto &t : readers) t.join();
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
}

static void test_channel_guard() {
    ChannelGuard g;
    CHECK(g.tryAcquire());
    CHECK(!g.tryAcquire()); // re-entrant / concurrent access is refused, not waited for
    g.release();
    CHECK(g.tryAcquire());
    g.release();
}

int main() {
    test_roundtrip();
    test_interrupted_writer_is_bounded();
    test_concurrent_readers_never_see_torn_data();
    test_channel_guard();
    std::printf("test_sync: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
