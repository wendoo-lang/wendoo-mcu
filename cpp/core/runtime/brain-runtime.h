#pragma once

#include <cstdint>

#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/mc-number.h"
#include "core/runtime/program.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"

namespace wendoo {

/** Sentinel page index marking "no page" (before any deactivation). */
inline constexpr uint32_t kNoPageIndex = 0xffffffffu;

/**
 * The brain think loop over one active page: page activation, per-think rule
 * fiber respawn, and time stamping. Mirrors the `startup`/`think` flow of
 * `BrainRuntime` in
 * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts for a
 * single-page program.
 *
 * Single-entry: only the host loop may call {@link think}; host callbacks
 * and action bodies must never re-enter it.
 */
class BrainRuntime {
public:
  /**
   * A runtime ticking `program` on `scheduler` against `surface`, whose
   * `context` must be non-null. All referenced objects must outlive the
   * runtime.
   */
  BrainRuntime(const ProgramImage& program, FiberScheduler& scheduler,
               const RuntimeSurface& surface);

  /**
   * Begins execution: activates page 0, running each host call site's
   * one-time initializer hook and per-activation page-entered hook with the
   * call site bound, then spawning the page's root-rule fibers in order. A
   * program with no pages activates nothing. Call once before {@link think}.
   * Fails with `ErrorCode::HostError` when the surface has no context or a
   * call-site id exceeds its slot table, and with `ErrorCode::StackOverflow`
   * when the region cannot back the page's root-rule tracking; spawn failures
   * propagate.
   */
  Status startup();

  /**
   * Advances the brain by one think. Stamps `time`, `dt` (0 until a previous
   * think exists, the difference otherwise), and the tick counter on the
   * execution context, respawns every completed, faulted, or cancelled rule
   * fiber, and runs one scheduler round followed by a reclaim sweep. A
   * no-op when no page is active. Fails with `ErrorCode::HostError` on
   * re-entry; spawn failures propagate.
   *
   * @param currentTimeMs - Monotonically increasing time in milliseconds.
   */
  Status think(mc_number_t currentTimeMs);

  /**
   * Ends execution: runs the current page's deactivation hooks in call-site
   * order -- each host page-exited hook or bytecode deactivation hook bound to
   * its call site -- cancels the page's rule fibers, and then drops every
   * pending async handle. No page is active afterwards, so {@link think} does
   * nothing. Repeating the call runs the same deactivation hooks again, as
   * `shutdown` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts does. Call
   * after {@link startup}. Fails with `ErrorCode::HostError` when a call-site
   * id exceeds its slot table; a faulting bytecode deactivation hook propagates
   * its code, and the handles are left in place.
   */
  Status shutdown();

  /**
   * Requests a switch to page `pageIndex` at the next {@link think}, which
   * deactivates the current page (running its actions' deactivation hooks and
   * cancelling its rule fibers) and activates the requested one. Cancels the
   * current page's rule fibers immediately; the rest of the in-flight round does
   * not run them. Requesting the current page restarts it (see
   * {@link requestPageRestart}); out of range is a no-op. Mirrors
   * `requestPageChange` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts.
   */
  void requestPageChange(uint32_t pageIndex);

  /**
   * Requests that the current page restart at the next {@link think}: cancels
   * its rule fibers so {@link think} respawns them from their entry, without
   * re-running the page's activation hooks or resetting per-callsite state.
   * Mirrors `requestPageRestart` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts.
   */
  void requestPageRestart();

  /**
   * Requests a switch to the page whose stable id equals the `length` bytes at
   * `pageId`, comparing by content against each page's id. A no-op when no page
   * matches. Mirrors the stable-id arm of `requestPageChangeByPageId` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts; the TS
   * page-name fallback has no mirror because the decoded image carries no page
   * names.
   */
  void requestPageChangeByPageId(const char* pageId, uint32_t length);

  /**
   * The stable page id of the active page as a borrowed-string {@link Value}.
   * Meaningful only while a page is active.
   */
  Value getCurrentPageId() const;

  /**
   * The stable page id of the most recently deactivated page as a
   * borrowed-string {@link Value}, or the current page's id when no page has
   * been deactivated yet. Mirrors `getPreviousPageId` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts.
   */
  Value getPreviousPageId() const;

private:
  Status activatePage(uint32_t pageIndex);
  Status deactivateCurrentPage();
  void cancelActiveFibers();
  // Runs each registered System's init once, in registration order, before the
  // first page activates.
  Status runSystemInits();
  // Runs each registered System's think once, in registration order, every
  // think regardless of the active page.
  Status runSystemThinks();

  /** One root rule of the active page and its current fiber. */
  struct RuleFiber {
    uint32_t funcId;
    uint32_t fiberId;
  };

  const ProgramImage& program_;
  FiberScheduler& scheduler_;
  RuntimeSurface surface_;
  /** Per-root-rule tracking for the active page. Allocated once for the brain's
   * lifetime in startup and reused on every activation; its capacity covers the
   * page with the most root rules. {@link ruleFiberCount_} is the active page's
   * count. */
  RuleFiber* ruleFibers_ = nullptr;
  uint32_t ruleFiberCount_ = 0;
  bool pageActive_ = false;
  bool inThink_ = false;
  mc_number_t lastThinkTime_ = 0;
  /** Index of the active page; meaningful only while {@link pageActive_}. */
  uint32_t currentPageIndex_ = 0;
  /** Index of the page a {@link requestPageChange} asked to switch to. */
  uint32_t desiredPageIndex_ = 0;
  /** Index of the most recently deactivated page, or {@link kNoPageIndex}. */
  uint32_t previousPageIndex_ = kNoPageIndex;
};

} // namespace wendoo
