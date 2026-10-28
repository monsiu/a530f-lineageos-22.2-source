#define LOG_TAG "SensorsSubHalLegacy"

#include <V2_1/SubHal.h>
#include <android-base/logging.h>
#include <convertV2_1.h>
#include <hardware/sensors.h>
#include <log/log.h>
#include <sensors/convert.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using ::android::sp;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::sensors::V1_0::OperationMode;
using ::android::hardware::sensors::V1_0::RateLevel;
using ::android::hardware::sensors::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorFlagBits;
using ::android::hardware::sensors::V1_0::SensorType;
using ::android::hardware::sensors::V1_0::SharedMemInfo;
using ::android::hardware::sensors::V2_1::implementation::IHalProxyCallback;
using ::android::hardware::sensors::V2_1::implementation::ISensorsSubHal;

namespace {

constexpr int kPollBufferSize = 64;
constexpr int kGetAllSensorsMode = 5555;

Result resultFromStatus(int err) {
    switch (err) {
        case 0:
            return Result::OK;
        case -EPERM:
        case -EACCES:
            return Result::PERMISSION_DENIED;
        case -ENOMEM:
            return Result::NO_MEMORY;
        case -EINVAL:
            return Result::BAD_VALUE;
        default:
            return Result::INVALID_OPERATION;
    }
}

class LegacySensorsSubHal : public ISensorsSubHal {
  public:
    LegacySensorsSubHal() { openDevice(); }

    Return<void> getSensorsList_2_1(
            ::android::hardware::sensors::V2_1::ISensors::getSensorsList_2_1_cb _hidl_cb) override {
        hidl_vec<::android::hardware::sensors::V2_1::SensorInfo> out;
        if (mModule == nullptr) {
            _hidl_cb(out);
            return Void();
        }

        sensor_t const* list = nullptr;
        size_t count = mModule->get_sensors_list(mModule, &list);
        out.resize(count);

        std::lock_guard<std::mutex> lock(mWakeLock);
        mWakeup.clear();

        for (size_t i = 0; i < count; ++i) {
            ::android::hardware::sensors::V1_0::SensorInfo info;
            ::android::hardware::sensors::V1_0::implementation::convertFromSensor(list[i], &info);

            if (info.requiredPermission == "com.samsung.permission.SSENSOR") {
                info.requiredPermission = "";
            }

            if (info.typeAsString == "com.samsung.sensor.physical_proximity") {
                info.type = SensorType::PROXIMITY;
                info.typeAsString = SENSOR_STRING_TYPE_PROXIMITY;
                info.maxRange = 1;
            }

            mWakeup[info.sensorHandle] =
                    (static_cast<uint32_t>(info.flags) &
                     static_cast<uint32_t>(SensorFlagBits::WAKE_UP)) != 0;
            out[i] = ::android::hardware::sensors::V2_1::implementation::convertToNewSensorInfo(info);
        }

        _hidl_cb(out);
        return Void();
    }

    Return<Result> setOperationMode(OperationMode mode) override {
        if (mModule == nullptr || mDevice == nullptr ||
            mDevice->common.version < SENSORS_DEVICE_API_VERSION_1_4 ||
            mModule->set_operation_mode == nullptr) {
            return Result::INVALID_OPERATION;
        }
        return resultFromStatus(mModule->set_operation_mode(static_cast<uint32_t>(mode)));
    }

    Return<Result> activate(int32_t sensorHandle, bool enabled) override {
        if (mDevice == nullptr) return Result::INVALID_OPERATION;
        return resultFromStatus(mDevice->activate(
                reinterpret_cast<sensors_poll_device_t*>(mDevice), sensorHandle, enabled));
    }

    Return<Result> batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                         int64_t maxReportLatencyNs) override {
        if (mDevice == nullptr) return Result::INVALID_OPERATION;
        return resultFromStatus(mDevice->batch(mDevice, sensorHandle, 0, samplingPeriodNs,
                                               maxReportLatencyNs));
    }

    Return<Result> flush(int32_t sensorHandle) override {
        if (mDevice == nullptr) return Result::INVALID_OPERATION;
        return resultFromStatus(mDevice->flush(mDevice, sensorHandle));
    }

    Return<Result> injectSensorData_2_1(
            const ::android::hardware::sensors::V2_1::Event& event) override {
        if (mDevice == nullptr || mDevice->common.version < SENSORS_DEVICE_API_VERSION_1_4 ||
            mDevice->inject_sensor_data == nullptr) {
            return Result::INVALID_OPERATION;
        }
        sensors_event_t out;
        ::android::hardware::sensors::V2_1::implementation::convertToSensorEvent(event, &out);
        return resultFromStatus(mDevice->inject_sensor_data(mDevice, &out));
    }

    Return<void> registerDirectChannel(
            const SharedMemInfo& /* mem */,
            ::android::hardware::sensors::V2_0::ISensors::registerDirectChannel_cb _hidl_cb)
            override {
        _hidl_cb(Result::INVALID_OPERATION, -1);
        return Void();
    }

    Return<Result> unregisterDirectChannel(int32_t /* channelHandle */) override {
        return Result::INVALID_OPERATION;
    }

    Return<void> configDirectReport(
            int32_t /* sensorHandle */, int32_t /* channelHandle */, RateLevel /* rate */,
            ::android::hardware::sensors::V2_0::ISensors::configDirectReport_cb _hidl_cb)
            override {
        _hidl_cb(Result::INVALID_OPERATION, 0);
        return Void();
    }

    Return<void> debug(const hidl_handle& /* fd */,
                       const hidl_vec<hidl_string>& /* args */) override {
        return Void();
    }

    const std::string getName() override { return "LegacySensorsSubHal"; }

    Return<Result> initialize(const sp<IHalProxyCallback>& halProxyCallback) override {
        if (mModule == nullptr || mDevice == nullptr) return Result::INVALID_OPERATION;

        {
            std::lock_guard<std::mutex> lock(mCallbackLock);
            mCallback = halProxyCallback;
        }

        sensor_t const* list = nullptr;
        size_t count = mModule->get_sensors_list(mModule, &list);
        for (size_t i = 0; i < count; ++i) {
            mDevice->activate(reinterpret_cast<sensors_poll_device_t*>(mDevice), list[i].handle,
                              0);
        }

        if (!mThreadStarted.exchange(true)) {
            std::thread(&LegacySensorsSubHal::pollLoop, this).detach();
        }
        return Result::OK;
    }

  private:
    void openDevice() {
        hw_module_t const* hwModule = nullptr;
        int err = hw_get_module(SENSORS_HARDWARE_MODULE_ID, &hwModule);
        if (err != 0 || hwModule == nullptr) {
            ALOGE("Could not load the legacy sensors module (%d)", err);
            return;
        }
        mModule = reinterpret_cast<sensors_module_t*>(const_cast<hw_module_t*>(hwModule));

        err = sensors_open_1(&mModule->common, &mDevice);
        if (err != 0 || mDevice == nullptr) {
            ALOGE("Could not open the legacy sensors device (%d)", err);
            mDevice = nullptr;
            return;
        }

        if (mDevice->common.version >= SENSORS_DEVICE_API_VERSION_1_4 &&
            mModule->set_operation_mode != nullptr) {
            mModule->set_operation_mode(kGetAllSensorsMode);
        }
    }

    bool isWakeup(int32_t handle) {
        std::lock_guard<std::mutex> lock(mWakeLock);
        auto it = mWakeup.find(handle);
        return it != mWakeup.end() && it->second;
    }

    void pollLoop() {
        sensors_event_t buffer[kPollBufferSize];
        while (true) {
            int n = mDevice->poll(reinterpret_cast<sensors_poll_device_t*>(mDevice), buffer,
                                  kPollBufferSize);
            if (n < 0) {
                ALOGE("poll failed (%d)", n);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            std::vector<::android::hardware::sensors::V2_1::Event> normal;
            std::vector<::android::hardware::sensors::V2_1::Event> wake;
            for (int i = 0; i < n; ++i) {
                if (buffer[i].type == SENSOR_TYPE_DYNAMIC_SENSOR_META) {
                    continue;
                }
                ::android::hardware::sensors::V2_1::Event event;
                ::android::hardware::sensors::V2_1::implementation::convertFromSensorEvent(buffer[i], &event);
                if (isWakeup(buffer[i].sensor)) {
                    wake.push_back(event);
                } else {
                    normal.push_back(event);
                }
            }

            sp<IHalProxyCallback> callback;
            {
                std::lock_guard<std::mutex> lock(mCallbackLock);
                callback = mCallback;
            }
            if (callback == nullptr) {
                continue;
            }
            if (!normal.empty()) {
                callback->postEvents(normal, callback->createScopedWakelock(false));
            }
            if (!wake.empty()) {
                callback->postEvents(wake, callback->createScopedWakelock(true));
            }
        }
    }

    sensors_module_t* mModule = nullptr;
    sensors_poll_device_1_t* mDevice = nullptr;

    std::mutex mCallbackLock;
    sp<IHalProxyCallback> mCallback;
    std::atomic<bool> mThreadStarted{false};

    std::mutex mWakeLock;
    std::unordered_map<int32_t, bool> mWakeup;
};

}  // namespace

extern "C" ISensorsSubHal* sensorsHalGetSubHal_2_1(uint32_t* version) {
    static LegacySensorsSubHal subHal;
    *version = SUB_HAL_2_1_VERSION;
    return &subHal;
}
