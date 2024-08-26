// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2006, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat
//         Maxim Lifantsev (refactoring)
//

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>   // for write()
#endif
#include <fcntl.h>    // for open()
#ifdef HAVE_GLOB_H
#include <glob.h>
#ifndef GLOB_NOMATCH  // true on some old cygwins
# define GLOB_NOMATCH 0
#endif
#endif
#include <inttypes.h> // for PRIxPTR
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#include <errno.h>
#include <stdarg.h>

#include <algorithm>  // for sort(), equal(), and copy()
#include <map>
#include <memory>
#include <string>

#include "heap-profile-table.h"

#include "base/commandlineflags.h"
#include "base/logging.h"
#include "base/proc_maps_iterator.h"
#include "base/sysinfo.h"
#include "gperftools/malloc_hook.h"
#include "gperftools/stacktrace.h"
#include "memory_region_map.h"
#include "symbolize.h"

using std::sort;
using std::equal;
using std::copy;
using std::string;
using std::map;

//----------------------------------------------------------------------

DEFINE_bool(cleanup_old_heap_profiles,
            EnvToBool("HEAP_PROFILE_CLEANUP", true),
            "At initialization time, delete old heap profiles.");

DEFINE_int32(heap_check_max_leaks,
             EnvToInt("HEAP_CHECK_MAX_LEAKS", 20),
             "The maximum number of leak reports to print.");

//----------------------------------------------------------------------

// header of the dumped heap profile
static const char kProfileHeader[] = "heap profile: ";
static const char kProcSelfMapsHeader[] = "\nMAPPED_LIBRARIES:\n";

//----------------------------------------------------------------------

const char HeapProfileTable::kFileExt[] = ".heap";

//----------------------------------------------------------------------

static const int kHashTableSize = 179999;   // Size for bucket_table_.
/*static*/ const int HeapProfileTable::kMaxStackDepth;

//----------------------------------------------------------------------

// We strip out different number of stack frames in debug mode
// because less inlining happens in that case
#ifdef NDEBUG
static const int kStripFrames = 2;
#else
static const int kStripFrames = 3;
#endif

//----------------------------------------------------------------------

HeapProfileTable::HeapProfileTable(Allocator alloc,
                                   DeAllocator dealloc,
                                   bool profile_mmap)
    : alloc_(alloc),
      dealloc_(dealloc),
      profile_mmap_(profile_mmap),
      bucket_table_(NULL),
      num_buckets_(0),
      address_map_(NULL) {
  // Make a hash table for buckets.
  const int table_bytes = kHashTableSize * sizeof(*bucket_table_);
  bucket_table_ = static_cast<Bucket**>(alloc_(table_bytes));
  memset(bucket_table_, 0, table_bytes);

  // Make an allocation map.
  address_map_ =
      new(alloc_(sizeof(AllocationMap))) AllocationMap(alloc_, dealloc_);

  // Initialize.
  memset(&total_, 0, sizeof(total_));
  num_buckets_ = 0;
}

HeapProfileTable::~HeapProfileTable() {
  // Free the allocation map.
  address_map_->~AllocationMap();
  dealloc_(address_map_);
  address_map_ = NULL;

  // Free the hash table.
  for (int i = 0; i < kHashTableSize; i++) {
    for (Bucket* curr = bucket_table_[i]; curr != 0; /**/) {
      Bucket* bucket = curr;
      curr = curr->next;
      dealloc_(bucket->stack);
      dealloc_(bucket);
    }
  }
  dealloc_(bucket_table_);
  bucket_table_ = NULL;
}

HeapProfileTable::Bucket* HeapProfileTable::GetBucket(int depth,
                                                      const void* const key[]) {
  // Make hash-value
  uintptr_t h = 0;
  for (int i = 0; i < depth; i++) {
    h += reinterpret_cast<uintptr_t>(key[i]);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;

  // Lookup stack trace in table
  unsigned int buck = ((unsigned int) h) % kHashTableSize;
  for (Bucket* b = bucket_table_[buck]; b != 0; b = b->next) {
    if ((b->hash == h) &&
        (b->depth == depth) &&
        equal(key, key + depth, b->stack)) {
      return b;
    }
  }

  // Create new bucket
  const size_t key_size = sizeof(key[0]) * depth;
  const void** kcopy = reinterpret_cast<const void**>(alloc_(key_size));
  copy(key, key + depth, kcopy);
  Bucket* b = reinterpret_cast<Bucket*>(alloc_(sizeof(Bucket)));
  memset(b, 0, sizeof(*b));
  b->hash  = h;
  b->depth = depth;
  b->stack = kcopy;
  b->next  = bucket_table_[buck];
  bucket_table_[buck] = b;
  num_buckets_++;
  return b;
}

int HeapProfileTable::GetCallerStackTrace(
    int skip_count, void* stack[kMaxStackDepth]) {
  return MallocHook::GetCallerStackTrace(
      stack, kMaxStackDepth, kStripFrames + skip_count + 1);
}

void HeapProfileTable::RecordAlloc(
    const void* ptr, size_t bytes, int stack_depth,
    const void* const call_stack[]) {
  Bucket* b = GetBucket(stack_depth, call_stack);
  b->allocs++;
  b->alloc_size += bytes;
  total_.allocs++;
  total_.alloc_size += bytes;

  AllocValue v;
  v.set_bucket(b);  // also did set_live(false); set_ignore(false)
  v.bytes = bytes;
  address_map_->Insert(ptr, v);
}

void HeapProfileTable::RecordFree(const void* ptr) {
  AllocValue v;
  if (address_map_->FindAndRemove(ptr, &v)) {
    Bucket* b = v.bucket();
    b->frees++;
    b->free_size += v.bytes;
    total_.frees++;
    total_.free_size += v.bytes;
  }
}

bool HeapProfileTable::FindAlloc(const void* ptr, size_t* object_size) const {
  const AllocValue* alloc_value = address_map_->Find(ptr);
  if (alloc_value != NULL) *object_size = alloc_value->bytes;
  return alloc_value != NULL;
}

bool HeapProfileTable::FindAllocDetails(const void* ptr,
                                        AllocInfo* info) const {
  const AllocValue* alloc_value = address_map_->Find(ptr);
  if (alloc_value != NULL) {
    info->object_size = alloc_value->bytes;
    info->call_stack = alloc_value->bucket()->stack;
    info->stack_depth = alloc_value->bucket()->depth;
  }
  return alloc_value != NULL;
}

bool HeapProfileTable::FindInsideAlloc(const void* ptr,
                                       size_t max_size,
                                       const void** object_ptr,
                                       size_t* object_size) const {
  const AllocValue* alloc_value =
    address_map_->FindInside(&AllocValueSize, max_size, ptr, object_ptr);
  if (alloc_value != NULL) *object_size = alloc_value->bytes;
  return alloc_value != NULL;
}

bool HeapProfileTable::MarkAsLive(const void* ptr) {
  AllocValue* alloc = address_map_->FindMutable(ptr);
  if (alloc && !alloc->live()) {
    alloc->set_live(true);
    return true;
  }
  return false;
}

void HeapProfileTable::MarkAsIgnored(const void* ptr) {
  AllocValue* alloc = address_map_->FindMutable(ptr);
  if (alloc) {
    alloc->set_ignore(true);
  }
}

void HeapProfileTable::UnparseBucket(const Bucket& b,
                                     tcmalloc::GenericWriter* writer,
                                     const char* extra) {
  writer->AppendF("%6" PRId64 ": %8" PRId64 " [%6" PRId64 ": %8" PRId64 "] @",
                  b.allocs - b.frees,
                  b.alloc_size - b.free_size,
                  b.allocs,
                  b.alloc_size);
  writer->AppendStr(extra);

  for (int d = 0; d < b.depth; d++) {
    writer->AppendF(" 0x%08" PRIxPTR,
                    reinterpret_cast<uintptr_t>(b.stack[d]));
  }
  writer->AppendStr("\n");
}

void HeapProfileTable::SaveProfile(tcmalloc::GenericWriter* writer) const {
  writer->AppendStr(kProfileHeader);
  UnparseBucket(total_, writer, " heapprofile");

  // Dump the mmap list first.
  if (profile_mmap_) {
    MemoryRegionMap::LockHolder holder{};
    MemoryRegionMap::IterateBuckets([writer] (const Bucket* bucket) {
      UnparseBucket(*bucket, writer, "");
    });
  }

  int bucket_count = 0;
  for (int i = 0; i < kHashTableSize; i++) {
    for (Bucket* curr = bucket_table_[i]; curr != nullptr; curr = curr->next) {
      UnparseBucket(*curr, writer, "");
      bucket_count++;
    }
  }
  RAW_DCHECK(bucket_count == num_buckets_, "");
  (void)bucket_count;

  writer->AppendStr(kProcSelfMapsHeader);
  tcmalloc::SaveProcSelfMaps(writer);
}

bool HeapProfileTable::WriteProfile(const char* file_name,
                                    const Bucket& total,
                                    AllocationMap* allocations) {
  RAW_VLOG(1, "Dumping non-live heap profile to %s", file_name);
  RawFD fd = RawOpenForWriting(file_name);
  if (fd == kIllegalRawFD) {
    RAW_LOG(ERROR, "Failed dumping filtered heap profile to %s", file_name);
    return false;
  }

  tcmalloc::RawFDGenericWriter<> writer{fd};

  writer.AppendStr(kProfileHeader);

  UnparseBucket(total, &writer, " heapprofile");

  allocations->Iterate([&writer] (const void* ptr, AllocValue* v) {
    if (v->live()) {
      v->set_live(false);
      return;
    }
    if (v->ignore()) {
      return;
    }
    Bucket b;
    memset(&b, 0, sizeof(b));
    b.allocs = 1;
    b.alloc_size = v->bytes;
    b.depth = v->bucket()->depth;
    b.stack = v->bucket()->stack;
    UnparseBucket(b, &writer, "");
  });

  RawWrite(fd, kProcSelfMapsHeader, strlen(kProcSelfMapsHeader));
  tcmalloc::SaveProcSelfMapsToRawFD(fd);

  RawClose(fd);
  return true;
}

void HeapProfileTable::CleanupOldProfiles(const char* prefix) {
  if (!FLAGS_cleanup_old_heap_profiles)
    return;
  string pattern = string(prefix) + ".*" + kFileExt;
#if defined(HAVE_GLOB_H)
  glob_t g;
  const int r = glob(pattern.c_str(), GLOB_ERR, NULL, &g);
  if (r == 0 || r == GLOB_NOMATCH) {
    const int prefix_length = strlen(prefix);
    for (int i = 0; i < g.gl_pathc; i++) {
      const char* fname = g.gl_pathv[i];
      if ((strlen(fname) >= prefix_length) &&
          (memcmp(fname, prefix, prefix_length) == 0)) {
        RAW_VLOG(1, "Removing old heap profile %s", fname);
        unlink(fname);
      }
    }
  }
  globfree(&g);
#else   /* HAVE_GLOB_H */
  RAW_LOG(WARNING, "Unable to remove old heap profiles (can't run glob())");
#endif
}

HeapProfileTable::Snapshot* HeapProfileTable::TakeSnapshot() {
  Snapshot* s = new (alloc_(sizeof(Snapshot))) Snapshot(alloc_, dealloc_);
  address_map_->Iterate([s] (const void* ptr, AllocValue* v) {
    s->Add(ptr, *v);
  });
  return s;
}

void HeapProfileTable::ReleaseSnapshot(Snapshot* s) {
  s->~Snapshot();
  dealloc_(s);
}

HeapProfileTable::Snapshot* HeapProfileTable::NonLiveSnapshot(
    Snapshot* base) {
  RAW_VLOG(2, "NonLiveSnapshot input: %" PRId64 " %" PRId64 "\n",
           total_.allocs - total_.frees,
           total_.alloc_size - total_.free_size);

  Snapshot* s = new (alloc_(sizeof(Snapshot))) Snapshot(alloc_, dealloc_);
  address_map_->Iterate([&] (const void* ptr, AllocValue* v) {
    if (v->live()) {
      v->set_live(false);
    } else {
      if (base != nullptr && base->map_.Find(ptr) != nullptr) {
        // Present in arg->base, so do not save
      } else {
        s->Add(ptr, *v);
      }
    }
  });
  RAW_VLOG(2, "NonLiveSnapshot output: %" PRId64 " %" PRId64 "\n",
           s->total_.allocs - s->total_.frees,
           s->total_.alloc_size - s->total_.free_size);
  return s;
}

// Information kept per unique bucket seen
struct HeapProfileTable::Snapshot::Entry {
  int count;
  size_t bytes;
  Bucket* bucket;
  Entry() : count(0), bytes(0) { }

  // Order by decreasing bytes
  bool operator<(const Entry& x) const {
    return this->bytes > x.bytes;
  }
};

void HeapProfileTable::Snapshot::ReportLeaks(const char* checker_name,
                                             const char* filename,
                                             bool should_symbolize) {
  // This is only used by the heap leak checker, but is intimately
  // tied to the allocation map that belongs in this module and is
  // therefore placed here.
  RAW_LOG(ERROR, "Leak check %s detected leaks of %zu bytes "
          "in %zu objects",
          checker_name,
          size_t(total_.alloc_size),
          size_t(total_.allocs));

  // Group objects by Bucket
  std::map<Bucket*, Entry> buckets;
  map_.Iterate([&] (const void* ptr, AllocValue* v) {
    Entry* e = &buckets[v->bucket()]; // Creates empty Entry first time
    e->bucket = v->bucket();
    e->count++;
    e->bytes += v->bytes;
  });

  // Sort buckets by decreasing leaked size
  const int n = buckets.size();
  Entry* entries = new Entry[n];
  int dst = 0;
  for (map<Bucket*,Entry>::const_iterator iter = buckets.begin();
       iter != buckets.end();
       ++iter) {
    entries[dst++] = iter->second;
  }
  std::sort(entries, entries + n);

  // Report a bounded number of leaks to keep the leak report from
  // growing too long.
  const int to_report =
      (FLAGS_heap_check_max_leaks > 0 &&
       n > FLAGS_heap_check_max_leaks) ? FLAGS_heap_check_max_leaks : n;
  RAW_LOG(ERROR, "The %d largest leaks:", to_report);

  // Print
  SymbolTable symbolization_table;
  for (int i = 0; i < to_report; i++) {
    const Entry& e = entries[i];
    for (int j = 0; j < e.bucket->depth; j++) {
      symbolization_table.Add(e.bucket->stack[j]);
    }
  }
  if (should_symbolize)
    symbolization_table.Symbolize();

  {
    auto do_log = +[] (const char* buf, size_t amt) {
      RAW_LOG(ERROR, "%.*s", amt, buf);
    };
    constexpr int kBufSize = 2<<10;
    tcmalloc::WriteFnWriter<decltype(do_log), kBufSize> printer{do_log};

    for (int i = 0; i < to_report; i++) {
      const Entry& e = entries[i];
      printer.AppendF("Leak of %zu bytes in %d objects allocated from:\n",
                      e.bytes, e.count);
      for (int j = 0; j < e.bucket->depth; j++) {
        const void* pc = e.bucket->stack[j];
        printer.AppendF("\t@ %" PRIxPTR " %s\n",
                        reinterpret_cast<uintptr_t>(pc), symbolization_table.GetSymbol(pc));
      }
    }
  }

  if (to_report < n) {
    RAW_LOG(ERROR, "Skipping leaks numbered %d..%d",
            to_report, n-1);
  }
  delete[] entries;

  if (!HeapProfileTable::WriteProfile(filename, total_, &map_)) {
    RAW_LOG(ERROR, "Could not write pprof profile to %s", filename);
  }
}

void HeapProfileTable::Snapshot::ReportIndividualObjects() {
  map_.Iterate([] (const void* ptr, AllocValue* v) {
    // Perhaps also log the allocation stack trace (unsymbolized)
    // on this line in case somebody finds it useful.
    RAW_LOG(ERROR, "leaked %zu byte object %p", v->bytes, ptr);
  });
}
