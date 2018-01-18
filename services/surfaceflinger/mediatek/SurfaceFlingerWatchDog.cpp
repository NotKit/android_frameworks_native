/* Copyright Statement:
 *
 * This software/firmware and related documentation ("MediaTek Software") are
 * protected under relevant copyright laws. The information contained herein is
 * confidential and proprietary to MediaTek Inc. and/or its licensors. Without
 * the prior written permission of MediaTek inc. and/or its licensors, any
 * reproduction, modification, use or disclosure of MediaTek Software, and
 * information contained herein, in whole or in part, shall be strictly
 * prohibited.
 *
 * MediaTek Inc. (C) 2010. All rights reserved.
 *
 * BY OPENING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
 * THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
 * RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER
 * ON AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR
 * NONINFRINGEMENT. NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH
 * RESPECT TO THE SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY,
 * INCORPORATED IN, OR SUPPLIED WITH THE MEDIATEK SOFTWARE, AND RECEIVER AGREES
 * TO LOOK ONLY TO SUCH THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO.
 * RECEIVER EXPRESSLY ACKNOWLEDGES THAT IT IS RECEIVER'S SOLE RESPONSIBILITY TO
 * OBTAIN FROM ANY THIRD PARTY ALL PROPER LICENSES CONTAINED IN MEDIATEK
 * SOFTWARE. MEDIATEK SHALL ALSO NOT BE RESPONSIBLE FOR ANY MEDIATEK SOFTWARE
 * RELEASES MADE TO RECEIVER'S SPECIFICATION OR TO CONFORM TO A PARTICULAR
 * STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S
 * ENTIRE AND CUMULATIVE LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE
 * RELEASED HEREUNDER WILL BE, AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE
 * MEDIATEK SOFTWARE AT ISSUE, OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE
 * CHARGE PAID BY RECEIVER TO MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.
 *
 * The following software/firmware and/or related documentation ("MediaTek
 * Software") have been modified by MediaTek Inc. All revisions are subject to
 * any receiver's applicable license agreements with MediaTek Inc.
 */

/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <inttypes.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <selinux/android.h>
#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/debugger.h>
#include <fcntl.h>

#include "SurfaceFlingerWatchDog.h"

#ifdef HAVE_AEE_FEATURE
#include "aee.h"
#else
#include <fcntl.h>
#endif

namespace android {


#ifdef HAVE_AEE_FEATURE
// AED Exported Functions
static int aee_ioctl_wdt_kick(int value) {
    int ret = 0;
    int fd = open(AE_WDT_DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        ALOGD("[SF-WD] ERROR: open %s failed.", AE_WDT_DEVICE_PATH);
        return 1;
    } else {
        if (ioctl(fd, AEEIOCTL_WDT_KICK_POWERKEY, (int)value) != 0) {
            ALOGD("[SF-WD] ERROR(%s): aee wdt kick powerkey ioctl failed.", strerror(errno));
            close(fd);
            return 1;
        }
    }
    close(fd);
    return ret;
}

static int aee_ioctl_swt_set(nsecs_t time) {
    int ret = 0;
    int fd = open(AE_WDT_DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        ALOGD("[SF-WD] ERROR: open %s failed.", AE_WDT_DEVICE_PATH);
        return 1;
    } else {
        if (ioctl(fd, AEEIOCTL_SET_SF_STATE, (long long)(&time)) != 0) {
            ALOGD("[SF-WD] ERROR(%s): aee swt set state ioctl failed.", strerror(errno));
            close(fd);
            return 1;
        }
    }
    close(fd);
    return ret;
}
#endif



//==================================================================================================
// RTTDumper
//
#define SF_WATCHDOG_RTTCOUNT    10
#define RTT_FOLDER_PATH         "/data/anr/SF_RTT/"
#define RTT_FILE_NAME           "rtt_dump"
#define RTT_DUMP                (RTT_FOLDER_PATH RTT_FILE_NAME)
#define RTT_IDLE                1
typedef KeyedVector<int, String8 > VDSPIDLIST;
class RTTDumper : public Thread {
public:
    RTTDumper() :
        mLastFinishTime(0) {}
    ~RTTDumper() {}

    bool dumpRTT() {
        if (!isRunning()) {
            nsecs_t idleTime = 0;
            {
                Mutex::Autolock _l(mLastFinishTimeLock);
                idleTime = systemTime() - mLastFinishTime;
            }
            if (idleTime > s2ns(RTT_IDLE))
                return run("RTT_Dumper", PRIORITY_NORMAL) == NO_ERROR;
            else
                ALOGD("[SF-WD] RTT Dumper cool down");
        }
        return false;
    }

    void setVDSConsumerList(VDSPIDLIST& VDSConsumerList) {
        if (isRunning()) {
            ALOGW("[SF-WD] Can not set VDS consumer during RTT dump");
            return;
        }

        // save VDS list
        Mutex::Autolock _l(mVDSListLock);
        mVDSConsumerList.clear();
        if (!VDSConsumerList.isEmpty()) {
            int nVDSConsumerPid = 0;
            String8 strVDSInfo;
            size_t nVDSCount = VDSConsumerList.size();
            for (size_t i = 0; i < nVDSCount; ++i) {
                nVDSConsumerPid = VDSConsumerList.keyAt(i);
                strVDSInfo = VDSConsumerList.valueAt(i);
                mVDSConsumerList.add(nVDSConsumerPid, strVDSInfo);
            }
        }
    }

private:
    VDSPIDLIST mVDSConsumerList;
    mutable Mutex mVDSListLock;
    nsecs_t mLastFinishTime;
    mutable Mutex mLastFinishTimeLock;

    bool createFolder(const char* path) {
        struct stat sb;
        if (stat(path, &sb) != 0) {
            if (mkdir(path, 0777) != 0) {
                ALOGE("[SF-WD-RTT] mkdir(%s) failed: %s", path, strerror(errno));
                return false;
            }
            if (selinux_android_restorecon(path, 0) == -1) {
                ALOGE("[SF-WD-RTT] restorecon failed(%s) failed", path);
                return false;
            } else {
                ALOGV("[SF-WD-RTT] restorecon(%s)", path);
            }
        }
        return true;
    }

    bool threadLoop() {
        static uint32_t rtt_ct = SF_WATCHDOG_RTTCOUNT;
        if (rtt_ct > 0) {
            rtt_ct --;
        } else {
            ALOGD("[SF-WD-RTT] swap rtt dump file");

            // swap rtt dump file
            char cmds[256];
            snprintf(cmds, sizeof(cmds), "mv %s.txt %s_1.txt", RTT_DUMP, RTT_DUMP);
            system(cmds);

            rtt_ct = SF_WATCHDOG_RTTCOUNT;
        }

        // create rtt folder
        createFolder(RTT_FOLDER_PATH);

        // append SurfaceFlinger rtt information to rtt file
        char filename[100];
        snprintf(filename, sizeof(filename), "%s.txt", RTT_DUMP);
        int fd = open(filename, O_CREAT | O_WRONLY | O_NOFOLLOW, 0666);  /* -rw-rw-rw- */
        if (fd < 0) {
            ALOGE("[SF-WD-RTT] Can't open %s: %s\n", filename, strerror(errno));
        } else {
            if (lseek(fd, 0, SEEK_END) < 0) {
                fprintf(stderr, "lseek: %s\n", strerror(errno));
            } else {
                // it might lost data since SWWatachDog thread blocked when debuggerd is dumping backtrace
                // fork child proc to dump backtrace to avoid lost data
                const pid_t pid = getpid();
                const int new_pid = fork();
                if (new_pid == 0) {
                    if (dump_backtrace_to_file(pid, fd) < 0) {
                        ALOGE("[SF-WD-RTT] dump_backtrace_to_file() failed!");
                    }
                    close(fd);
                    _exit(0);
                } else if (new_pid > 0) {
                    const nsecs_t startDump = systemTime();
                    waitpid(new_pid, NULL, 0);
                    ALOGD("[SF-WD-RTT] dump backtrace time: %" PRId64 "ms", ns2ms(systemTime() - startDump));
                } else {
                    ALOGE("[SF-WD-RTT] fork() failed! (%d)", __LINE__);
                }

                Mutex::Autolock _l(mVDSListLock);
                int nVDSConsumerPid = 0;
                String8 info;
                for (size_t i = 0; i < mVDSConsumerList.size(); ++i) {
                    nVDSConsumerPid = mVDSConsumerList.keyAt(i);
                    info = mVDSConsumerList.valueAt(i);

                    if (nVDSConsumerPid > 0) {
                        write(fd, info.string(), info.size());
                        dump_backtrace_to_file(nVDSConsumerPid, fd);
                    } else {
                        ALOGE("[SF-WD-RTT] Incorrect VDS consumer pid:%d [%s] (%zu/%zu)",
                            nVDSConsumerPid, info.string(), i, mVDSConsumerList.size());
                    }
                }
            }
            close(fd);
            ALOGD("[SF-WD-RTT] dump rtt file: %s.txt", RTT_DUMP);
        }

        {
            Mutex::Autolock _l(mLastFinishTimeLock);
            mLastFinishTime = systemTime();
        }
        return false;
    }
};



//==================================================================================================
// WDNotify
//
class WDNotify : public SWWatchDog::Recipient {
public:
    explicit WDNotify(SFWatchDog* wdt) :
        mSFWdt(wdt) {}

    ~WDNotify() {}

    void onSetAnchor(const SWWatchDog::anchor_id_t& id, const pid_t& tid, const nsecs_t& anchorTime,
                     const String8& msg) {
        mSFWdt->onSetAnchor(id, tid, anchorTime, msg);
    }

    void onDelAnchor(const SWWatchDog::anchor_id_t& id, const pid_t& tid, const nsecs_t& anchorTime,
                     const String8& msg, const nsecs_t& now) {
        mSFWdt->onDelAnchor(id, tid, anchorTime, msg, now);
    }

    void onTimeout(  const SWWatchDog::anchor_id_t& id, const pid_t& tid, const nsecs_t& anchorTime,
                     const String8& msg, const nsecs_t& now) {
        mSFWdt->onTimeout(id, tid, anchorTime, msg, now);
    }

    void onTick(const nsecs_t& now) {
        mSFWdt->onTick(now);
    }

private:
    SFWatchDog* mSFWdt;
};



//==================================================================================================
// SFWatchDog
//
SFWatchDog::SFWatchDog() :
    mShowLog(false),
    mUpdateCount(0),
    mTimer(SWWatchDog::DEFAULT_TIMER),
    mThreshold(SWWatchDog::DEFAULT_THRESHOLD),
    mTimeout(false),
    mID(SWWatchDog::NO_ANCHOR),
    mFreshAnchor(true) {
    sp<WDNotify> notify = new WDNotify(this);
    SWWatchDog::setWDTNotify(notify);
    SWWatchDog::setTickNotify(notify);

    mRTTDumper = new RTTDumper;
}

SFWatchDog::~SFWatchDog() {}

bool SFWatchDog::setAnchor(const String8& msg) {
#ifndef DISABLE_SWWDT
    if (mID != SWWatchDog::NO_ANCHOR) {
        ALOGE("[SF-WD] anchor(%#" PRIxPTR ") already exist, replacing by <<%s>>", mID, msg.string());
        return false;
    }
    {
        Mutex::Autolock _l(mFreshAnchorLock);
        mFreshAnchor = true;
    }
    mID = SWWatchDog::setAnchor(msg, mThreshold);
    return mID != SWWatchDog::NO_ANCHOR;
#else
    (void)(mID);
    (void)(msg);
    return true;
#endif
}

bool SFWatchDog::delAnchor() {
#ifndef DISABLE_SWWDT
    if (mID == SWWatchDog::NO_ANCHOR) {
        ALOGE("[SF-WD] [%s] There is no anchor.", __func__);
        return false;
    }
    anchor_id_t id = mID;
    mID = SWWatchDog::NO_ANCHOR;
    return SWWatchDog::delAnchor(id);
#else
    return true;
#endif
}

ssize_t SFWatchDog::registerVirtualDisplay(const sp<ANativeWindow>& nativeWindow, const String8& info) {
#ifndef DISABLE_SWWDT
    Mutex::Autolock _l(mVDSListLock);
    ssize_t size = 0;

    const ssize_t i = mVDSList.indexOfKey(nativeWindow);

    if (i > 0) {
        ALOGI("register an already registered VDS");
        return i;
    }
    mVDSList.add(nativeWindow, info);
    size = mVDSList.size();
    ALOGI("[%s] registered VDS:%p, size:%zd,", __func__, nativeWindow.get(), size);

    return size;
#else
    (void)(nativeWindow);
    (void)(info);
    return 0;
#endif
}

ssize_t SFWatchDog::unregisterVirtualDisplay(const sp<ANativeWindow>& nativeWindow) {
#ifndef DISABLE_SWWDT
    Mutex::Autolock _l(mVDSListLock);
    ssize_t size = 0;

    const ssize_t i = mVDSList.indexOfKey(nativeWindow);
    if (i < 0) {
        ALOGI("unregister an already VDS");
        return i;
    }
    mVDSList.removeItem(nativeWindow);
    size = mVDSList.size();
    ALOGI("[%s] unregistered VDS:%p, size:%zd", __func__, nativeWindow.get(), size);

    return size;
#else
    (void)(nativeWindow);
    return 0;
#endif
}

void SFWatchDog::setSuspend() {
    SWWatchDog::suspend();
#ifdef HAVE_AEE_FEATURE
    {
        Mutex::Autolock _l(mAEELock);
        aee_ioctl_wdt_kick(WDT_SETBY_SF);
    }
#endif
}

void SFWatchDog::setResume() {
#ifdef HAVE_AEE_FEATURE
    {
        Mutex::Autolock _l(mAEELock);
        aee_ioctl_wdt_kick(WDT_SETBY_SF);
    }
#endif
    SWWatchDog::resume();
}

void SFWatchDog::setThreshold(const SFWatchDog::msecs_t& threshold) {
    if (threshold != mThreshold) {
        ALOGD("SF watch dog change threshold from %" PRId64 " --> %" PRId64 ".", mThreshold, threshold);

        // SFWatchDog::mThreshold unit is ms.
        // SWWatchDog::mThreshold unit is ns.
        // Because SFWatchDog::mThreshold hide SWWatchDog::mThreshold.
        // therefore, we should set SWWatchDog::mThreshold via SWWatchDog::setThreshold().
        mThreshold = threshold;
        SWWatchDog::setThreshold(mThreshold);
    }
}

void SFWatchDog::getProperty() {
    char value[PROPERTY_VALUE_MAX];

    if (property_get("debug.sf.wdthreshold", value, NULL) > 0) {
        nsecs_t threshold = static_cast<nsecs_t>(atoi(value));
        setThreshold(threshold);
    }

    if (property_get("debug.sf.wdtimer", value, NULL) > 0) {
        nsecs_t timer = static_cast<nsecs_t>(atoi(value));
        if (timer != mTimer) {
            ALOGD("SF watch dog change timer from %" PRId64 " --> %" PRId64 ".", mTimer, timer);
            mTimer = timer;
            setTimer(mTimer);
        }
    }

    property_get("debug.sf.wdlog", value, "0");
    mShowLog = atoi(value);
}

void SFWatchDog::onSetAnchor(const SWWatchDog::anchor_id_t& /*id*/, const pid_t& tid,
                             const nsecs_t& anchorTime, const String8& msg) {
    if (mShowLog) {
        ALOGV("[SF-WD] Set Anchor <<%s>> TID=%d, AnchorTime=%" PRId64 ".",
            msg.string(), tid, anchorTime);
    }

    ++mUpdateCount;
}

void SFWatchDog::onDelAnchor(const SWWatchDog::anchor_id_t& /*id*/, const pid_t& tid,
                             const nsecs_t& anchorTime, const String8& msg, const nsecs_t& now) {
    if (mShowLog) {
        ALOGV("[SF-WD] Delete Anchor <<%s>> TID=%d, AnchorTime=%" PRId64 " SpendTime=%" PRId64 ".",
            msg.string(), tid, anchorTime, now - anchorTime);
    }
}

void SFWatchDog::onTimeout( const SWWatchDog::anchor_id_t& /*id*/, const pid_t& tid,
                            const nsecs_t& anchorTime, const String8& msg, const nsecs_t& now) {
    String8 warningMessage;
    {
        Mutex::Autolock _l(mFreshAnchorLock);
        warningMessage = mFreshAnchor ? "performance down" : "maybe hang";
        mFreshAnchor = false;
    }
    ALOGW("[SF-WD] ============================================");
    ALOGW("[SF-WD] SF %s, TID=%d SpendTime=%" PRId64 "ms Threshold=%" PRId64
          "ms AnchorTime=%" PRId64 " Now=%" PRId64 ".",
            warningMessage.string(), tid, ns2ms(now - anchorTime), getThreshold(), anchorTime, now);
    ALOGW("[SF-WD] %s", msg.string());

    if (mRTTDumper->isRunning()) {
        ALOGW("[SF-WD] RTT is still dumping");
    } else {
        // pass VDS consumer list to RTTDumper
        VDSPIDLIST VDSPidList;
        {
            Mutex::Autolock _l(mVDSListLock);
            const size_t dc = mVDSList.size();
            int32_t cPid = -1;
            sp<ANativeWindow> nativeWindow;
            String8 info;
            for(size_t i = 0; i < dc; ++i) {
                nativeWindow =  mVDSList.keyAt(i);
                info = mVDSList.valueAt(i);
                nativeWindow->query(nativeWindow.get(), NATIVE_WINDOW_CONSUMER_PID, &cPid);
                ALOGI("[SF-WD] Try to get Consumer PID: %d", cPid);
                if (cPid > 0) {
                    VDSPidList.add(cPid, info);
                } else {
                    ALOGW("[SF-WD] Can't get consmuer pid of Virtual Display !!");
                }
            }
        }
        mRTTDumper->setVDSConsumerList(VDSPidList);

        if (mRTTDumper->dumpRTT()) {
            ALOGW("[SF-WD] Start to dump RTT");
        } else {
            ALOGW("[SF-WD] Start RTT Dumper fail");
        }
    }
    ALOGW("[SF-WD] ============================================");

#ifdef HAVE_AEE_FEATURE
    nsecs_t spendTime = 1;
    if (getThreshold() != 0) {
        spendTime = ns2ms(now - anchorTime);
    }
    aee_ioctl_swt_set(spendTime);
#endif
    mTimeout = true;
}

void SFWatchDog::onTick(const nsecs_t& now) {
    if (mShowLog) {
        ALOGI("[SF-WD] Tick Now=%" PRId64 ".", now);
    }

    getProperty();

#ifdef HAVE_AEE_FEATURE
    if (!mTimeout) {
        aee_ioctl_swt_set(1);
    }
#endif
    mTimeout = false;

    if (mUpdateCount) {
        if (mShowLog)
            ALOGD("[SF-WD] mUpdateCount: %d", mUpdateCount);
#ifdef HAVE_AEE_FEATURE
        {
            Mutex::Autolock _l(mAEELock);
            aee_ioctl_wdt_kick(WDT_SETBY_SF);
        }
#endif
        mUpdateCount = 0;
    }
}

};  // namespace android
