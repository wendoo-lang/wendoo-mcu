#pragma once

#include <cstdint>

#include "core/runtime/error-code.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/pool.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/value.h"

namespace wendoo {

/** Sentinel handle id; real ids start at 1. */
inline constexpr uint32_t kNoHandleId = 0;

/**
 * Lifecycle state of an async {@link Handle}. Mirrors `HandleState` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
 */
enum class HandleState : uint8_t {
  /** The async operation is in flight; the handle may still settle. */
  Pending,
  /** The operation resolved; {@link Handle::result} holds its value. */
  Resolved,
  /** The operation rejected; {@link Handle::error} holds the classifier. */
  Rejected,
  /** The operation was cancelled; {@link Handle::error} is `Cancelled`. */
  Cancelled,
};

/** One fiber waiting on a handle, threaded into a per-handle singly linked list. */
struct HandleWaiter {
  uint32_t fiberId;
  HandleWaiter* next;
};

/**
 * One async operation handle: its id, lifecycle state, settled value/error, the
 * list of waiting fibers, and the intrusive completed-queue link. Mirrors
 * `Handle` in external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
 */
struct Handle {
  /** Stable handle id, unique for the table's lifetime. */
  uint32_t id;
  HandleState state;
  /** The resolved value; meaningful only when {@link state} is `Resolved`. */
  Value result;
  /** The fault classifier; meaningful only when `Rejected` or `Cancelled`. */
  ErrorCode error;
  /** Head of the waiting-fiber list, or nullptr when none wait. */
  HandleWaiter* waitersHead;
  /** Next handle in the settled (completed) drain queue. */
  Handle* nextCompleted;
  /**
   * Whether this handle counts against the table's `maxHandles` cap. True for
   * a handle backing a device or host operation; false for a handle whose live
   * count the runtime already bounds by construction.
   */
  bool capped;
};

/**
 * Tracks pending async operations: carves handles on demand from a pool,
 * settles them (resolve/reject/cancel) only from `Pending`, records the fibers
 * waiting on each, and enqueues every settled handle onto a completed queue the
 * scheduler drains. Mirrors `HandleTable` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts under the
 * enqueue->drain resolution model: a settle only flips state and enqueues, so a
 * host callback may settle a handle from outside the single-entry loop; the
 * scheduler resumes its waiters on a later drain.
 *
 * Pool-backed: both handles and waiter nodes carve on demand from the shared
 * {@link RegionArena}. {@link DeviceProfileCaps::maxHandles} is a runtime guard
 * checked at {@link createPending}, never a preallocation size.
 */
class HandleTable {
public:
  /**
   * A table carving handles and waiter nodes from `arena` under a `maxHandles`
   * live-handle guard. `arena` must outlive the table.
   */
  HandleTable(RegionArena& arena, uint32_t maxHandles)
      : handles_(arena), waiters_(arena), maxHandles_(maxHandles) {}

  /**
   * True when a capped {@link createPending} would clear the live-handle guard
   * right now: the live capped-handle count is below {@link maxHandles}.
   * Uncapped handles are excluded from the count and always allocate.
   * Non-mutating; an async dispatch checks it to decide whether to allocate or
   * to park and retry next round. A create can still fail on arena exhaustion
   * after this returns true.
   */
  bool hasCapacity() const { return cappedCount_ < maxHandles_; }

  /** Count of live handles (any state) currently carved from the pool. */
  uint32_t size() const { return handles_.liveCount(); }

  /** Count of live handles that count against {@link maxHandles}. */
  uint32_t cappedSize() const { return cappedCount_; }

  /**
   * Carves a fresh `Pending` handle and returns its id, or {@link kNoHandleId}
   * when a capped create would cross {@link maxHandles} or the arena cannot
   * back a new slot. Pass `capped` false to keep the handle out of the
   * {@link maxHandles} accounting.
   */
  uint32_t createPending(bool capped = true) {
    if (capped && cappedCount_ >= maxHandles_) {
      return kNoHandleId;
    }
    Handle* h = handles_.alloc();
    if (h == nullptr) {
      return kNoHandleId;
    }
    h->id = nextId_++;
    h->state = HandleState::Pending;
    h->result = kNilValue;
    h->error = ErrorCode::HostError;
    h->waitersHead = nullptr;
    h->nextCompleted = nullptr;
    h->capped = capped;
    if (capped) {
      cappedCount_++;
    }
    return h->id;
  }

  /** The handle holding `id`, or nullptr when none does. */
  Handle* get(uint32_t id) { return findHandle(id); }

  /**
   * Settles `id` to `Resolved` with `result` and enqueues it for draining.
   * Returns false (a no-op) when no handle holds `id` or it is not `Pending`.
   */
  bool resolve(uint32_t id, const Value& result) {
    Handle* h = findHandle(id);
    if (h == nullptr || h->state != HandleState::Pending) {
      return false;
    }
    h->state = HandleState::Resolved;
    h->result = result;
    enqueueCompleted(h);
    return true;
  }

  /**
   * Settles `id` to `Rejected` with `error` and enqueues it for draining.
   * Returns false (a no-op) when no handle holds `id` or it is not `Pending`.
   */
  bool reject(uint32_t id, ErrorCode error) {
    Handle* h = findHandle(id);
    if (h == nullptr || h->state != HandleState::Pending) {
      return false;
    }
    h->state = HandleState::Rejected;
    h->error = error;
    enqueueCompleted(h);
    return true;
  }

  /**
   * Settles `id` to `Cancelled` and enqueues it for draining. Returns false (a
   * no-op) when no handle holds `id` or it is not `Pending`.
   */
  bool cancel(uint32_t id) {
    Handle* h = findHandle(id);
    if (h == nullptr || h->state != HandleState::Pending) {
      return false;
    }
    h->state = HandleState::Cancelled;
    h->error = ErrorCode::Cancelled;
    enqueueCompleted(h);
    return true;
  }

  /** Adds `fiberId` to `id`'s waiter list. A no-op when no handle holds `id`. */
  void addWaiter(uint32_t id, uint32_t fiberId) {
    Handle* h = findHandle(id);
    if (h == nullptr) {
      return;
    }
    HandleWaiter* node = waiters_.alloc();
    if (node == nullptr) {
      return;
    }
    node->fiberId = fiberId;
    node->next = nullptr;
    // Append at the tail (FIFO waiter order).
    if (h->waitersHead == nullptr) {
      h->waitersHead = node;
      return;
    }
    HandleWaiter* tail = h->waitersHead;
    while (tail->next != nullptr) {
      tail = tail->next;
    }
    tail->next = node;
  }

  /** Removes `fiberId` from `id`'s waiter list. A no-op when not present. */
  void removeWaiter(uint32_t id, uint32_t fiberId) {
    Handle* h = findHandle(id);
    if (h == nullptr) {
      return;
    }
    HandleWaiter* prev = nullptr;
    for (HandleWaiter* cur = h->waitersHead; cur != nullptr; cur = cur->next) {
      if (cur->fiberId == fiberId) {
        if (prev == nullptr) {
          h->waitersHead = cur->next;
        } else {
          prev->next = cur->next;
        }
        waiters_.free(cur);
        return;
      }
      prev = cur;
    }
  }

  /**
   * Removes and returns the next settled handle from the completed queue, or
   * nullptr when the queue is empty. The handle is still live; the caller
   * resumes its waiters and then {@link deleteHandle}s it.
   */
  Handle* popCompleted() {
    Handle* h = completedHead_;
    if (h == nullptr) {
      return nullptr;
    }
    completedHead_ = h->nextCompleted;
    if (completedHead_ == nullptr) {
      completedTail_ = nullptr;
    }
    h->nextCompleted = nullptr;
    return h;
  }

  /** Frees `id`'s remaining waiter nodes and its handle slot. */
  void deleteHandle(uint32_t id) {
    Handle* h = findHandle(id);
    if (h == nullptr) {
      return;
    }
    for (HandleWaiter* cur = h->waitersHead; cur != nullptr;) {
      HandleWaiter* next = cur->next;
      waiters_.free(cur);
      cur = next;
    }
    if (h->capped) {
      cappedCount_--;
    }
    handles_.free(h);
  }

  /**
   * Marks every live handle's settled value into `marker`: a resolved handle
   * holds its result until a waiter consumes it, so it is a collection root.
   */
  void enumerateResults(GcMarker& marker) {
    handles_.forEachLive([&marker](Handle& h) {
      if (h.state == HandleState::Resolved) {
        marker.mark(h.result);
      }
    });
  }

private:
  Handle* findHandle(uint32_t id) {
    Handle* found = nullptr;
    handles_.forEachLive([&](Handle& h) {
      if (h.id == id) {
        found = &h;
      }
    });
    return found;
  }

  void enqueueCompleted(Handle* h) {
    h->nextCompleted = nullptr;
    if (completedTail_ != nullptr) {
      completedTail_->nextCompleted = h;
    } else {
      completedHead_ = h;
    }
    completedTail_ = h;
  }

  Pool<Handle> handles_;
  Pool<HandleWaiter> waiters_;
  uint32_t maxHandles_;
  uint32_t cappedCount_ = 0;
  uint32_t nextId_ = 1;
  Handle* completedHead_ = nullptr;
  Handle* completedTail_ = nullptr;
};

/**
 * Settle handle for one pending async operation, bound to a single {@link id} in
 * a {@link HandleTable}. An async host function or action body retains this
 * across the async boundary and calls exactly one of {@link resolve},
 * {@link reject}, or {@link cancel} when the work completes. Settling a handle
 * that is no longer pending -- already settled, cancelled with its fiber, or
 * freed -- is a no-op. Mirrors `AsyncHandle` in
 * external/wendoo-lang/packages/core/src/runtime/value.ts.
 */
struct AsyncHandle {
  /** The handle table the bound handle lives in. */
  HandleTable* table;

  /** The bound handle id, the value the dispatching opcode pushed for `AWAIT`. */
  uint32_t id;

  /** Settles the handle as resolved, carrying `value` to the awaiting fiber. */
  void resolve(const Value& value) const { table->resolve(id, value); }

  /** Settles the handle as rejected; the awaiting fiber throws an error of `error`. */
  void reject(ErrorCode error) const { table->reject(id, error); }

  /** Settles the handle as cancelled; the awaiting fiber throws `ErrorCode::Cancelled`. */
  void cancel() const { table->cancel(id); }
};

} // namespace wendoo
