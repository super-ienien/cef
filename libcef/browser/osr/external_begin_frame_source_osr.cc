// Copyright 2026 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "cef/libcef/browser/osr/external_begin_frame_source_osr.h"

#include "base/atomic_sequence_num.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/trace_event/trace_event.h"

// static
uint64_t CefExternalBeginFrameSourceOSR::AllocateSourceId() {
  static base::AtomicSequenceNumber counter;
  return counter.GetNext() + 1;
}

CefExternalBeginFrameSourceOSR::CefExternalBeginFrameSourceOSR(
    ui::Compositor* compositor)
    : compositor_(compositor) {
  CHECK(compositor_);
  // Acknowledge a tick as soon as the Display has drawn, not when every frame
  // sink has replied. This is what ui::ExternalBeginFrameAdapter does
  // ("prevents deadlocks when sinks join after frames are displayed"), and it
  // keeps the gate below open for most of the interval.
  compositor_->set_wait_for_all_frame_sinks(false);
  compositor_->SetExternalBeginFrameControllerClientFactory(this);
}

CefExternalBeginFrameSourceOSR::~CefExternalBeginFrameSourceOSR() {
  compositor_->SetExternalBeginFrameControllerClientFactory(nullptr);
}

void CefExternalBeginFrameSourceOSR::Tick(base::TimeTicks frame_time,
                                          base::TimeTicks deadline,
                                          base::TimeDelta interval) {
  DCHECK(interval.is_positive());
  if (!interval.is_positive()) {
    return;
  }

  if (interval != interval_) {
    interval_ = interval;
    // Keep the vsync parameters of the compositor in step with the external
    // clock (ui::ExternalBeginFrameAdapter::OnVSyncIntervalChanged). The
    // historical path set them once, with the time of the call and the
    // windowless frame rate.
    compositor_->SetDisplayVSyncParameters(frame_time, interval);
  }

  // viz drops a BeginFrame whose frame time does not advance: keep it strictly
  // increasing even if two ticks are stamped with the same instant.
  if (!last_frame_time_.is_null() && frame_time <= last_frame_time_) {
    frame_time = last_frame_time_ + base::Microseconds(1);
  }
  if (deadline.is_null() || deadline <= frame_time) {
    deadline = frame_time + interval;
  }

  const PendingTick tick{frame_time, deadline, interval};
  if (in_flight_) {
    // One tick in flight at a time (viz reports a bad message otherwise).
    // Keep this one, with its own time stamps, for the acknowledgement.
    if (queued_) {
      ++ticks_dropped_;
    }
    queued_ = tick;
    ++ticks_queued_;
    if ((ticks_queued_ % 600) == 1) {
      LOG(WARNING) << "External BeginFrame arrived while the previous frame "
                      "was pending; queued ("
                   << ticks_queued_ << " so far, " << ticks_dropped_
                   << " dropped)";
    }
    return;
  }
  Issue(tick);
}

void CefExternalBeginFrameSourceOSR::Issue(const PendingTick& tick) {
  in_flight_ = true;
  last_frame_time_ = tick.frame_time;
  ++ticks_issued_;

  // The generator estimates the sequence number from the time elapsed since
  // the previous tick, so a tick that was dropped leaves a hole, as with a
  // real vsync source; it also fills in |unthrottled_interval|.
  viz::BeginFrameArgs args = args_generator_.GenerateBeginFrameArgs(
      source_id_, tick.frame_time, tick.deadline, tick.interval, tick.interval);
  TRACE_EVENT2("viz", "CefExternalBeginFrameSourceOSR::Issue", "frame_time",
               tick.frame_time, "sequence_number",
               args.frame_id.sequence_number);
  DCHECK(args.IsValid());

  compositor_->IssueExternalBeginFrame(
      args, base::BindOnce(&CefExternalBeginFrameSourceOSR::OnBeginFrameAck,
                           weak_factory_.GetWeakPtr()));
}

void CefExternalBeginFrameSourceOSR::OnBeginFrameAck(
    const viz::BeginFrameAck& ack) {
  DCHECK(in_flight_);
  in_flight_ = false;
  if (!queued_) {
    return;
  }

  const PendingTick tick = *queued_;
  queued_.reset();

  // Issue the kept tick with its own time stamps while its interval is not
  // half over; past that point the next tick is due and the slot stays empty,
  // as it would with a missed vsync.
  if (base::TimeTicks::Now() - tick.frame_time < tick.interval / 2) {
    Issue(tick);
  } else {
    ++ticks_dropped_;
  }
}

mojo::PendingAssociatedRemote<viz::mojom::ExternalBeginFrameControllerClient>
CefExternalBeginFrameSourceOSR::CreateExternalBeginFrameControllerClient() {
  TRACE_EVENT0("ui", "CefExternalBeginFrameSourceOSR::CreateClient");
  // A new root frame sink (first creation, or re-creation after a GPU process
  // crash): nothing is in flight anymore, and the sequence restarts.
  receivers_.Clear();
  in_flight_ = false;
  queued_.reset();
  last_frame_time_ = base::TimeTicks();
  args_generator_ = viz::BeginFrameSource::BeginFrameArgsGenerator();

  mojo::PendingAssociatedRemote<viz::mojom::ExternalBeginFrameControllerClient>
      remote;
  receivers_.Add(this, remote.InitWithNewEndpointAndPassReceiver());
  needs_begin_frames_ = true;
  return remote;
}

void CefExternalBeginFrameSourceOSR::SetNeedsBeginFrame(
    bool needs_begin_frames) {
  // Informational: the client clocks the ticks and keeps sending them, as it
  // did with the historical implementation.
  needs_begin_frames_ = needs_begin_frames;
}

void CefExternalBeginFrameSourceOSR::SetPreferredInterval(
    base::TimeDelta interval) {
  // The external clock is not negotiable.
}

void CefExternalBeginFrameSourceOSR::NeedsBeginFrameWithId(
    int64_t display_id,
    bool needs_begin_frames) {
  SetNeedsBeginFrame(needs_begin_frames);
}
