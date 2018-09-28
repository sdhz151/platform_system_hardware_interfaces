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
#include <android-base/unique_fd.h>
#include <cutils/native_handle.h>
#include <gtest/gtest.h>
#include <hidl/HidlTransportSupport.h>

#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

using android::sp;
using android::base::Socketpair;
using android::base::unique_fd;
using android::base::WriteStringToFd;
using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;
using android::hardware::Return;
using android::hardware::Void;
using android::system::suspend::V1_0::ISystemSuspend;
using android::system::suspend::V1_0::IWakeLock;
using android::system::suspend::V1_0::readFd;
using android::system::suspend::V1_0::SystemSuspend;

namespace android {

static constexpr char kServiceName[] = "TestService";

static bool isReadBlocked(int fd) {
    struct pollfd pfd {
        .fd = fd, .events = POLLIN,
    };
    int timeout_ms = 20;
    return poll(&pfd, 1, timeout_ms) == 0;
}

class SystemSuspendTestEnvironment : public ::testing::Environment {
   public:
    using Env = SystemSuspendTestEnvironment;
    static Env* Instance() {
        static Env* instance = new Env{};
        return instance;
    }

    SystemSuspendTestEnvironment() {
        Socketpair(SOCK_STREAM, &wakeupCountFds[0], &wakeupCountFds[1]);
        Socketpair(SOCK_STREAM, &stateFds[0], &stateFds[1]);
    }

    void registerTestService() {
        std::thread testService([this] {
            configureRpcThreadpool(1, true /* callerWillJoin */);
            sp<ISystemSuspend> suspend =
                new SystemSuspend(std::move(wakeupCountFds[1]), std::move(stateFds[1]));
            status_t status = suspend->registerAsService(kServiceName);
            if (android::OK != status) {
                LOG(FATAL) << "Unable to register service: " << status;
            }
            joinRpcThreadpool();
        });
        testService.detach();
    }

    virtual void SetUp() {
        registerTestService();
        ::android::hardware::details::waitForHwService(ISystemSuspend::descriptor, kServiceName);
        sp<ISystemSuspend> suspendService = ISystemSuspend::getService(kServiceName);
        ASSERT_NE(suspendService, nullptr) << "failed to get suspend service";
        ASSERT_EQ(suspendService->enableAutosuspend(), true) << "failed to start autosuspend";
    }

    unique_fd wakeupCountFds[2];
    unique_fd stateFds[2];
};

class SystemSuspendTest : public ::testing::Test {
   public:
    virtual void SetUp() override {
        ::android::hardware::details::waitForHwService(ISystemSuspend::descriptor, kServiceName);
        suspendService = ISystemSuspend::getService(kServiceName);
        ASSERT_NE(suspendService, nullptr) << "failed to get suspend service";

        auto* environment = SystemSuspendTestEnvironment::Instance();
        wakeupCountFd = environment->wakeupCountFds[0];
        stateFd = environment->stateFds[0];

        // SystemSuspend HAL should not have written back to wakeupCountFd or stateFd yet.
        ASSERT_TRUE(isReadBlocked(wakeupCountFd));
        ASSERT_TRUE(isReadBlocked(stateFd));
    }

    virtual void TearDown() override {
        if (!isReadBlocked(wakeupCountFd)) readFd(wakeupCountFd);
        if (!isReadBlocked(stateFd)) readFd(stateFd).empty();
        ASSERT_TRUE(isReadBlocked(wakeupCountFd));
        ASSERT_TRUE(isReadBlocked(stateFd));
    }

    void unblockSystemSuspendFromWakeupCount() {
        std::string wakeupCount = std::to_string(rand());
        ASSERT_TRUE(WriteStringToFd(wakeupCount, wakeupCountFd));
    }

    bool isSystemSuspendBlocked() { return isReadBlocked(stateFd); }

    sp<ISystemSuspend> suspendService;
    int stateFd;
    int wakeupCountFd;
};

// Tests that autosuspend thread can only be enabled once.
TEST_F(SystemSuspendTest, OnlyOneEnableAutosuspend) {
    ASSERT_EQ(suspendService->enableAutosuspend(), false);
}

TEST_F(SystemSuspendTest, AutosuspendLoop) {
    for (int i = 0; i < 2; i++) {
        // Mock value for /sys/power/wakeup_count.
        std::string wakeupCount = std::to_string(rand());
        ASSERT_TRUE(WriteStringToFd(wakeupCount, wakeupCountFd));
        ASSERT_EQ(readFd(wakeupCountFd), wakeupCount)
            << "wakeup count value written by SystemSuspend is not equal to value given to it";
        ASSERT_EQ(readFd(stateFd), "mem") << "SystemSuspend failed to write correct sleep state.";
    }
}

// Tests that upon WakeLock destruction SystemSuspend HAL is unblocked.
TEST_F(SystemSuspendTest, WakeLockDestructor) {
    {
        sp<IWakeLock> wl = suspendService->acquireWakeLock();
        ASSERT_NE(wl, nullptr);
        unblockSystemSuspendFromWakeupCount();
        ASSERT_TRUE(isSystemSuspendBlocked());
    }
    ASSERT_FALSE(isSystemSuspendBlocked());
}

// Tests that multiple WakeLocks correctly block SystemSuspend HAL.
TEST_F(SystemSuspendTest, MultipleWakeLocks) {
    {
        sp<IWakeLock> wl1 = suspendService->acquireWakeLock();
        ASSERT_NE(wl1, nullptr);
        ASSERT_TRUE(isSystemSuspendBlocked());
        unblockSystemSuspendFromWakeupCount();
        {
            sp<IWakeLock> wl2 = suspendService->acquireWakeLock();
            ASSERT_NE(wl2, nullptr);
            ASSERT_TRUE(isSystemSuspendBlocked());
        }
        ASSERT_TRUE(isSystemSuspendBlocked());
    }
    ASSERT_FALSE(isSystemSuspendBlocked());
}

// Tests that upon thread deallocation WakeLock is destructed and SystemSuspend HAL is unblocked.
TEST_F(SystemSuspendTest, ThreadCleanup) {
    std::thread clientThread([this] {
        sp<IWakeLock> wl = suspendService->acquireWakeLock();
        ASSERT_NE(wl, nullptr);
        unblockSystemSuspendFromWakeupCount();
        ASSERT_TRUE(isSystemSuspendBlocked());
    });
    clientThread.join();
    ASSERT_FALSE(isSystemSuspendBlocked());
}

// Test that binder driver correctly deallocates acquired WakeLocks, even if the client processs
// is terminated without ability to do clean up.
TEST_F(SystemSuspendTest, CleanupOnAbort) {
    ASSERT_EXIT(
        {
            sp<IWakeLock> wl = suspendService->acquireWakeLock();
            ASSERT_NE(wl, nullptr);
            std::abort();
        },
        ::testing::KilledBySignal(SIGABRT), "");
    ASSERT_TRUE(isSystemSuspendBlocked());
    unblockSystemSuspendFromWakeupCount();
    ASSERT_FALSE(isSystemSuspendBlocked());
}

}  // namespace android

int main(int argc, char** argv) {
    setenv("TREBLE_TESTING_OVERRIDE", "true", true);
    ::testing::AddGlobalTestEnvironment(android::SystemSuspendTestEnvironment::Instance());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
