// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2009 Google Inc. All Rights Reserved.
// Author: fikes@google.com (Andrew Fikes)
//
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.

#include "config_for_unittests.h"

#include <stdio.h>

#include <limits>
#include <memory>

#include "page_heap.h"

#include "base/cleanup.h"
#include "common.h"
#include "system-alloc.h"

#include "gtest/gtest.h"

DECLARE_int64(tcmalloc_heap_limit_mb);

// TODO: add testing from >1 min_span_size setting.

bool HaveSystemRelease() {
  static bool have = ([] () {
    size_t actual;
    auto ptr = TCMalloc_SystemAlloc(kPageSize, &actual, 0);
    return TCMalloc_SystemRelease(ptr, actual);
  })();
  return have;
}

#if __linux__ || __APPLE__ || _WIN32 || __FreeBSD__ || __NetBSD__
TEST(PageHeapTest, HaveSystemRelease) {
  ASSERT_TRUE(HaveSystemRelease());
}
#endif

static void CheckStats(const tcmalloc::PageHeap* ph,
                       uint64_t system_pages,
                       uint64_t free_pages,
                       uint64_t unmapped_pages) {
  tcmalloc::PageHeap::Stats stats = ph->StatsLocked();

  if (!HaveSystemRelease()) {
    free_pages += unmapped_pages;
    unmapped_pages = 0;
  }

  EXPECT_EQ(system_pages, stats.system_bytes >> kPageShift);
  EXPECT_EQ(free_pages, stats.free_bytes >> kPageShift);
  EXPECT_EQ(unmapped_pages, stats.unmapped_bytes >> kPageShift);
}

TEST(PageHeapTest, Stats) {
  std::unique_ptr<tcmalloc::PageHeap> ph(new tcmalloc::PageHeap());

  // Empty page heap
  CheckStats(ph.get(), 0, 0, 0);

  // Allocate a span 's1'
  tcmalloc::Span* s1 = ph->New(256);
  CheckStats(ph.get(), 256, 0, 0);

  // Split span 's1' into 's1', 's2'.  Delete 's2'
  tcmalloc::Span* s2 = ph->SplitForTest(s1, 128);
  ph->Delete(s2);
  CheckStats(ph.get(), 256, 128, 0);

  // Unmap deleted span 's2'
  {
      SpinLockHolder l(ph->pageheap_lock());
      ph->ReleaseAtLeastNPages(1);
  }
  CheckStats(ph.get(), 256, 0, 128);

  // Delete span 's1'
  ph->Delete(s1);
  CheckStats(ph.get(), 256, 128, 128);
}

// The number of kMaxPages-sized Spans we will allocate and free during the
// tests.
// We will also do twice this many kMaxPages/2-sized ones.
static constexpr int kNumberMaxPagesSpans = 10;

// Allocates all the last-level page tables we will need. Doing this before
// calculating the base heap usage is necessary, because otherwise if any of
// these are allocated during the main test it will throw the heap usage
// calculations off and cause the test to fail.
static void AllocateAllPageTables() {
  // Make a separate PageHeap from the main test so the test can start without
  // any pages in the lists.
  std::unique_ptr<tcmalloc::PageHeap> ph(new tcmalloc::PageHeap());
  tcmalloc::Span *spans[kNumberMaxPagesSpans * 2];
  for (int i = 0; i < kNumberMaxPagesSpans; ++i) {
    spans[i] = ph->New(kMaxPages);
    EXPECT_NE(spans[i], nullptr);
  }
  for (int i = 0; i < kNumberMaxPagesSpans; ++i) {
    ph->Delete(spans[i]);
  }
  for (int i = 0; i < kNumberMaxPagesSpans * 2; ++i) {
    spans[i] = ph->New(kMaxPages >> 1);
    EXPECT_NE(spans[i], nullptr);
  }
  for (int i = 0; i < kNumberMaxPagesSpans * 2; ++i) {
    ph->Delete(spans[i]);
  }
}

TEST(PageHeapTest, Limit) {
  AllocateAllPageTables();

  tcmalloc::Cleanup restore_heap_limit_flag{[] () {
    FLAGS_tcmalloc_heap_limit_mb = 0;
  }};

  std::unique_ptr<tcmalloc::PageHeap> ph(new tcmalloc::PageHeap());

  // Lets also test if huge number of pages is ooming properly
  {
    auto res = ph->New(std::numeric_limits<Length>::max());
    ASSERT_EQ(res, nullptr);
    ASSERT_EQ(errno, ENOMEM);
  }

  ASSERT_EQ(kMaxPages, 1 << (20 - kPageShift));

  // We do not know much is taken from the system for other purposes,
  // so we detect the proper limit:
  {
    FLAGS_tcmalloc_heap_limit_mb = 1;
    tcmalloc::Span* s = nullptr;
    while((s = ph->New(kMaxPages)) == nullptr) {
      FLAGS_tcmalloc_heap_limit_mb++;
    }
    FLAGS_tcmalloc_heap_limit_mb += kNumberMaxPagesSpans - 1;
    ph->Delete(s);
    // We are [10, 11) mb from the limit now.
  }

  // Test AllocLarge and GrowHeap first:
  {
    tcmalloc::Span * spans[kNumberMaxPagesSpans];
    for (int i=0; i<kNumberMaxPagesSpans; ++i) {
      spans[i] = ph->New(kMaxPages);
      EXPECT_NE(spans[i], nullptr);
    }
    EXPECT_EQ(ph->New(kMaxPages), nullptr);

    for (int i=0; i<kNumberMaxPagesSpans; i += 2) {
      ph->Delete(spans[i]);
    }

    tcmalloc::Span *defragmented =
        ph->New(kNumberMaxPagesSpans / 2 * kMaxPages);

    if (HaveSystemRelease()) {
      // EnsureLimit should release deleted normal spans
      EXPECT_NE(defragmented, nullptr);
      ph->PrepareAndDelete(defragmented, [&] () {
        EXPECT_TRUE(ph->CheckExpensive());
      });
    }
    else
    {
      EXPECT_EQ(defragmented, nullptr);
      EXPECT_TRUE(ph->CheckExpensive());
    }

    for (int i=1; i<kNumberMaxPagesSpans; i += 2) {
      ph->Delete(spans[i]);
    }
  }

  // Once again, testing small lists this time (twice smaller spans):
  {
    tcmalloc::Span * spans[kNumberMaxPagesSpans * 2];
    for (int i=0; i<kNumberMaxPagesSpans * 2; ++i) {
      spans[i] = ph->New(kMaxPages >> 1);
      EXPECT_NE(spans[i], nullptr);
    }
    // one more half size allocation may be possible:
    tcmalloc::Span * lastHalf = ph->New(kMaxPages >> 1);
    EXPECT_EQ(ph->New(kMaxPages >> 1), nullptr);

    for (int i=0; i<kNumberMaxPagesSpans * 2; i += 2) {
      ph->Delete(spans[i]);
    }

    for (Length len = kMaxPages >> 2;
         len < kNumberMaxPagesSpans / 2 * kMaxPages; len = len << 1) {
      if(len <= kMaxPages >> 1 || HaveSystemRelease()) {
        tcmalloc::Span *s = ph->New(len);
        EXPECT_NE(s, nullptr);
        ph->Delete(s);
      }
    }

    {
        SpinLockHolder l(ph->pageheap_lock());
        EXPECT_TRUE(ph->CheckExpensive());
    }

    for (int i=1; i<kNumberMaxPagesSpans * 2; i += 2) {
      ph->Delete(spans[i]);
    }

    if (lastHalf != nullptr) {
      ph->Delete(lastHalf);
    }
  }
}
