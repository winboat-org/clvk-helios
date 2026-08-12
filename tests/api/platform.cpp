// Copyright 2022 The clvk authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "testcl.hpp"

#include <unordered_set>

#if defined(_WIN32)
#include <CL/cl_d3d10.h>
#endif

TEST(Platform, DeviceQueryWithMultipleTypes) {
    cl_int err;

    // Get the type of the already selected device
    cl_device_type dtype;
    err = clGetDeviceInfo(gDevice, CL_DEVICE_TYPE, sizeof(dtype), &dtype,
                          nullptr);
    ASSERT_EQ(err, CL_SUCCESS);

    // Check its type is one of the expected values
    ASSERT_TRUE(dtype == CL_DEVICE_TYPE_GPU || dtype == CL_DEVICE_TYPE_CPU ||
                dtype == CL_DEVICE_TYPE_ACCELERATOR);

    std::unordered_set<cl_device_type> other_types = {
        CL_DEVICE_TYPE_DEFAULT, CL_DEVICE_TYPE_CPU, CL_DEVICE_TYPE_GPU,
        CL_DEVICE_TYPE_ACCELERATOR, CL_DEVICE_TYPE_CUSTOM};

    other_types.erase(dtype);

    // Check that the device can still be enumerated when the requested device
    // type specifies other device types as well
    for (auto otype : other_types) {
        cl_device_id device;
        err = clGetDeviceIDs(gPlatform, dtype | otype, 1, &device, nullptr);
        ASSERT_EQ(err, CL_SUCCESS);
    }
}

TEST(Platform, InteropUserSyncContextProperty) {
    cl_int err;
    const cl_context_properties valid_properties[] = {
        CL_CONTEXT_INTEROP_USER_SYNC, CL_FALSE, 0};
    auto context =
        clCreateContext(valid_properties, 1, &gDevice, nullptr, nullptr, &err);
    ASSERT_CL_SUCCESS(err);
    ASSERT_NE(context, nullptr);
    ASSERT_CL_SUCCESS(clReleaseContext(context));

    const cl_context_properties invalid_properties[] = {
        CL_CONTEXT_INTEROP_USER_SYNC, 2, 0};
    context = clCreateContext(invalid_properties, 1, &gDevice, nullptr, nullptr,
                              &err);
    EXPECT_EQ(context, nullptr);
    ASSERT_EQ(err, CL_INVALID_PROPERTY);
}

#if defined(_WIN32)
TEST(Platform, D3D10DeviceAssociationExtension) {
    size_t extension_size = 0;
    ASSERT_EQ(clGetPlatformInfo(gPlatform, CL_PLATFORM_EXTENSIONS, 0, nullptr,
                                &extension_size),
              CL_SUCCESS);

    std::string extensions(extension_size, '\0');
    ASSERT_EQ(clGetPlatformInfo(gPlatform, CL_PLATFORM_EXTENSIONS,
                                extensions.size(), extensions.data(), nullptr),
              CL_SUCCESS);
    ASSERT_NE(extensions.find(CL_KHR_D3D10_SHARING_EXTENSION_NAME),
              std::string::npos);

    auto get_devices = reinterpret_cast<clGetDeviceIDsFromD3D10KHR_fn>(
        clGetExtensionFunctionAddressForPlatform(gPlatform,
                                                 "clGetDeviceIDsFromD3D10KHR"));
    ASSERT_NE(get_devices, nullptr);

    cl_device_id device = nullptr;
    cl_uint num_devices = 0;
    EXPECT_EQ(get_devices(gPlatform, 0, nullptr,
                          CL_PREFERRED_DEVICES_FOR_D3D10_KHR, 1, &device,
                          nullptr),
              CL_INVALID_VALUE);
    EXPECT_EQ(get_devices(gPlatform, CL_D3D10_DXGI_ADAPTER_KHR, nullptr, 0, 1,
                          &device, nullptr),
              CL_INVALID_VALUE);
    EXPECT_EQ(get_devices(gPlatform, CL_D3D10_DXGI_ADAPTER_KHR, nullptr,
                          CL_PREFERRED_DEVICES_FOR_D3D10_KHR, 0, &device,
                          nullptr),
              CL_INVALID_VALUE);
    EXPECT_EQ(get_devices(gPlatform, CL_D3D10_DXGI_ADAPTER_KHR, nullptr,
                          CL_PREFERRED_DEVICES_FOR_D3D10_KHR, 0, nullptr,
                          nullptr),
              CL_INVALID_VALUE);
    EXPECT_EQ(get_devices(gPlatform, CL_D3D10_DXGI_ADAPTER_KHR, nullptr,
                          CL_PREFERRED_DEVICES_FOR_D3D10_KHR, 0, nullptr,
                          &num_devices),
              CL_INVALID_D3D10_DEVICE_KHR);
}
#endif
