// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2006, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ---
 * Author: Sanjay Ghemawat
 */

// SpinLock is async signal safe.
// If used within a signal handler, all lock holders
// should block the signal even outside the signal handler.

#ifndef BASE_SPINLOCK_H_
#define BASE_SPINLOCK_H_

#include <config.h>

#include <atomic>
#include <type_traits>

#include "base/basictypes.h"
#include "base/dynamic_annotations.h"
#include "base/static_storage.h"
#include "base/thread_annotations.h"

class LOCKABLE SpinLock {
 public:
  constexpr SpinLock() : lockword_(kSpinLockFree) { }

  // Acquire this SpinLock.
  void Lock() EXCLUSIVE_LOCK_FUNCTION() {
    int old = kSpinLockFree;
    if (!lockword_.compare_exchange_weak(old, kSpinLockHeld, std::memory_order_acquire)) {
      SlowLock();
    }
  }

  // Try to acquire this SpinLock without blocking and return true if the
  // acquisition was successful.  If the lock was not acquired, false is
  // returned.  If this SpinLock is free at the time of the call, TryLock
  // will return true with high probability.
  bool TryLock() EXCLUSIVE_TRYLOCK_FUNCTION(true) {
    int old = kSpinLockFree;
    return lockword_.compare_exchange_weak(old, kSpinLockHeld);
  }

  // Release this SpinLock, which must be held by the calling thread.
  void Unlock() UNLOCK_FUNCTION() {
    int prev_value = lockword_.exchange(kSpinLockFree, std::memory_order_release);
    if (prev_value != kSpinLockHeld) {
      // Speed the wakeup of any waiter.
      SlowUnlock();
    }
  }

  // Determine if the lock is held.  When the lock is held by the invoking
  // thread, true will always be returned. Intended to be used as
  // CHECK(lock.IsHeld()).
  bool IsHeld() const {
    return lockword_.load(std::memory_order_relaxed) != kSpinLockFree;
  }

 private:
  enum { kSpinLockFree = 0 };
  enum { kSpinLockHeld = 1 };
  enum { kSpinLockSleeper = 2 };

  std::atomic<int> lockword_;

  void SlowLock();
  void SlowUnlock();
  int SpinLoop();

  DISALLOW_COPY_AND_ASSIGN(SpinLock);
};

// Corresponding locker object that arranges to acquire a spinlock for
// the duration of a C++ scope.
class SCOPED_LOCKABLE SpinLockHolder {
 private:
  SpinLock* lock_;
 public:
  explicit SpinLockHolder(SpinLock* l) EXCLUSIVE_LOCK_FUNCTION(l)
      : lock_(l) {
    l->Lock();
  }
  SpinLockHolder(const SpinLockHolder&) = delete;
  ~SpinLockHolder() UNLOCK_FUNCTION() {
    lock_->Unlock();
  }
};
// Catch bug where variable name is omitted, e.g. SpinLockHolder (&lock);
#define SpinLockHolder(x) static_assert(0)

namespace tcmalloc {

class TrivialOnce {
public:
  template <typename Body>
  bool RunOnce(Body body) {
    auto done_atomic = reinterpret_cast<std::atomic<int>*>(&done_flag_);
    if (done_atomic->load(std::memory_order_acquire) == 1) {
      return false;
    }

    SpinLockHolder h(lock_storage_.get());

    if (done_atomic->load(std::memory_order_relaxed) == 1) {
      // barrier provided by lock
      return false;
    }
    body();
    done_atomic->store(1, std::memory_order_release);
    return true;
  }

private:
  int done_flag_;
  StaticStorage<SpinLock> lock_storage_;
};

static_assert(std::is_trivial<TrivialOnce>::value == true, "");

}  // namespace tcmalloc


#endif  // BASE_SPINLOCK_H_
