/*
 * Copyright 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <scheduler/Fps.h>
#include <scheduler/Timer.h>

#include "VsyncSchedule.h"

#include "ISchedulerCallback.h"
#include "Scheduler.h"
#include "Utils/Dumper.h"
#include "VSyncDispatchTimerQueue.h"
#include "VSyncPredictor.h"
#include "VSyncReactor.h"

#include "../TracedOrdinal.h"

namespace android::scheduler {

class VsyncSchedule::PredictedVsyncTracer {
    // Invoked from the thread of the VsyncDispatch owned by this VsyncSchedule.
    constexpr auto makeVsyncCallback() {
        return [this](nsecs_t, nsecs_t, nsecs_t) {
            mParity = !mParity;
            schedule();
        };
    }

public:
    explicit PredictedVsyncTracer(std::shared_ptr<VsyncDispatch> dispatch)
          : mRegistration(std::move(dispatch), makeVsyncCallback(), __func__) {
        schedule();
    }

private:
    void schedule() { mRegistration.schedule({0, 0, 0}); }

    TracedOrdinal<bool> mParity = {"VSYNC-predicted", 0};
    VSyncCallbackRegistration mRegistration;
};

VsyncSchedule::VsyncSchedule(PhysicalDisplayId id, FeatureFlags features)
      : mId(id),
        mTracker(createTracker(id)),
        mDispatch(createDispatch(mTracker)),
        mController(createController(id, *mTracker, features)) {
    if (features.test(Feature::kTracePredictedVsync)) {
        mTracer = std::make_unique<PredictedVsyncTracer>(mDispatch);
    }
}

VsyncSchedule::VsyncSchedule(PhysicalDisplayId id, TrackerPtr tracker, DispatchPtr dispatch,
                             ControllerPtr controller)
      : mId(id),
        mTracker(std::move(tracker)),
        mDispatch(std::move(dispatch)),
        mController(std::move(controller)) {}

VsyncSchedule::~VsyncSchedule() = default;

Period VsyncSchedule::period() const {
    return Period::fromNs(mTracker->currentPeriod());
}

TimePoint VsyncSchedule::vsyncDeadlineAfter(TimePoint timePoint) const {
    return TimePoint::fromNs(mTracker->nextAnticipatedVSyncTimeFrom(timePoint.ns()));
}

void VsyncSchedule::dump(std::string& out) const {
    utils::Dumper dumper(out);
    {
        std::lock_guard<std::mutex> lock(mHwVsyncLock);
        dumper.dump("hwVsyncState", ftl::enum_string(mHwVsyncState));
        dumper.dump("lastHwVsyncState", ftl::enum_string(mLastHwVsyncState));
    }

    out.append("VsyncController:\n");
    mController->dump(out);

    out.append("VsyncDispatch:\n");
    mDispatch->dump(out);
}

VsyncSchedule::TrackerPtr VsyncSchedule::createTracker(PhysicalDisplayId id) {
    // TODO(b/144707443): Tune constants.
    constexpr nsecs_t kInitialPeriod = (60_Hz).getPeriodNsecs();
    constexpr size_t kHistorySize = 20;
    constexpr size_t kMinSamplesForPrediction = 6;
    constexpr uint32_t kDiscardOutlierPercent = 20;

    return std::make_unique<VSyncPredictor>(to_string(id), kInitialPeriod, kHistorySize,
                                            kMinSamplesForPrediction, kDiscardOutlierPercent);
}

VsyncSchedule::DispatchPtr VsyncSchedule::createDispatch(TrackerPtr tracker) {
    using namespace std::chrono_literals;

    // TODO(b/144707443): Tune constants.
    constexpr std::chrono::nanoseconds kGroupDispatchWithin = 500us;
    constexpr std::chrono::nanoseconds kSnapToSameVsyncWithin = 3ms;

    return std::make_unique<VSyncDispatchTimerQueue>(std::make_unique<Timer>(), std::move(tracker),
                                                     kGroupDispatchWithin.count(),
                                                     kSnapToSameVsyncWithin.count());
}

VsyncSchedule::ControllerPtr VsyncSchedule::createController(PhysicalDisplayId id,
                                                             VsyncTracker& tracker,
                                                             FeatureFlags features) {
    // TODO(b/144707443): Tune constants.
    constexpr size_t kMaxPendingFences = 20;
    const bool hasKernelIdleTimer = features.test(Feature::kKernelIdleTimer);

    auto reactor = std::make_unique<VSyncReactor>(to_string(id), std::make_unique<SystemClock>(),
                                                  tracker, kMaxPendingFences, hasKernelIdleTimer);

    reactor->setIgnorePresentFences(!features.test(Feature::kPresentFences));
    return reactor;
}

void VsyncSchedule::enableHardwareVsync(ISchedulerCallback& callback) {
    std::lock_guard<std::mutex> lock(mHwVsyncLock);
    if (mHwVsyncState == HwVsyncState::Disabled) {
        getTracker().resetModel();
        callback.setVsyncEnabled(mId, true);
        mHwVsyncState = HwVsyncState::Enabled;
        mLastHwVsyncState = HwVsyncState::Enabled;
    }
}

void VsyncSchedule::disableHardwareVsync(ISchedulerCallback& callback, bool disallow) {
    std::lock_guard<std::mutex> lock(mHwVsyncLock);
    if (mHwVsyncState == HwVsyncState::Enabled) {
        callback.setVsyncEnabled(mId, false);
        mLastHwVsyncState = HwVsyncState::Disabled;
    }
    mHwVsyncState = disallow ? HwVsyncState::Disallowed : HwVsyncState::Disabled;
}

bool VsyncSchedule::isHardwareVsyncAllowed() const {
    std::lock_guard<std::mutex> lock(mHwVsyncLock);
    return mHwVsyncState != HwVsyncState::Disallowed;
}

void VsyncSchedule::allowHardwareVsync() {
    std::lock_guard<std::mutex> lock(mHwVsyncLock);
    if (mHwVsyncState == HwVsyncState::Disallowed) {
        mHwVsyncState = HwVsyncState::Disabled;
    }
}

} // namespace android::scheduler
