/* Copyright (c) 2015, 2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "ExSurfaceFlinger.h"
#include <fstream>
#include <cutils/properties.h>
#include <ui/GraphicBufferAllocator.h>

namespace android {

bool ExSurfaceFlinger::sAllowHDRFallBack = false;

ExSurfaceFlinger::ExSurfaceFlinger() {
    char property[PROPERTY_VALUE_MAX] = {0};

    mDebugLogs = false;
    if ((property_get("vendor.display.qdframework_logs", property, NULL) > 0) &&
        (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
         (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
        mDebugLogs = true;
    }

    ALOGD_IF(isDebug(),"Creating custom SurfaceFlinger %s",__FUNCTION__);

    mDisableExtAnimation = false;
    if ((property_get("vendor.display.disable_ext_anim", property, "0") > 0) &&
        (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
         (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
        mDisableExtAnimation = true;
    }

    ALOGD_IF(isDebug(),"Animation on external is %s in %s",
             mDisableExtAnimation ? "disabled" : "not disabled", __FUNCTION__);

    if((property_get("vendor.display.hwc_disable_hdr", property, "0") > 0) &&
       (!strncmp(property, "1", PROPERTY_VALUE_MAX ) ||
        (!strncasecmp(property,"true", PROPERTY_VALUE_MAX )))) {
       sAllowHDRFallBack = true;
    }

    using vendor::display::config::V1_2::IDisplayConfig;
    mDisplayConfig = IDisplayConfig::getService();
    if (mDisplayConfig != NULL) {
        if (!mDisplayConfig->setDisplayIndex(IDisplayConfig::DisplayTypeExt::DISPLAY_BUILTIN,
                 HWC_DISPLAY_BUILTIN_2, (HWC_DISPLAY_VIRTUAL - HWC_DISPLAY_BUILTIN_2)) &&
            !mDisplayConfig->setDisplayIndex(IDisplayConfig::DisplayTypeExt::DISPLAY_PLUGGABLE,
                 HWC_DISPLAY_EXTERNAL, (HWC_DISPLAY_BUILTIN_2 - HWC_DISPLAY_EXTERNAL)) &&
            !mDisplayConfig->setDisplayIndex(IDisplayConfig::DisplayTypeExt::DISPLAY_VIRTUAL,
                 HWC_DISPLAY_VIRTUAL, 1)) {
        }
    }
}

ExSurfaceFlinger::~ExSurfaceFlinger() { }

void ExSurfaceFlinger::handleDPTransactionIfNeeded(
        const Vector<DisplayState>& displays) {
    /* Wait for one draw cycle before setting display projection only when the disable
     * external rotation animation feature is enabled
     */
    if (mDisableExtAnimation) {
        size_t count = displays.size();
        bool builtin_orient_changed = false;
        for (size_t i=0 ; i<count ; i++) {
            const DisplayState& s(displays[i]);
            sp<DisplayDevice> device(getDisplayDevice(s.token));
            if (device == nullptr) {
                continue;
            }
            int type = device->getDisplayType();
            if ((type > DisplayDevice::DISPLAY_ID_INVALID &&
                type < DisplayDevice::NUM_BUILTIN_DISPLAY_TYPES && mBuiltInBitmask.test(type)) &&
                !(s.orientation & DisplayState::eOrientationUnchanged)) {
                builtin_orient_changed = true;
                break;
            }
        }
        for (size_t i=0 ; i<count ; i++) {
            const DisplayState& s(displays[i]);
            sp<DisplayDevice> device(getDisplayDevice(s.token));
            if (device == nullptr) {
                continue;
            }
            int type = device->getDisplayType();
            if (type == DisplayDevice::DISPLAY_VIRTUAL ||
                (type > DisplayDevice::DISPLAY_ID_INVALID &&
                type < DisplayDevice::NUM_BUILTIN_DISPLAY_TYPES && !mBuiltInBitmask.test(type))) {
                const uint32_t what = s.what;
                /* Invalidate and wait on eDisplayProjectionChanged to trigger a draw cycle so that
                 * it can fix one incorrect frame on the External, when we
                 * disable external animation
                 */
                if (what & DisplayState::eDisplayProjectionChanged && builtin_orient_changed) {
                    Mutex::Autolock lock(mExtAnimationLock);
                    invalidateHwcGeometry();
                    android_atomic_or(1, &mRepaintEverything);
                    signalRefresh();
                    status_t err = mExtAnimationCond.waitRelative(mExtAnimationLock, 1000000000);
                    if (err == -ETIMEDOUT) {
                        ALOGW("External animation signal timed out!");
                    }
                }
            }
        }
    }
}

void ExSurfaceFlinger::setDisplayAnimating(const sp<const DisplayDevice>& hw __unused) {
    static android::sp<vendor::display::config::V1_1::IDisplayConfig> disp_config_v1_1 =
                                        vendor::display::config::V1_1::IDisplayConfig::getService();

    int32_t dpy = hw->getDisplayType();
    if (disp_config_v1_1 == NULL || !mDisableExtAnimation ||
        ((dpy > DisplayDevice::DISPLAY_ID_INVALID &&
        dpy < DisplayDevice::NUM_BUILTIN_DISPLAY_TYPES) && mBuiltInBitmask.test(dpy))) {
        return;
    }

    bool hasScreenshot = false;
    mDrawingState.traverseInZOrder([&](Layer* layer) {
      if (layer->getLayerStack() == hw->getLayerStack()) {
          if (layer->isScreenshot()) {
              hasScreenshot = true;
          }
      }
    });

    if (hasScreenshot == mAnimating) {
        return;
    }

    disp_config_v1_1->setDisplayAnimating(dpy, hasScreenshot);
    mAnimating = hasScreenshot;
}


void ExSurfaceFlinger::handleMessageRefresh() {
    SurfaceFlinger::handleMessageRefresh();
    if (mDisableExtAnimation && mAnimating) {
        Mutex::Autolock lock(mExtAnimationLock);
        mExtAnimationCond.signal();
    }
}

}; // namespace android
