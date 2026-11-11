// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "effects.h"

#include <QTest>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// The realtime contract (effects.h): process() doesn't allocate. This test
// watches the C allocator, which operator new also goes through, while a
// Processor runs every effect, and fails if anything allocates. The
// allocator is wrapped for the whole test program, so it's a program of its
// own.

extern "C" {
void *__libc_malloc(std::size_t size);           // NOLINT(bugprone-reserved-identifier)
void *__libc_calloc(std::size_t count, std::size_t size); // NOLINT(bugprone-reserved-identifier)
void *__libc_realloc(void *pointer, std::size_t size);    // NOLINT(bugprone-reserved-identifier)
void *__libc_memalign(std::size_t alignment, std::size_t size); // NOLINT(bugprone-reserved-identifier)
}

namespace {

// The allocator wrappers below need these as plain globals.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
thread_local bool watching = false;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> allocations{0};

void note()
{
    if (watching) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

// Counts what the code in between allocates on this thread.
class Watch
{
public:
    Watch() { watching = true; }
    ~Watch() { watching = false; }
    Watch(const Watch &) = delete;
    Watch &operator=(const Watch &) = delete;
    Watch(Watch &&) = delete;
    Watch &operator=(Watch &&) = delete;
};

} // namespace

// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
extern "C" void *malloc(std::size_t size)
{
    note();
    return __libc_malloc(size);
}
extern "C" void *calloc(std::size_t count, std::size_t size)
{
    note();
    return __libc_calloc(count, size);
}
extern "C" void *realloc(void *pointer, std::size_t size)
{
    note();
    return __libc_realloc(pointer, size);
}
extern "C" void *aligned_alloc(std::size_t alignment, std::size_t size)
{
    note();
    return __libc_memalign(alignment, size);
}
extern "C" void *memalign(std::size_t alignment, std::size_t size)
{
    note();
    return __libc_memalign(alignment, size);
}
extern "C" int posix_memalign(void **out, std::size_t alignment, std::size_t size)
{
    note();
    *out = __libc_memalign(alignment, size);
    return *out == nullptr ? 12 : 0; // ENOMEM
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace pp;

class TestRtAlloc : public QObject
{
    Q_OBJECT

private:
    static constexpr double rate = 48000.0;

    // Blocks of several sizes (odd ones, and PipeWire's largest), with a tone,
    // a key signal and now and then a broken sample, through the processor.
    static void run(Processor &processor, bool keyed)
    {
        static constexpr std::array sizes{128, 480, 1024, 333, maxEffectFrames};
        std::vector<float> left(maxEffectFrames);
        std::vector<float> right(maxEffectFrames);
        std::vector<float> key(maxEffectFrames);
        std::size_t t = 0;
        for (int round = 0; round < 6; ++round) {
            for (const int frames : sizes) {
                for (int i = 0; i < frames; ++i, ++t) {
                    const auto x = static_cast<float>(0.5 * std::sin(2.0 * M_PI * 220.0 * static_cast<double>(t) / rate));
                    left[static_cast<std::size_t>(i)] = x;
                    right[static_cast<std::size_t>(i)] = -x;
                    key[static_cast<std::size_t>(i)] = (t / 4800) % 2 == 0 ? x : 0.0F;
                }
                if (round == 2) {
                    left[7] = std::numeric_limits<float>::quiet_NaN();
                }
                std::array<float *, 2> planes{left.data(), right.data()};
                const Watch watch;
                processor.process(planes.data(), planes.data(), 2, frames, keyed ? key.data() : nullptr);
            }
        }
    }

    // Everything the realtime side does: metering, bypass and back (each
    // effect's resume), idle and back (the chain's resume).
    static int exercise(std::vector<std::unique_ptr<Effect>> effects, bool keyed)
    {
        allocations = 0;
        auto chain = std::make_unique<EffectChain>(std::move(effects), rate, 2);
        auto *raw = chain.get();
        Processor processor;
        processor.setChain(std::move(chain));
        run(processor, keyed);
        processor.setMetering(true);
        run(processor, keyed);
        for (std::size_t e = 0; e < raw->size(); ++e) {
            raw->at(e).enabled = false;
        }
        run(processor, keyed);
        for (std::size_t e = 0; e < raw->size(); ++e) {
            raw->at(e).enabled = true;
        }
        run(processor, keyed);
        processor.setIdle(true);
        run(processor, keyed);
        processor.setIdle(false);
        run(processor, keyed);
        (void)raw->takeLevels(); // main thread: may allocate, not watched
        return allocations.load();
    }

private slots:
    void theWatchSeesAllocations()
    {
        // Through a volatile pointer, so an optimising compiler can't remove
        // the pair (it may drop a new that's deleted at once, and malloc/free).
        static void *(*volatile allocate)(std::size_t) = std::malloc;
        allocations = 0;
        {
            const Watch watch;
            void *p = allocate(64);
            std::free(p); // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        }
        QVERIFY(allocations.load() >= 1);
    }

    void everyEffectRunsWithoutAllocating_data()
    {
        QTest::addColumn<QString>("type");
        for (const auto &type : effectTypes()) {
            QTest::newRow(type.c_str()) << QString::fromStdString(type);
        }
    }
    void everyEffectRunsWithoutAllocating()
    {
        QFETCH(QString, type);
        std::vector<std::unique_ptr<Effect>> effects;
        effects.push_back(makeEffect(type.toStdString()));
        QVERIFY(effects.front() != nullptr);
        const int count = exercise(std::move(effects), effectTakesKey(type.toStdString()));
        QVERIFY2(count == 0, qPrintable(QStringLiteral("%1 allocations in process()").arg(count)));
    }

    void aFullChainRunsWithoutAllocating()
    {
        std::vector<std::unique_ptr<Effect>> effects;
        for (const auto &type : effectTypes()) {
            if (effects.size() < static_cast<std::size_t>(maxEffectsPerStrip)) {
                effects.push_back(makeEffect(type));
            }
        }
        const int count = exercise(std::move(effects), true);
        QVERIFY2(count == 0, qPrintable(QStringLiteral("%1 allocations in process()").arg(count)));
    }

    void noChainRunsWithoutAllocating()
    {
        allocations = 0;
        Processor processor;
        run(processor, false);
        QCOMPARE(allocations.load(), 0);
    }
};

QTEST_GUILESS_MAIN(TestRtAlloc)
#include "test_rtalloc.moc"
