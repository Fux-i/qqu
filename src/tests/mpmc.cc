#include "qqu/mpmc.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace {

struct Pair {
    int first{};
    int second{};
    Pair() = default;
    Pair(int a, int b) : first(a), second(b) {}
};

} // namespace

TEST(mpmc, basic_push_pop) {
    qqu::mpmc<int, 4> queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.full());
    EXPECT_EQ(queue.size(), 0);
    EXPECT_EQ(queue.capacity(), 4);

    ASSERT_TRUE(queue.try_push(1));
    EXPECT_EQ(queue.size(), 1);
    ASSERT_TRUE(queue.try_push(2));
    ASSERT_TRUE(queue.try_push(3));
    ASSERT_TRUE(queue.try_push(4));
    EXPECT_TRUE(queue.full());

    int v{};
    ASSERT_TRUE(queue.try_pop(v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(queue.try_pop(v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(queue.try_pop(v));
    EXPECT_EQ(v, 3);
    ASSERT_TRUE(queue.try_pop(v));
    EXPECT_EQ(v, 4);
    EXPECT_TRUE(queue.empty());
}

TEST(mpmc, blocking_push_pop) {
    qqu::mpmc<int, 2> queue;
    queue.push(42);
    queue.push(43);

    int v{};
    queue.pop(v);
    EXPECT_EQ(v, 42);
    queue.pop(v);
    EXPECT_EQ(v, 43);
}

TEST(mpmc, emplace) {
    qqu::mpmc<Pair, 4> queue;
    ASSERT_TRUE(queue.try_emplace(1, 2));
    Pair v;
    ASSERT_TRUE(queue.try_pop(v));
    EXPECT_EQ(v.first, 1);
    EXPECT_EQ(v.second, 2);
}

TEST(mpmc, multi_producer_single_consumer) {
    constexpr size_t              kProducers = 4;
    constexpr size_t              kMessages  = 10000;
    qqu::mpmc<size_t, 1024>       queue;
    std::atomic<size_t>           consumed{0};
    std::vector<std::thread>      producers;
    std::array<size_t, kProducers> per_producer{};

    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (size_t i = 0; i < kMessages; ++i)
                queue.push(p * kMessages + i);
        });
    }

    auto consumer = std::thread([&] {
        for (size_t i = 0; i < kProducers * kMessages; ++i) {
            size_t v{};
            queue.pop(v);
            ++per_producer[v / kMessages];
            ++consumed;
        }
    });

    for (auto &t : producers)
        t.join();
    consumer.join();

    EXPECT_EQ(consumed.load(), kProducers * kMessages);
    for (size_t count : per_producer)
        EXPECT_EQ(count, kMessages);
}

TEST(mpmc, single_producer_multi_consumer) {
    constexpr size_t         kConsumers = 4;
    constexpr size_t         kMessages  = 10000;
    qqu::mpmc<size_t, 1024>  queue;
    std::atomic<size_t>      produced{0};
    std::vector<std::thread> consumers;
    std::atomic<size_t>      total_consumed{0};

    auto producer = std::thread([&] {
        for (size_t i = 0; i < kMessages; ++i) {
            queue.push(i);
            ++produced;
        }
    });

    for (size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (total_consumed.load(std::memory_order_relaxed) < kMessages) {
                size_t v{};
                if (queue.try_pop(v))
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    producer.join();
    for (auto &t : consumers)
        t.join();

    EXPECT_EQ(produced.load(), kMessages);
    EXPECT_EQ(total_consumed.load(), kMessages);
    EXPECT_TRUE(queue.empty());
}

TEST(mpmc, multi_producer_multi_consumer) {
    constexpr size_t         kProducers = 4;
    constexpr size_t         kConsumers = 4;
    constexpr size_t         kMessages  = 10000;
    qqu::mpmc<size_t, 1024>  queue;
    std::atomic<size_t>      produced{0};
    std::atomic<size_t>      consumed{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (size_t i = 0; i < kMessages; ++i) {
                queue.push(i);
                ++produced;
            }
        });
    }

    for (size_t c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (consumed.load(std::memory_order_relaxed) <
                   kProducers * kMessages) {
                size_t v{};
                if (queue.try_pop(v))
                    consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto &t : producers)
        t.join();
    for (auto &t : consumers)
        t.join();

    EXPECT_EQ(produced.load(), kProducers * kMessages);
    EXPECT_EQ(consumed.load(), kProducers * kMessages);
    EXPECT_TRUE(queue.empty());
}
