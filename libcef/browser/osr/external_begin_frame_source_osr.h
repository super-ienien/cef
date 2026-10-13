// Copyright 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#ifndef CEF_LIBCEF_BROWSER_OSR_EXTERNAL_BEGIN_FRAME_SOURCE_OSR_H_
#define CEF_LIBCEF_BROWSER_OSR_EXTERNAL_BEGIN_FRAME_SOURCE_OSR_H_
#pragma once

#include <cstdint>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "cef/include/internal/cef_types.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "services/viz/privileged/mojom/compositing/external_begin_frame_controller.mojom.h"
#include "ui/compositor/compositor.h"

// Drives the viz external BeginFrame controller of a windowless compositor
// from the ticks that the client supplies through
// CefBrowserHost::SendExternalBeginFrame[WithTiming].
//
// Modeled on ui::ExternalBeginFrameAdapter, the platform BeginFrame source
// that Chromium uses with Wayland, which is only built on Linux. Compared with
// the historical implementation in CefRenderWidgetHostViewOSR:
//
// - the BeginFrameArgs carry the real frame time, deadline and interval of the
//   external clock. The historical path stamped the frame with the time of the
//   call, no deadline and a fixed 60 Hz interval; with no deadline the Display
//   drew as soon as the first frame sink had submitted, so a page and a
//   <video> that both animate were drawn one tick apart and the video dropped
//   frames (its submitter was still waiting for the ack of the previous frame
//   when the next BeginFrame arrived);
// - the sequence number comes from viz::BeginFrameSource::
//   BeginFrameArgsGenerator, so a missed tick leaves a hole in the sequence as
//   it would with a real vsync source;
// - the compositor is configured with wait_for_all_frame_sinks = false: a
//   tick is acknowledged as soon as the Display has drawn, not when every
//   frame sink has replied, which is also what the Wayland adapter does;
// - a tick that arrives while another one is in flight is kept (one deep)
//   with its own time stamps and issued when the acknowledgement comes back,
//   provided its interval is not half over; otherwise it is dropped like a
//   missed vsync. viz accepts a single IssueExternalBeginFrame in flight per
//   controller (mojo bad message otherwise), hence the gate.
//
// Every tick is reported exactly once to the delegate, in the order of the
// Tick() calls, as soon as its outcome is known: DRAWN or NOT_DRAWN from the
// BeginFrameAck of the Display, DROPPED when the gate discards it. A tick that
// is still pending when the source is destroyed is not reported.
//
// All methods are called on the UI thread.
class CefExternalBeginFrameSourceOSR
    : public ui::ExternalBeginFrameControllerClientFactory,
      public viz::mojom::ExternalBeginFrameControllerClient {
 public:
  class Delegate {
   public:
    // |compositor_idle| is true if no frame sink was requesting BeginFrames
    // when the tick was issued (sampled before the issue). Always false for a
    // DROPPED tick.
    virtual void OnExternalBeginFrameComplete(
        cef_external_begin_frame_result_t result,
        bool compositor_idle) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  CefExternalBeginFrameSourceOSR(ui::Compositor* compositor,
                                 Delegate* delegate);

  CefExternalBeginFrameSourceOSR(const CefExternalBeginFrameSourceOSR&) =
      delete;
  CefExternalBeginFrameSourceOSR& operator=(
      const CefExternalBeginFrameSourceOSR&) = delete;

  ~CefExternalBeginFrameSourceOSR() override;

  // Issue a BeginFrame for the tick that occurred at |frame_time| (at or
  // before now). |interval| is the period of the external clock and must be
  // positive. |deadline| is the Display deadline for this frame; a null value
  // selects |frame_time| + |interval|, which is what Chromium's own BeginFrame
  // sources use.
  void Tick(base::TimeTicks frame_time,
            base::TimeTicks deadline,
            base::TimeDelta interval);

  // The interval of the last tick (zero before the first tick).
  base::TimeDelta interval() const { return interval_; }

  // Whether viz currently asks for BeginFrames.
  bool needs_begin_frames() const { return needs_begin_frames_; }

  // Counters for diagnostics.
  uint64_t ticks_issued() const { return ticks_issued_; }
  uint64_t ticks_queued() const { return ticks_queued_; }
  uint64_t ticks_dropped() const { return ticks_dropped_; }

  // ui::ExternalBeginFrameControllerClientFactory methods:
  mojo::PendingAssociatedRemote<viz::mojom::ExternalBeginFrameControllerClient>
  CreateExternalBeginFrameControllerClient() override;

  // viz::mojom::ExternalBeginFrameControllerClient methods:
  void SetNeedsBeginFrame(bool needs_begin_frames) override;
  void SetPreferredInterval(base::TimeDelta interval) override;
  void NeedsBeginFrameWithId(int64_t display_id,
                             bool needs_begin_frames) override;

 private:
  struct PendingTick {
    base::TimeTicks frame_time;
    base::TimeTicks deadline;
    base::TimeDelta interval;
  };

  void Issue(const PendingTick& tick);
  void OnBeginFrameAck(const viz::BeginFrameAck& ack);

  // Report the outcome of the tick in flight, then the ticks dropped behind
  // it (they came after it, so they are reported after it).
  void ReportInFlight(cef_external_begin_frame_result_t result);
  void ReportDropped();

  static uint64_t AllocateSourceId();

  const raw_ptr<ui::Compositor> compositor_;
  const raw_ptr<Delegate> delegate_;
  const uint64_t source_id_ = AllocateSourceId();
  viz::BeginFrameSource::BeginFrameArgsGenerator args_generator_;
  mojo::AssociatedReceiverSet<viz::mojom::ExternalBeginFrameControllerClient>
      receivers_;

  base::TimeDelta interval_;
  base::TimeTicks last_frame_time_;
  bool in_flight_ = false;
  // Sampled when the tick in flight was issued: no frame sink wanted it.
  bool in_flight_idle_ = false;
  std::optional<PendingTick> queued_;
  // Ticks dropped behind the one in flight, reported once it is.
  uint64_t dropped_pending_ = 0;
  bool needs_begin_frames_ = false;

  uint64_t ticks_issued_ = 0;
  uint64_t ticks_queued_ = 0;
  uint64_t ticks_dropped_ = 0;

  base::WeakPtrFactory<CefExternalBeginFrameSourceOSR> weak_factory_{this};
};

#endif  // CEF_LIBCEF_BROWSER_OSR_EXTERNAL_BEGIN_FRAME_SOURCE_OSR_H_
