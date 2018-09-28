/*
 * Copyright 2018 The Android Open Source Project
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

#include "SystemSuspend.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <hidl/Status.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <string>
#include <thread>

using ::android::base::ReadFdToString;
using ::android::base::WriteStringToFd;
using ::android::hardware::Void;
using ::std::string;

namespace android {
namespace system {
namespace suspend {
namespace V1_0 {

static const char kSleepState[] = "mem";

// This function assumes that data in fd is small enough that it can be read in one go.
// We use this function instead of the ones available in libbase because it doesn't block
// indefinitely when reading from socket streams which are used for testing.
string readFd(int fd) {
    char buf[BUFSIZ];
    ssize_t n = TEMP_FAILURE_RETRY(read(fd, &buf[0], sizeof(buf)));
    if (n < 0) return "";
    return string{buf, static_cast<size_t>(n)};
}

WakeLock::WakeLock(SystemSuspend* systemSuspend) : mSystemSuspend(systemSuspend) {
    mSystemSuspend->incSuspendCounter();
}

WakeLock::~WakeLock() {
    mSystemSuspend->decSuspendCounter();
}

SystemSuspend::SystemSuspend(unique_fd wakeupCountFd, unique_fd stateFd)
    : mCounterLock(),
      mCounterCondVar(),
      mSuspendCounter(0),
      mWakeupCountFd(std::move(wakeupCountFd)),
      mStateFd(std::move(stateFd)) {}

Return<bool> SystemSuspend::enableAutosuspend() {
    static bool initialized = false;
    if (initialized) {
        LOG(ERROR) << "Autosuspend already started.";
        return false;
    }

    initAutosuspend();
    initialized = true;
    return true;
}

Return<sp<IWakeLock>> SystemSuspend::acquireWakeLock() {
    return new WakeLock{this};
}

Return<void> SystemSuspend::debug(const hidl_handle& handle,
                                  const hidl_vec<hidl_string>& /* options */) {
    if (handle == nullptr || handle->numFds < 1 || handle->data[0] < 0) {
        LOG(ERROR) << "no valid fd";
        return Void();
    }
    int fd = handle->data[0];
    WriteStringToFd(std::to_string(mSuspendCounter) + "\n", fd);
    fsync(fd);
    return Void();
}

void SystemSuspend::incSuspendCounter() {
    auto l = std::lock_guard(mCounterLock);
    mSuspendCounter++;
}

void SystemSuspend::decSuspendCounter() {
    auto l = std::lock_guard(mCounterLock);
    if (--mSuspendCounter == 0) {
        mCounterCondVar.notify_one();
    }
}

void SystemSuspend::initAutosuspend() {
    std::thread autosuspendThread([this] {
        while (true) {
            lseek(mWakeupCountFd, 0, SEEK_SET);
            const string wakeupCount = readFd(mWakeupCountFd);
            if (wakeupCount.empty()) {
                PLOG(ERROR) << "error reading from /sys/power/wakeup_count";
                continue;
            }

            auto l = std::unique_lock(mCounterLock);
            mCounterCondVar.wait(l, [this] { return mSuspendCounter == 0; });
            // The mutex is locked and *MUST* remain locked until the end of the scope. Otherwise,
            // a WakeLock might be acquired after we check mSuspendCounter and before we write to
            // /sys/power/state.

            if (!WriteStringToFd(wakeupCount, mWakeupCountFd)) {
                PLOG(VERBOSE) << "error writing from /sys/power/wakeup_count";
                continue;
            }
            if (!WriteStringToFd(kSleepState, mStateFd)) {
                PLOG(VERBOSE) << "error writing to /sys/power/state";
            }
        }
    });
    autosuspendThread.detach();
    LOG(INFO) << "automatic system suspend enabled";
}

}  // namespace V1_0
}  // namespace suspend
}  // namespace system
}  // namespace android
