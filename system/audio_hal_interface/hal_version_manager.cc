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

#include "hal_version_manager.h"

#include <android/binder_manager.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <base/logging.h>
#include <hidl/ServiceManagement.h>

#include <memory>

#include "aidl/audio_aidl_interfaces.h"
#include "osi/include/log.h"
#include "osi/include/properties.h"

namespace bluetooth {
namespace audio {

using ::aidl::android::hardware::bluetooth::audio::
    IBluetoothAudioProviderFactory;

static const std::string kDefaultAudioProviderFactoryInterface =
    std::string() + IBluetoothAudioProviderFactory::descriptor + "/default";
static const std::string kSystemAudioProviderFactoryInterface =
    std::string() + IBluetoothAudioProviderFactory::descriptor + "/sysbta";
static inline const std::string& audioProviderFactoryInterface() {
  return osi_property_get_bool("persist.bluetooth.system_audio_hal.enabled", false)
    ? kSystemAudioProviderFactoryInterface : kDefaultAudioProviderFactoryInterface;
}

std::unique_ptr<HalVersionManager> HalVersionManager::instance_ptr =
    std::make_unique<HalVersionManager>();

BluetoothAudioHalVersion HalVersionManager::GetHalVersion() {
  std::lock_guard<std::mutex> guard(instance_ptr->mutex_);
  return instance_ptr->hal_version_;
}

BluetoothAudioHalTransport HalVersionManager::GetHalTransport() {
  return instance_ptr->hal_transport_;
}

android::sp<IBluetoothAudioProvidersFactory_2_1>
HalVersionManager::GetProvidersFactory_2_1() {
  std::lock_guard<std::mutex> guard(instance_ptr->mutex_);
  if (instance_ptr->hal_version_ != BluetoothAudioHalVersion::VERSION_2_1) {
    return nullptr;
  }
  android::sp<IBluetoothAudioProvidersFactory_2_1> providers_factory =
      IBluetoothAudioProvidersFactory_2_1::getService();
  CHECK(providers_factory)
      << "V2_1::IBluetoothAudioProvidersFactory::getService() failed";

  LOG(INFO) << "V2_1::IBluetoothAudioProvidersFactory::getService() returned "
            << providers_factory.get()
            << (providers_factory->isRemote() ? " (remote)" : " (local)");
  return providers_factory;
}

android::sp<IBluetoothAudioProvidersFactory_2_0>
HalVersionManager::GetProvidersFactory_2_0() {
  std::unique_lock<std::mutex> guard(instance_ptr->mutex_);
  if (instance_ptr->hal_version_ == BluetoothAudioHalVersion::VERSION_2_1) {
    guard.unlock();
    return instance_ptr->GetProvidersFactory_2_1();
  }
  android::sp<IBluetoothAudioProvidersFactory_2_0> providers_factory =
      IBluetoothAudioProvidersFactory_2_0::getService();
  CHECK(providers_factory)
      << "V2_0::IBluetoothAudioProvidersFactory::getService() failed";

  LOG(INFO) << "V2_0::IBluetoothAudioProvidersFactory::getService() returned "
            << providers_factory.get()
            << (providers_factory->isRemote() ? " (remote)" : " (local)");
  guard.unlock();
  return providers_factory;
}

BluetoothAudioHalVersion GetAidlInterfaceVersion() {
  int aidl_version = 0;

  auto provider_factory = IBluetoothAudioProviderFactory::fromBinder(
      ::ndk::SpAIBinder(AServiceManager_waitForService(
              audioProviderFactoryInterface().c_str())));

  if (provider_factory == nullptr) {
    LOG_ERROR("Can't get aidl version from unknown factory");
    return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
  }

  auto aidl_retval = provider_factory->getInterfaceVersion(&aidl_version);
  if (!aidl_retval.isOk()) {
    LOG_ERROR("BluetoothAudioHal::getInterfaceVersion failure: %s",
              aidl_retval.getDescription().c_str());
    return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
  }

  switch (aidl_version) {
    case 1:
      return BluetoothAudioHalVersion::VERSION_AIDL_V1;
    case 2:
      return BluetoothAudioHalVersion::VERSION_AIDL_V2;
    case 3:
      return BluetoothAudioHalVersion::VERSION_AIDL_V3;
    case 4:
      return BluetoothAudioHalVersion::VERSION_AIDL_V4;
    default:
      LOG_ERROR("Unknown AIDL version %d", aidl_version);
      return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
  }

  return BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
}

HalVersionManager::HalVersionManager() {
  hal_transport_ = BluetoothAudioHalTransport::UNKNOWN;
  if (AServiceManager_checkService(
          audioProviderFactoryInterface().c_str()) != nullptr) {
    hal_version_ = GetAidlInterfaceVersion();
    hal_transport_ = BluetoothAudioHalTransport::AIDL;
    return;
  }

  auto service_manager = android::hardware::defaultServiceManager1_2();
  CHECK(service_manager != nullptr);
  size_t instance_count = 0;
  auto listManifestByInterface_cb =
      [&instance_count](
          const hidl_vec<android::hardware::hidl_string>& instanceNames) {
        instance_count = instanceNames.size();
      };
  auto hidl_retval = service_manager->listManifestByInterface(
      kFullyQualifiedInterfaceName_2_1, listManifestByInterface_cb);
  if (!hidl_retval.isOk()) {
    LOG(FATAL) << __func__ << ": IServiceManager::listByInterface failure: "
               << hidl_retval.description();
    return;
  }

  if (instance_count > 0) {
    hal_version_ = BluetoothAudioHalVersion::VERSION_2_1;
    hal_transport_ = BluetoothAudioHalTransport::HIDL;
    return;
  }

  hidl_retval = service_manager->listManifestByInterface(
      kFullyQualifiedInterfaceName_2_0, listManifestByInterface_cb);
  if (!hidl_retval.isOk()) {
    LOG(FATAL) << __func__ << ": IServiceManager::listByInterface failure: "
               << hidl_retval.description();
    return;
  }

  if (instance_count > 0) {
    hal_version_ = BluetoothAudioHalVersion::VERSION_2_0;
    hal_transport_ = BluetoothAudioHalTransport::HIDL;
    return;
  }

  hal_version_ = BluetoothAudioHalVersion::VERSION_UNAVAILABLE;
  LOG(ERROR) << __func__ << " No supported HAL version";
}

}  // namespace audio
}  // namespace bluetooth
