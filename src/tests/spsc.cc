#include "qqu.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>

struct stu {
    int         age{};
    std::string name;
};

// Tracks live instances (assignment-based ring still default-constructs slots).
struct Life {
    static int live;
    int        id{-1};

    Life() noexcept {
        ++live;
    }
    explicit Life(int i) noexcept : id(i) {
        ++live;
    }
    Life(const Life &o) noexcept : id(o.id) {
        ++live;
    }
    Life(Life &&o) noexcept : id(o.id) {
        o.id = -1;
        ++live;
    }
    auto operator=(const Life &o) noexcept -> Life & {
        id = o.id;
        return *this;
    }
    auto operator=(Life &&o) noexcept -> Life & {
        id   = o.id;
        o.id = -1;
        return *this;
    }
    ~Life() noexcept {
        --live;
    }
};
int Life::live = 0;

TEST(qqu_test, empty_try_pop) {
    qqu::spsc<stu, 8> q;
    stu               s;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_FALSE(q.full());
    EXPECT_FALSE(q.try_pop(s));
}

TEST(qqu_test, try_push_try_emplace_fifo) {
    qqu::spsc<stu, 8> q;
    stu               s;
    EXPECT_TRUE(q.try_emplace(1, "Amy"));
    EXPECT_TRUE(q.try_push({2, "Bob"}));
    EXPECT_EQ(q.size(), 2u);
    EXPECT_TRUE(q.try_pop(s));
    EXPECT_EQ(s.age, 1);
    EXPECT_EQ(s.name, "Amy");
    EXPECT_TRUE(q.try_pop(s));
    EXPECT_EQ(s.age, 2);
    EXPECT_EQ(s.name, "Bob");
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, full_and_capacity) {
    qqu::spsc<stu, 8> q;
    EXPECT_EQ(q.capacity(), 8u);
    for (int i = 0; i < 8; ++i)
        EXPECT_TRUE(q.try_emplace(i, "x"));
    EXPECT_TRUE(q.full());
    EXPECT_EQ(q.size(), 8u);
    EXPECT_FALSE(q.try_emplace(99, "full"));
    EXPECT_FALSE(q.try_push({99, "full"}));
}

TEST(qqu_test, drain_all) {
    qqu::spsc<stu, 8> q;
    stu               s;
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(q.try_emplace(i, std::to_string(i)));
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.try_pop(s));
        EXPECT_EQ(s.age, i);
        EXPECT_EQ(s.name, std::to_string(i));
    }
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.try_pop(s));
}

TEST(qqu_test, wrap_around) {
    qqu::spsc<int, 4> q;
    int               v;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_TRUE(q.try_push(4));
    EXPECT_FALSE(q.try_push(5));
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_push(5));
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 3);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 4);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 5);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, multi_cycle) {
    qqu::spsc<int, 4> q;
    int               v;
    for (int cycle = 0; cycle < 10; ++cycle) {
        for (int i = 0; i < 4; ++i)
            ASSERT_TRUE(q.try_push(cycle * 10 + i));
        EXPECT_TRUE(q.full());
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(q.try_pop(v));
            EXPECT_EQ(v, cycle * 10 + i);
        }
        EXPECT_TRUE(q.empty());
    }
}

TEST(qqu_test, min_capacity) {
    qqu::spsc<int, 2> q;
    int               v;
    EXPECT_EQ(q.capacity(), 2u);
    EXPECT_TRUE(q.try_push(42));
    EXPECT_TRUE(q.try_push(43));
    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.try_push(1));
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 42);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 43);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, move_only) {
    qqu::spsc<std::unique_ptr<int>, 4> q;
    std::unique_ptr<int>               out;
    EXPECT_TRUE(q.try_push(std::make_unique<int>(7)));
    EXPECT_TRUE(q.try_emplace(std::make_unique<int>(8)));
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(*out, 7);
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(*out, 8);
}

TEST(qqu_test, spsc_stress) {
    constexpr int        n = 100'000;
    qqu::spsc<int, 1024> q;
    std::thread          prod([&] {
        for (int i = 0; i < n;)
            if (q.try_push(i))
                ++i;
    });
    std::thread cons([&] {
        int v, expect = 0;
        while (expect < n) {
            if (q.try_pop(v)) {
                EXPECT_EQ(v, expect);
                ++expect;
            }
        }
    });
    prod.join();
    cons.join();
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, lifetime_ctor_dtor) {
    const int base = Life::live;
    {
        qqu::spsc<Life, 4> q;
        EXPECT_EQ(Life::live, base + 4);
        Life out;
        for (int i = 0; i < 4; ++i)
            ASSERT_TRUE(q.try_emplace(i));
        for (int i = 0; i < 2; ++i) {
            ASSERT_TRUE(q.try_pop(out));
            EXPECT_EQ(out.id, i);
        }
        ASSERT_TRUE(q.try_emplace(4));
        ASSERT_TRUE(q.try_emplace(5));
        // Destroy while 4 live values remain in slots.
    }
    EXPECT_EQ(Life::live, base);
}

TEST(qqu_test, checksum_stress) {
    constexpr int          n          = 200'000;
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    qqu::spsc<int, 256>    q;
    std::atomic<long long> sum{0};
    std::thread            prod([&] {
        for (int i = 0; i < n;)
            if (q.try_push(i))
                ++i;
    });
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    prod.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, soak) {
    constexpr int       n = 1'000'000;
    qqu::spsc<int, 512> q;
    std::thread         prod([&] {
        for (int i = 0; i < n;)
            if (q.try_push(i))
                ++i;
    });
    std::thread cons([&] {
        int v, expect = 0;
        while (expect < n) {
            if (q.try_pop(v)) {
                ASSERT_EQ(v, expect);
                ++expect;
            }
        }
    });
    prod.join();
    cons.join();
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, mostly_full_mostly_empty) {
    auto run = [](int push_bias_pct) {
        qqu::spsc<int, 32> q;
        std::mt19937       rng(static_cast<unsigned>(push_bias_pct));
        int                next_in = 0, next_out = 0, in_q = 0;
        for (int i = 0; i < 100'000; ++i) {
            const bool prefer_push = static_cast<int>(rng() % 100) < push_bias_pct;
            if ((prefer_push || in_q == 0) && in_q < 32) {
                if (q.try_push(next_in)) {
                    ++next_in;
                    ++in_q;
                }
            } else if (in_q > 0) {
                int v;
                if (q.try_pop(v)) {
                    EXPECT_EQ(v, next_out);
                    ++next_out;
                    --in_q;
                }
            }
        }
        int v;
        while (q.try_pop(v)) {
            EXPECT_EQ(v, next_out);
            ++next_out;
            --in_q;
        }
        EXPECT_EQ(in_q, 0);
        EXPECT_EQ(next_in, next_out);
        EXPECT_TRUE(q.empty());
    };
    run(85); // mostly full
    run(15); // mostly empty
}

TEST(qqu_test, try_emplace_fail_preserves_rvalue) {
    qqu::spsc<std::unique_ptr<int>, 2> q;
    ASSERT_TRUE(q.try_push(std::make_unique<int>(1)));
    ASSERT_TRUE(q.try_push(std::make_unique<int>(2)));
    auto p = std::make_unique<int>(99);
    EXPECT_FALSE(q.try_emplace(std::move(p)));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 99);
    auto qv = std::make_unique<int>(100);
    EXPECT_FALSE(q.try_push(std::move(qv)));
    ASSERT_NE(qv, nullptr);
    EXPECT_EQ(*qv, 100);
}
