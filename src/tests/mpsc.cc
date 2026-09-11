#include "mpsc.h"
#include "mpsc_fetchadd.h"
#include "mpsc_remap.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

struct stu {
    int  age{};
    char name[8]{};
};

TEST(qqu_variants, fetchadd_full_does_not_abandon_ticket) {
    qqu::mpsc_fetchadd<int, 2> queue;
    ASSERT_TRUE(queue.try_push(1));
    ASSERT_TRUE(queue.try_push(2));
    ASSERT_FALSE(queue.try_push(3));
    int output{};
    ASSERT_TRUE(queue.try_pop(output));
    EXPECT_EQ(output, 1);
    ASSERT_TRUE(queue.try_push(3));
    ASSERT_TRUE(queue.try_pop(output));
    EXPECT_EQ(output, 2);
    ASSERT_TRUE(queue.try_pop(output));
    EXPECT_EQ(output, 3);
}

TEST(qqu_variants, remap_slot_sequences) {
    auto check = []<class T, size_t Capacity>() {
        qqu::mpsc_remap<T, Capacity> queue;
        for (size_t cycle = 0; cycle < 3; ++cycle) {
            for (size_t index = 0; index < Capacity; ++index) {
                T message{};
                message[0] = cycle * Capacity + index;
                ASSERT_TRUE(queue.try_push(message));
            }
            T output{};
            for (size_t index = 0; index < Capacity; ++index) {
                ASSERT_TRUE(queue.try_pop(output));
                EXPECT_EQ(output[0], cycle * Capacity + index);
            }
            EXPECT_TRUE(queue.empty());
        }
    };
    check.template operator()<std::array<uint32_t, 1>, 2>();
    check.template operator()<std::array<uint32_t, 1>, 64>();
    check.template operator()<std::array<uint64_t, 8>, 64>();
}

TEST(qqu_variants, mixed_producers_wrap_around) {
    auto check = []<class Queue>() {
        constexpr int messages = 10'000;
        constexpr int producer_count = 4;
        Queue queue;
        std::vector<std::jthread> producers;
        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                for (int index = 0; index < messages; ++index) {
                    const int message = producer * messages + index;
                    if (producer % 2 == 0)
                        queue.push(message);
                    else
                        while (!queue.try_push(message))
                            _mm_pause();
                }
            });
        }
        std::vector<int> seen(messages * producer_count);
        for (int index = 0; index < messages * producer_count; ++index) {
            int message{};
            queue.pop(message);
            EXPECT_GE(message, 0);
            EXPECT_LT(message, messages * producer_count);
            if (message >= 0 && message < messages * producer_count)
                ++seen[message];
        }
        producers.clear();
        for (int count : seen)
            EXPECT_EQ(count, 1);
        EXPECT_TRUE(queue.empty());
    };
    check.template operator()<qqu::mpsc_fetchadd<int, 4>>();
    check.template operator()<qqu::mpsc_remap<int, 4>>();
}

TEST(qqu_test, empty_try_pop) {
    qqu::mpsc<stu, 8> q;
    stu               s;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    EXPECT_FALSE(q.full());
    EXPECT_FALSE(q.try_pop(s));
}

TEST(qqu_test, try_push_try_emplace_fifo) {
    qqu::mpsc<stu, 8> q;
    stu               s;
    EXPECT_TRUE(q.try_push({1, "Amy"}));
    EXPECT_TRUE(q.try_push({2, "Bob"}));
    EXPECT_EQ(q.size(), 2u);
    EXPECT_TRUE(q.try_pop(s));
    EXPECT_EQ(s.age, 1);
    EXPECT_STREQ(s.name, "Amy");
    EXPECT_TRUE(q.try_pop(s));
    EXPECT_EQ(s.age, 2);
    EXPECT_STREQ(s.name, "Bob");
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, full_and_capacity) {
    qqu::mpsc<stu, 8> q;
    EXPECT_EQ(q.capacity(), 8u);
    for (int i = 0; i < 8; ++i)
        EXPECT_TRUE(q.try_push({i, "x"}));
    EXPECT_TRUE(q.full());
    EXPECT_EQ(q.size(), 8u);
    EXPECT_FALSE(q.try_push({99, "full"}));
}

TEST(qqu_test, drain_all) {
    qqu::mpsc<stu, 8> q;
    stu               s;
    for (int i = 0; i < 8; ++i)
        ASSERT_TRUE(q.try_push({i, "x"}));
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(q.try_pop(s));
        EXPECT_EQ(s.age, i);
    }
    EXPECT_TRUE(q.empty());
    EXPECT_FALSE(q.try_pop(s));
}

TEST(qqu_test, wrap_around) {
    qqu::mpsc<int, 4> q;
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
    qqu::mpsc<int, 4> q;
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
    qqu::mpsc<int, 2> q;
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

TEST(qqu_test, spsc_stress) {
    constexpr int        n = 100'000;
    qqu::mpsc<int, 1024> q;
    std::thread          prod([&] {
        for (int i = 0; i < n;)
            if (q.try_push(i))
                ++i;
    });
    std::thread          cons([&] {
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

TEST(qqu_test, checksum_stress) {
    constexpr int          n          = 200'000;
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    qqu::mpsc<int, 256>    q;
    std::atomic<long long> sum{0};
    std::thread            prod([&] {
        for (int i = 0; i < n;)
            if (q.try_push(i))
                ++i;
    });
    std::thread            cons([&] {
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
    qqu::mpsc<int, 512> q;
    std::thread         prod([&] {
        for (int i = 0; i < n;)
            if (q.try_push(i))
                ++i;
    });
    std::thread         cons([&] {
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
        qqu::mpsc<int, 32> q;
        std::mt19937       rng(static_cast<unsigned>(push_bias_pct));
        int                next_in = 0, next_out = 0, in_q = 0;
        for (int i = 0; i < 100'000; ++i) {
            const bool prefer_push =
                static_cast<int>(rng() % 100) < push_bias_pct;
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
    run(85);
    run(15);
}

TEST(qqu_test, blocking_push_pop) {
    qqu::mpsc<int, 4> q;
    int               v;
    q.push(1);
    q.push(2);
    q.pop(v);
    EXPECT_EQ(v, 1);
    q.pop(v);
    EXPECT_EQ(v, 2);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_test, mpsc_stress) {
    constexpr int        n_prod = 4;
    constexpr int        n_each = 50'000;
    constexpr int        n      = n_prod * n_each;
    qqu::mpsc<int, 1024> q;
    std::vector<char>    seen(static_cast<size_t>(n), 0);
    std::thread          prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int v, got = 0;
        while (got < n) {
            if (q.try_pop(v)) {
                ASSERT_GE(v, 0);
                ASSERT_LT(v, n);
                EXPECT_EQ(seen[static_cast<size_t>(v)], 0);
                seen[static_cast<size_t>(v)] = 1;
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_TRUE(q.empty());
    for (char c : seen)
        EXPECT_EQ(c, 1);
}

TEST(qqu_test, mpsc_checksum) {
    constexpr int          n_prod     = 8;
    constexpr int          n_each     = 25'000;
    constexpr int          n          = n_prod * n_each;
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    qqu::mpsc<int, 256>    q;
    std::atomic<long long> sum{0};
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_2prod_soak) {
    constexpr int          n_prod = 2;
    constexpr int          n_each = 5'000'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 1024>   q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_4prod_soak) {
    constexpr int          n_prod = 4;
    constexpr int          n_each = 2'500'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 1024>   q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_8prod_soak) {
    constexpr int          n_prod = 8;
    constexpr int          n_each = 1'250'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 1024>   q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_edge_cap2) {
    constexpr int          n_prod = 4;
    constexpr int          n_each = 1'250'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 2>      q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_edge_cap65536) {
    constexpr int          n_prod = 4;
    constexpr int          n_each = 1'250'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 65536>  q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_mixed_contention) {
    constexpr int          n_prod = 4;
    constexpr int          n_each = 500'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 512>    q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            std::mt19937 rng(static_cast<unsigned>(p + 100));
            const int    base = p * n_each;
            for (int i = 0; i < n_each;) {
                if (q.try_push(base + i))
                    ++i;
                else if (rng() % 10 < 3)
                    for (int j = 0; j < 5; ++j)
                        _mm_pause();
            }
        });
    }
    std::thread cons([&] {
        std::mt19937 rng(200);
        int          got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
                if (rng() % 20 < 3)
                    for (int j = 0; j < 3; ++j)
                        _mm_pause();
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_unbalanced_prods) {
    constexpr int          n_prod = 4;
    constexpr int          n_each = 500'000;
    constexpr int          n      = n_prod * n_each;
    qqu::mpsc<int, 256>    q;
    std::atomic<long long> sum{0};
    constexpr long long    expect_sum = static_cast<long long>(n) * (n - 1) / 2;
    std::thread            prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;) {
                if (q.try_push(base + i))
                    ++i;
                else {
                    const int pause_count = (p % 2 == 0) ? 15 : 3;
                    for (int j = 0; j < pause_count; ++j)
                        _mm_pause();
                }
            }
        });
    }
    std::thread cons([&] {
        int got = 0, v;
        while (got < n) {
            if (q.try_pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_EQ(sum.load(), expect_sum);
    EXPECT_TRUE(q.empty());
}

TEST(qqu_stress, mpsc_16prod) {
    constexpr int        n_prod = 16;
    constexpr int        n_each = 100'000;
    constexpr int        n      = n_prod * n_each;
    qqu::mpsc<int, 2048> q;
    std::vector<char>    seen(static_cast<size_t>(n), 0);
    std::thread          prods[n_prod];
    for (int p = 0; p < n_prod; ++p) {
        prods[p] = std::thread([&, p] {
            const int base = p * n_each;
            for (int i = 0; i < n_each;)
                if (q.try_push(base + i))
                    ++i;
        });
    }
    std::thread cons([&] {
        int v, got = 0;
        while (got < n) {
            if (q.try_pop(v)) {
                ASSERT_GE(v, 0);
                ASSERT_LT(v, n);
                EXPECT_EQ(seen[static_cast<size_t>(v)], 0);
                seen[static_cast<size_t>(v)] = 1;
                ++got;
            }
        }
    });
    for (auto &t : prods)
        t.join();
    cons.join();
    EXPECT_TRUE(q.empty());
    for (char c : seen)
        EXPECT_EQ(c, 1);
}
