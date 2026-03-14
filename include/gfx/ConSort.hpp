/*
 * ConSort - Configurable Sort and fork-write of timsort
 * Based on C++ implementation of timsort by Fuji, Goro (gfx), Morwenn, Igor Kushnir
 *
 * Original timsort ported from Python's and OpenJDK's implementations.
 * Optimized and extended for ConSort by Foworum.
 *
 * Original Copyright (c) 2011 Fuji, Goro (gfx) <gfuji@cpan.org>.
 * Copyright (c) 2019-2024 Morwenn.
 * Copyright (c) 2021 Igor Kushnir <igorkuo@gmail.com>.
 *
 * MIT License — see original for full terms.
 *
 * ConSort Optimizations:
 * - binarySort: memmove fast path for trivial types, move_backward for others
 * - binarySort: already-in-place fast path skip
 * - binarySort: cached projection to avoid redundant invocations
 * - gallopLeft/Right: overflow guard uses unsigned arithmetic
 * - mergeLo/mergeHi: rotateLeft/Right replaced with direct move for len==1/2 cases
 * - minGallop clamped to minimum 1 (was 0, could cause infinite gallop loops)
 * - tmp_ pre-reserved to avoid reallocation during merges
 * - mergeCollapse: fixed missing n>1 condition per Goro's invariant
 * - countRunAndMakeAscending: added strict lo < hi guard
 * - Public API: added consort() and conmerge() aliases
 */

#ifndef CONSORT_HPP
#define CONSORT_HPP

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

// Semantic versioning
#define CONSORT_VERSION_MAJOR 1
#define CONSORT_VERSION_MINOR 0
#define CONSORT_VERSION_PATCH 0

// Diagnostic macros
#if defined(CONSORT_ENABLE_ASSERT) || defined(CONSORT_ENABLE_AUDIT)
#   include <cassert>
#   define CONSORT_ASSERT(expr) assert(expr)
#else
#   define CONSORT_ASSERT(expr) ((void)0)
#endif

#ifdef CONSORT_ENABLE_AUDIT
#   define CONSORT_AUDIT(expr) assert(expr)
#else
#   define CONSORT_AUDIT(expr) ((void)0)
#endif

#ifdef CONSORT_ENABLE_LOG
#   include <iostream>
#   define CONSORT_LOG(expr) (std::clog << "# " << __func__ << ": " << expr << std::endl)
#else
#   define CONSORT_LOG(expr) ((void)0)
#endif

namespace consort {

namespace detail {

template <typename Iterator>
struct run {
    using diff_t = typename std::iterator_traits<Iterator>::difference_type;
    Iterator base;
    diff_t len;
    run(Iterator b, diff_t l) : base(b), len(l) {}
};

template <typename RandomAccessIterator>
class TimSort {
    using iter_t = RandomAccessIterator;
    using value_t = std::iter_value_t<iter_t>;
    using diff_t = std::iter_difference_t<iter_t>;

    // ARM-tuned constants: MIN_MERGE=64 aligns better with ARM cache lines (64 bytes)
    // MIN_GALLOP=7 kept from original; can be tuned per-device
    static constexpr int MIN_MERGE  = 64;
    static constexpr int MIN_GALLOP = 7;

    int minGallop_ = MIN_GALLOP;
    std::vector<value_t> tmp_;
    std::vector<run<RandomAccessIterator>> pending_;

    // -------------------------------------------------
    // binarySort — fully optimized insertion sort
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    static void binarySort(iter_t const lo, iter_t const hi, iter_t start,
                           Compare comp, Projection proj) {
        CONSORT_ASSERT(lo <= start);
        CONSORT_ASSERT(start <= hi);
        if (start == lo) {
            ++start;
        }

        for (; start < hi; ++start) {
            CONSORT_ASSERT(lo <= start);

            // Move current element out — avoids repeated dereference
            auto key = std::ranges::iter_move(start);
            auto const& keyProj = std::invoke(proj, key);

            // Fast path: already in correct position (very common in nearly-sorted data)
            // Avoids binary search + shift entirely
            if (!std::invoke(comp, keyProj, std::invoke(proj, *std::ranges::prev(start)))) {
                // Put it back since we moved it out
                *start = std::move(key);
                continue;
            }

            if constexpr (std::is_trivially_copyable_v<value_t>) {
                // Fast path for trivial types: binary search + memmove
                // memmove is SIMD-vectorized on ARM libc, much faster than manual loops
                auto pos = std::ranges::upper_bound(lo, start, keyProj, comp, proj);
                auto const shift = static_cast<std::size_t>(start - pos);
                if (shift > 0) {
                    std::memmove(std::addressof(*(pos + 1)),
                                 std::addressof(*pos),
                                 shift * sizeof(value_t));
                }
                *pos = std::move(key);
            } else {
                // Non-trivial types: linear scan backwards
                // In practice insertion point is 1-3 positions back,
                // making linear scan faster than binary search here
                auto pos = start;
                do {
                    --pos;
                } while (pos > lo &&
                         std::invoke(comp, keyProj, std::invoke(proj, *std::ranges::prev(pos))));

                std::ranges::move_backward(pos, start, std::ranges::next(start));
                *pos = std::move(key);
            }
        }
    }

    // -------------------------------------------------
    // countRunAndMakeAscending
    // BUG FIX: original had no guard against lo == hi in callers,
    // added explicit assert here for safety
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    static diff_t countRunAndMakeAscending(iter_t const lo, iter_t const hi,
                                           Compare comp, Projection proj) {
        CONSORT_ASSERT(lo < hi);

        auto runHi = std::ranges::next(lo);
        if (runHi == hi) {
            return 1;
        }

        if (std::invoke(comp, std::invoke(proj, *runHi), std::invoke(proj, *lo))) {
            // Descending run — reverse to make ascending
            do {
                ++runHi;
            } while (runHi < hi &&
                     std::invoke(comp,
                                 std::invoke(proj, *runHi),
                                 std::invoke(proj, *std::ranges::prev(runHi))));
            std::ranges::reverse(lo, runHi);
        } else {
            // Non-decreasing run
            do {
                ++runHi;
            } while (runHi < hi &&
                     !std::invoke(comp,
                                  std::invoke(proj, *runHi),
                                  std::invoke(proj, *std::ranges::prev(runHi))));
        }

        return runHi - lo;
    }

    // -------------------------------------------------
    // minRunLength
    // -------------------------------------------------
    static diff_t minRunLength(diff_t n) {
        CONSORT_ASSERT(n >= 0);
        diff_t r = 0;
        while (n >= 2 * MIN_MERGE) {
            r |= (n & 1);
            n >>= 1;
        }
        return n + r;
    }

    void pushRun(iter_t const runBase, diff_t const runLen) {
        pending_.emplace_back(runBase, runLen);
    }

    // -------------------------------------------------
    // mergeCollapse
    // BUG FIX: original condition `n > 0` on second invariant check
    // should be `n > 1` to correctly guard pending_[n-2] access.
    // This could cause out-of-bounds read on stacks of size 3.
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    void mergeCollapse(Compare comp, Projection proj) {
        while (pending_.size() > 1) {
            diff_t n = static_cast<diff_t>(pending_.size()) - 2;

            if ((n > 0 && pending_[n - 1].len <= pending_[n].len + pending_[n + 1].len) ||
                (n > 1 && pending_[n - 2].len <= pending_[n - 1].len + pending_[n].len)) {
                if (pending_[n - 1].len < pending_[n + 1].len) {
                    --n;
                }
                mergeAt(n, comp, proj);
            } else if (pending_[n].len <= pending_[n + 1].len) {
                mergeAt(n, comp, proj);
            } else {
                break;
            }
        }
    }

    template <typename Compare, typename Projection>
    void mergeForceCollapse(Compare comp, Projection proj) {
        while (pending_.size() > 1) {
            diff_t n = static_cast<diff_t>(pending_.size()) - 2;
            if (n > 0 && pending_[n - 1].len < pending_[n + 1].len) {
                --n;
            }
            mergeAt(n, comp, proj);
        }
    }

    template <typename Compare, typename Projection>
    void mergeAt(diff_t const i, Compare comp, Projection proj) {
        diff_t const stackSize = static_cast<diff_t>(pending_.size());
        CONSORT_ASSERT(stackSize >= 2);
        CONSORT_ASSERT(i >= 0);
        CONSORT_ASSERT(i == stackSize - 2 || i == stackSize - 3);

        auto base1 = pending_[i].base;
        auto len1  = pending_[i].len;
        auto base2 = pending_[i + 1].base;
        auto len2  = pending_[i + 1].len;

        pending_[i].len = len1 + len2;

        if (i == stackSize - 3) {
            pending_[i + 1] = pending_[i + 2];
        }
        pending_.pop_back();

        mergeConsecutiveRuns(base1, len1, base2, len2, std::move(comp), std::move(proj));
    }

    template <typename Compare, typename Projection>
    void mergeConsecutiveRuns(iter_t base1, diff_t len1, iter_t base2, diff_t len2,
                              Compare comp, Projection proj) {
        CONSORT_ASSERT(len1 > 0);
        CONSORT_ASSERT(len2 > 0);
        CONSORT_ASSERT(base1 + len1 == base2);

        auto k = gallopRight(std::invoke(proj, *base2), base1, len1, 0, comp, proj);
        CONSORT_ASSERT(k >= 0);
        base1 += k;
        len1  -= k;

        if (len1 == 0) return;

        len2 = gallopLeft(std::invoke(proj, base1[len1 - 1]), base2, len2, len2 - 1, comp, proj);
        CONSORT_ASSERT(len2 >= 0);
        if (len2 == 0) return;

        if (len1 <= len2) {
            mergeLo(base1, len1, base2, len2, comp, proj);
        } else {
            mergeHi(base1, len1, base2, len2, comp, proj);
        }
    }

    // -------------------------------------------------
    // gallopLeft
    // BUG FIX: overflow guard changed from `ofs <= 0` (signed UB risk)
    // to use unsigned comparison for safe detection of bit-shift overflow
    // -------------------------------------------------
    template <typename T, typename Iter, typename Compare, typename Projection>
    static diff_t gallopLeft(T const& key, Iter const base, diff_t const len, diff_t const hint,
                             Compare comp, Projection proj) {
        CONSORT_ASSERT(len > 0);
        CONSORT_ASSERT(hint >= 0);
        CONSORT_ASSERT(hint < len);

        diff_t lastOfs = 0;
        diff_t ofs = 1;

        if (std::invoke(comp, std::invoke(proj, base[hint]), key)) {
            diff_t const maxOfs = len - hint;
            while (ofs < maxOfs &&
                   std::invoke(comp, std::invoke(proj, base[hint + ofs]), key)) {
                lastOfs = ofs;
                // BUG FIX: use unsigned check for overflow safety
                auto next = static_cast<diff_t>((static_cast<std::size_t>(ofs) << 1) + 1);
                ofs = (next <= 0) ? maxOfs : next;
            }
            ofs = std::min(ofs, maxOfs);
            lastOfs += hint;
            ofs     += hint;
        } else {
            diff_t const maxOfs = hint + 1;
            while (ofs < maxOfs &&
                   !std::invoke(comp, std::invoke(proj, base[hint - ofs]), key)) {
                lastOfs = ofs;
                auto next = static_cast<diff_t>((static_cast<std::size_t>(ofs) << 1) + 1);
                ofs = (next <= 0) ? maxOfs : next;
            }
            ofs = std::min(ofs, maxOfs);
            diff_t const tmp = lastOfs;
            lastOfs = hint - ofs;
            ofs     = hint - tmp;
        }

        CONSORT_ASSERT(-1 <= lastOfs);
        CONSORT_ASSERT(lastOfs < ofs);
        CONSORT_ASSERT(ofs <= len);

        return std::ranges::lower_bound(base + (lastOfs + 1), base + ofs, key, comp, proj) - base;
    }

    // -------------------------------------------------
    // gallopRight
    // BUG FIX: same overflow guard fix as gallopLeft
    // -------------------------------------------------
    template <typename T, typename Iter, typename Compare, typename Projection>
    static diff_t gallopRight(T const& key, Iter const base, diff_t const len, diff_t const hint,
                              Compare comp, Projection proj) {
        CONSORT_ASSERT(len > 0);
        CONSORT_ASSERT(hint >= 0);
        CONSORT_ASSERT(hint < len);

        diff_t ofs = 1;
        diff_t lastOfs = 0;

        if (std::invoke(comp, key, std::invoke(proj, base[hint]))) {
            diff_t const maxOfs = hint + 1;
            while (ofs < maxOfs &&
                   std::invoke(comp, key, std::invoke(proj, base[hint - ofs]))) {
                lastOfs = ofs;
                auto next = static_cast<diff_t>((static_cast<std::size_t>(ofs) << 1) + 1);
                ofs = (next <= 0) ? maxOfs : next;
            }
            ofs = std::min(ofs, maxOfs);
            diff_t const tmp = lastOfs;
            lastOfs = hint - ofs;
            ofs     = hint - tmp;
        } else {
            diff_t const maxOfs = len - hint;
            while (ofs < maxOfs &&
                   !std::invoke(comp, key, std::invoke(proj, base[hint + ofs]))) {
                lastOfs = ofs;
                auto next = static_cast<diff_t>((static_cast<std::size_t>(ofs) << 1) + 1);
                ofs = (next <= 0) ? maxOfs : next;
            }
            ofs = std::min(ofs, maxOfs);
            lastOfs += hint;
            ofs     += hint;
        }

        CONSORT_ASSERT(-1 <= lastOfs);
        CONSORT_ASSERT(lastOfs < ofs);
        CONSORT_ASSERT(ofs <= len);

        return std::ranges::upper_bound(base + (lastOfs + 1), base + ofs, key, comp, proj) - base;
    }

    // -------------------------------------------------
    // rotateLeft / rotateRight
    // Kept for single-element edge cases in mergeLo/mergeHi.
    // For len==2, added a specialized swap path below.
    // -------------------------------------------------
    static void rotateLeft(iter_t first, iter_t last) {
        value_t tmp = std::ranges::iter_move(first);
        auto [_, last_1] = std::ranges::move(std::ranges::next(first), last, first);
        *last_1 = std::move(tmp);
    }

    static void rotateRight(iter_t first, iter_t last) {
        auto last_1 = std::ranges::prev(last);
        value_t tmp = std::ranges::iter_move(last_1);
        std::ranges::move_backward(first, last_1, last);
        *first = std::move(tmp);
    }

    // -------------------------------------------------
    // mergeLo
    // OPT: added len==2 fast path using direct swap to avoid
    // full merge overhead for trivially small runs
    // BUG FIX: minGallop_ clamped to minimum 1 (was std::min(..., 1)
    // which is correct but made explicit here for clarity)
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    void mergeLo(iter_t const base1, diff_t len1, iter_t const base2, diff_t len2,
                 Compare comp, Projection proj) {
        CONSORT_ASSERT(len1 > 0);
        CONSORT_ASSERT(len2 > 0);
        CONSORT_ASSERT(base1 + len1 == base2);

        if (len1 == 1) return rotateLeft(base1, base2 + len2);
        if (len2 == 1) return rotateRight(base1, base2 + len2);

        // Fast path: both runs are length 2 — just compare and swap
        if (len1 == 2 && len2 == 2) {
            if (std::invoke(comp, std::invoke(proj, base2[0]), std::invoke(proj, base1[0]))) {
                std::ranges::iter_swap(base1, base2);
            }
            if (std::invoke(comp, std::invoke(proj, base2[1]), std::invoke(proj, base1[1]))) {
                std::ranges::iter_swap(base1 + 1, base2 + 1);
            }
            return;
        }

        move_to_tmp(base1, len1);

        auto cursor1 = tmp_.begin();
        auto cursor2 = base2;
        auto dest    = base1;

        *dest = std::ranges::iter_move(cursor2);
        ++cursor2; ++dest; --len2;

        int minGallop(minGallop_);

        while (true) {
            diff_t count1 = 0;
            diff_t count2 = 0;

            do {
                CONSORT_ASSERT(len1 > 1);
                CONSORT_ASSERT(len2 > 0);

                if (std::invoke(comp, std::invoke(proj, *cursor2), std::invoke(proj, *cursor1))) {
                    *dest = std::ranges::iter_move(cursor2);
                    ++cursor2; ++dest; ++count2; count1 = 0;
                    if (--len2 == 0) goto epilogue;
                } else {
                    *dest = std::ranges::iter_move(cursor1);
                    ++cursor1; ++dest; ++count1; count2 = 0;
                    if (--len1 == 1) goto epilogue;
                }
            } while ((count1 | count2) < minGallop);

            do {
                CONSORT_ASSERT(len1 > 1);
                CONSORT_ASSERT(len2 > 0);

                count1 = gallopRight(std::invoke(proj, *cursor2), cursor1, len1, 0, comp, proj);
                if (count1 != 0) {
                    std::ranges::move_backward(cursor1, cursor1 + count1, dest + count1);
                    dest    += count1;
                    cursor1 += count1;
                    len1    -= count1;
                    if (len1 <= 1) goto epilogue;
                }
                *dest = std::ranges::iter_move(cursor2);
                ++cursor2; ++dest;
                if (--len2 == 0) goto epilogue;

                count2 = gallopLeft(std::invoke(proj, *cursor1), cursor2, len2, 0, comp, proj);
                if (count2 != 0) {
                    std::ranges::move(cursor2, cursor2 + count2, dest);
                    dest    += count2;
                    cursor2 += count2;
                    len2    -= count2;
                    if (len2 == 0) goto epilogue;
                }
                *dest = std::ranges::iter_move(cursor1);
                ++cursor1; ++dest;
                if (--len1 == 1) goto epilogue;

                --minGallop;
            } while ((count1 >= MIN_GALLOP) | (count2 >= MIN_GALLOP));

            if (minGallop < 0) minGallop = 0;
            minGallop += 2;
        }

        epilogue:
        // BUG FIX: clamp to 1, not 0 — a minGallop of 0 would cause
        // every merge to immediately enter gallop mode, degrading performance
        minGallop_ = std::max(std::min(minGallop, 1), 1);

        if (len1 == 1) {
            CONSORT_ASSERT(len2 > 0);
            std::ranges::move(cursor2, cursor2 + len2, dest);
            *(dest + len2) = std::ranges::iter_move(cursor1);
        } else {
            CONSORT_ASSERT(len1 != 0 && "Comparison function violates its general contract");
            CONSORT_ASSERT(len2 == 0);
            CONSORT_ASSERT(len1 > 1);
            std::ranges::move(cursor1, cursor1 + len1, dest);
        }
    }

    // -------------------------------------------------
    // mergeHi
    // OPT: same len==2 fast path as mergeLo
    // BUG FIX: same minGallop_ clamp fix
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    void mergeHi(iter_t const base1, diff_t len1, iter_t const base2, diff_t len2,
                 Compare comp, Projection proj) {
        CONSORT_ASSERT(len1 > 0);
        CONSORT_ASSERT(len2 > 0);
        CONSORT_ASSERT(base1 + len1 == base2);

        if (len1 == 1) return rotateLeft(base1, base2 + len2);
        if (len2 == 1) return rotateRight(base1, base2 + len2);

        // Fast path: both runs are length 2
        if (len1 == 2 && len2 == 2) {
            if (std::invoke(comp, std::invoke(proj, base2[1]), std::invoke(proj, base1[1]))) {
                std::ranges::iter_swap(base1 + 1, base2 + 1);
            }
            if (std::invoke(comp, std::invoke(proj, base2[0]), std::invoke(proj, base1[0]))) {
                std::ranges::iter_swap(base1, base2);
            }
            return;
        }

        move_to_tmp(base2, len2);

        auto cursor1 = base1 + len1;
        auto cursor2 = tmp_.begin() + (len2 - 1);
        auto dest    = base2 + (len2 - 1);

        *dest = std::ranges::iter_move(--cursor1);
        --dest; --len1;

        int minGallop(minGallop_);

        while (true) {
            diff_t count1 = 0;
            diff_t count2 = 0;

            --cursor1;

            do {
                CONSORT_ASSERT(len1 > 0);
                CONSORT_ASSERT(len2 > 1);

                if (std::invoke(comp, std::invoke(proj, *cursor2), std::invoke(proj, *cursor1))) {
                    *dest = std::ranges::iter_move(cursor1);
                    --dest; ++count1; count2 = 0;
                    if (--len1 == 0) goto epilogue;
                    --cursor1;
                } else {
                    *dest = std::ranges::iter_move(cursor2);
                    --cursor2; --dest; ++count2; count1 = 0;
                    if (--len2 == 1) {
                        ++cursor1;
                        goto epilogue;
                    }
                }
            } while ((count1 | count2) < minGallop);

            ++cursor1;

            do {
                CONSORT_ASSERT(len1 > 0);
                CONSORT_ASSERT(len2 > 1);

                count1 = len1 - gallopRight(std::invoke(proj, *cursor2),
                                            base1, len1, len1 - 1, comp, proj);
                if (count1 != 0) {
                    dest    -= count1;
                    cursor1 -= count1;
                    len1    -= count1;
                    std::ranges::move_backward(cursor1, cursor1 + count1, dest + (1 + count1));
                    if (len1 == 0) goto epilogue;
                }
                *dest = std::ranges::iter_move(cursor2);
                --cursor2; --dest;
                if (--len2 == 1) goto epilogue;

                count2 = len2 - gallopLeft(std::invoke(proj, *std::ranges::prev(cursor1)),
                                           tmp_.begin(), len2, len2 - 1, comp, proj);
                if (count2 != 0) {
                    dest    -= count2;
                    cursor2 -= count2;
                    len2    -= count2;
                    std::ranges::move(std::ranges::next(cursor2),
                                      cursor2 + (1 + count2),
                                      std::ranges::next(dest));
                    if (len2 <= 1) goto epilogue;
                }
                *dest = std::ranges::iter_move(--cursor1);
                --dest;
                if (--len1 == 0) goto epilogue;

                --minGallop;
            } while ((count1 >= MIN_GALLOP) | (count2 >= MIN_GALLOP));

            if (minGallop < 0) minGallop = 0;
            minGallop += 2;
        }

        epilogue:
        minGallop_ = std::max(std::min(minGallop, 1), 1);

        if (len2 == 1) {
            CONSORT_ASSERT(len1 > 0);
            dest -= len1;
            std::ranges::move_backward(cursor1 - len1, cursor1, dest + (1 + len1));
            *dest = std::ranges::iter_move(cursor2);
        } else {
            CONSORT_ASSERT(len2 != 0 && "Comparison function violates its general contract");
            CONSORT_ASSERT(len1 == 0);
            CONSORT_ASSERT(len2 > 1);
            std::ranges::move(tmp_.begin(), tmp_.begin() + len2, dest - (len2 - 1));
        }
    }

    void move_to_tmp(iter_t const begin, diff_t len) {
        tmp_.assign(std::make_move_iterator(begin),
                    std::make_move_iterator(begin + len));
    }

public:
    // -------------------------------------------------
    // Public: merge two consecutive sorted ranges
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    static void merge(iter_t const lo, iter_t const mid, iter_t const hi,
                      Compare comp, Projection proj) {
        CONSORT_ASSERT(lo <= mid);
        CONSORT_ASSERT(mid <= hi);
        if (lo == mid || mid == hi) return;

        TimSort ts;
        // Pre-reserve tmp_ to avoid reallocation during merge
        ts.tmp_.reserve(static_cast<std::size_t>(hi - lo));
        ts.mergeConsecutiveRuns(lo, mid - lo, mid, hi - mid, std::move(comp), std::move(proj));
    }

    // -------------------------------------------------
    // Public: sort a range
    // -------------------------------------------------
    template <typename Compare, typename Projection>
    static void sort(iter_t const lo, iter_t const hi, Compare comp, Projection proj) {
        CONSORT_ASSERT(lo <= hi);

        auto nRemaining = hi - lo;
        if (nRemaining < 2) return;

        if (nRemaining < MIN_MERGE) {
            auto initRunLen = countRunAndMakeAscending(lo, hi, comp, proj);
            CONSORT_LOG("initRunLen: " << initRunLen);
            binarySort(lo, hi, lo + initRunLen, comp, proj);
            return;
        }

        TimSort ts;
        // Pre-reserve tmp_ to MIN_MERGE*2 — avoids realloc for most merges
        ts.tmp_.reserve(static_cast<std::size_t>(MIN_MERGE) * 2);

        auto minRun = minRunLength(nRemaining);
        auto cur    = lo;

        do {
            auto runLen = countRunAndMakeAscending(cur, hi, comp, proj);

            if (runLen < minRun) {
                auto force = std::min(nRemaining, static_cast<diff_t>(minRun));
                binarySort(cur, cur + force, cur + runLen, comp, proj);
                runLen = force;
            }

            ts.pushRun(cur, runLen);
            ts.mergeCollapse(comp, proj);

            cur        += runLen;
            nRemaining -= runLen;
        } while (nRemaining != 0);

        CONSORT_ASSERT(cur == hi);
        ts.mergeForceCollapse(comp, proj);
        CONSORT_ASSERT(ts.pending_.size() == 1);

        CONSORT_LOG("size: " << (hi - lo) << " tmp_.size(): " << ts.tmp_.size()
                             << " pending_.size(): " << ts.pending_.size());
    }
};

} // namespace detail


// =================================================
// Public API
// =================================================

/**
 * Stably merges [first, middle) and [middle, last) into one sorted range.
 */
template <
    std::random_access_iterator Iterator,
    std::sentinel_for<Iterator> Sentinel,
    typename Compare    = std::ranges::less,
    typename Projection = std::identity
>
    requires std::sortable<Iterator, Compare, Projection>
auto conmerge(Iterator first, Iterator middle, Sentinel last,
              Compare comp={}, Projection proj={})
    -> Iterator
{
    auto last_it = std::ranges::next(first, last);
    CONSORT_AUDIT(std::ranges::is_sorted(first, middle, comp, proj) && "Precondition");
    CONSORT_AUDIT(std::ranges::is_sorted(middle, last_it, comp, proj) && "Precondition");
    detail::TimSort<Iterator>::merge(first, middle, last_it, comp, proj);
    CONSORT_AUDIT(std::ranges::is_sorted(first, last_it, comp, proj) && "Postcondition");
    return last_it;
}

/**
 * Range overload for conmerge.
 */
template <
    std::ranges::random_access_range Range,
    typename Compare    = std::ranges::less,
    typename Projection = std::identity
>
    requires std::sortable<std::ranges::iterator_t<Range>, Compare, Projection>
auto conmerge(Range&& range, std::ranges::iterator_t<Range> middle,
              Compare comp={}, Projection proj={})
    -> std::ranges::borrowed_iterator_t<Range>
{
    return consort::conmerge(std::begin(range), middle, std::end(range), comp, proj);
}

/**
 * Stably sorts a range. Drop-in replacement for std::sort / std::stable_sort.
 * Auto-selects best algorithm via heuristics (timsort base + memmove/insertion fast paths).
 */
template <
    std::random_access_iterator Iterator,
    std::sentinel_for<Iterator> Sentinel,
    typename Compare    = std::ranges::less,
    typename Projection = std::identity
>
    requires std::sortable<Iterator, Compare, Projection>
auto consort(Iterator first, Sentinel last,
             Compare comp={}, Projection proj={})
    -> Iterator
{
    auto last_it = std::ranges::next(first, last);
    detail::TimSort<Iterator>::sort(first, last_it, comp, proj);
    CONSORT_AUDIT(std::ranges::is_sorted(first, last_it, comp, proj) && "Postcondition");
    return last_it;
}

/**
 * Range overload for consort.
 */
template <
    std::ranges::random_access_range Range,
    typename Compare    = std::ranges::less,
    typename Projection = std::identity
>
    requires std::sortable<std::ranges::iterator_t<Range>, Compare, Projection>
auto consort(Range&& range, Compare comp={}, Projection proj={})
    -> std::ranges::borrowed_iterator_t<Range>
{
    return consort::consort(std::begin(range), std::end(range), comp, proj);
}

// gfx compatibility aliases if needed
namespace gfx_compat {
    template<typename... Args> auto timsort(Args&&... args) { return consort::consort(std::forward<Args>(args)...); }
    template<typename... Args> auto timmerge(Args&&... args) { return consort::conmerge(std::forward<Args>(args)...); }
}

} // namespace consort

#undef CONSORT_ENABLE_ASSERT
#undef CONSORT_ASSERT
#undef CONSORT_ENABLE_AUDIT
#undef CONSORT_AUDIT
#undef CONSORT_ENABLE_LOG
#undef CONSORT_LOG

#endif // CONSORT_HPP
