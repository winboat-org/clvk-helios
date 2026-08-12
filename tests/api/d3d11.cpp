// Copyright 2026 The clvk authors.
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

#include <CL/cl_d3d11.h>
#include <d3d11.h>
#include <d3d11_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

template <typename T> class com_holder {
public:
    com_holder() = default;
    ~com_holder() {
        if (m_object != nullptr) {
            m_object->Release();
        }
    }
    com_holder(const com_holder&) = delete;
    com_holder& operator=(const com_holder&) = delete;

    T** put() { return &m_object; }
    T* get() const { return m_object; }
    T* operator->() const { return m_object; }

private:
    T* m_object{nullptr};
};

struct callback_acquire_data {
    cl_command_queue queue;
    cl_mem memory;
    std::atomic<cl_int> result{CL_INVALID_OPERATION};
};

void CL_CALLBACK acquire_d3d11_from_callback(cl_event, cl_int status,
                                             void* user_data) {
    auto* data = static_cast<callback_acquire_data*>(user_data);
    if (status != CL_COMPLETE) {
        data->result.store(status);
        return;
    }
    data->result.store(clEnqueueAcquireD3D11ObjectsKHR(
        data->queue, 1, &data->memory, 0, nullptr, nullptr));
}

class D3D11Sharing : public ::testing::Test {
protected:
    void SetUp() override {
        D3D_FEATURE_LEVEL feature_level;
        ASSERT_TRUE(SUCCEEDED(
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                              nullptr, 0, D3D11_SDK_VERSION, m_d3d_device.put(),
                              &feature_level, m_d3d_context.put())));

        const cl_context_properties properties[] = {
            CL_CONTEXT_PLATFORM,
            reinterpret_cast<cl_context_properties>(gPlatform),
            CL_CONTEXT_D3D11_DEVICE_KHR,
            reinterpret_cast<cl_context_properties>(m_d3d_device.get()),
            0,
        };
        cl_int error;
        m_context =
            clCreateContext(properties, 1, &gDevice, nullptr, nullptr, &error);
        ASSERT_CL_SUCCESS(error);
        ASSERT_NE(m_context, nullptr);

        m_queue = clCreateCommandQueue(m_context, gDevice, 0, &error);
        ASSERT_CL_SUCCESS(error);
        ASSERT_NE(m_queue, nullptr);
    }

    void TearDown() override {
        if (m_queue != nullptr) {
            EXPECT_CL_SUCCESS(clReleaseCommandQueue(m_queue));
        }
        if (m_context != nullptr) {
            EXPECT_CL_SUCCESS(clReleaseContext(m_context));
        }
    }

    std::vector<uint8_t> read_buffer(ID3D11Buffer* buffer, size_t size) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(size);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        com_holder<ID3D11Buffer> staging;
        if (FAILED(m_d3d_device->CreateBuffer(&desc, nullptr, staging.put()))) {
            ADD_FAILURE() << "could not create D3D11 staging buffer";
            return {};
        }
        m_d3d_context->CopyResource(staging.get(), buffer);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_d3d_context->Map(staging.get(), 0, D3D11_MAP_READ, 0,
                                      &mapped))) {
            ADD_FAILURE() << "could not map D3D11 staging buffer";
            return {};
        }
        std::vector<uint8_t> output(size);
        if (mapped.pData != nullptr) {
            memcpy(output.data(), mapped.pData, size);
            m_d3d_context->Unmap(staging.get(), 0);
        }
        return output;
    }

    com_holder<ID3D11Device> m_d3d_device;
    com_holder<ID3D11DeviceContext> m_d3d_context;
    cl_context m_context{nullptr};
    cl_command_queue m_queue{nullptr};
};

TEST_F(D3D11Sharing, AssociationEntrypointsAndBufferRoundTrip) {
    size_t extension_size = 0;
    ASSERT_CL_SUCCESS(clGetDeviceInfo(gDevice, CL_DEVICE_EXTENSIONS, 0, nullptr,
                                      &extension_size));
    std::string extensions(extension_size, '\0');
    ASSERT_CL_SUCCESS(clGetDeviceInfo(gDevice, CL_DEVICE_EXTENSIONS,
                                      extensions.size(), extensions.data(),
                                      nullptr));
    EXPECT_NE(extensions.find(CL_KHR_D3D11_SHARING_EXTENSION_NAME),
              std::string::npos);

    EXPECT_NE(clGetExtensionFunctionAddressForPlatform(
                  gPlatform, "clGetDeviceIDsFromD3D11KHR"),
              nullptr);
    EXPECT_NE(clGetExtensionFunctionAddressForPlatform(
                  gPlatform, "clCreateFromD3D11BufferKHR"),
              nullptr);
    EXPECT_NE(clGetExtensionFunctionAddressForPlatform(
                  gPlatform, "clCreateFromD3D11Texture2DKHR"),
              nullptr);
    EXPECT_NE(clGetExtensionFunctionAddressForPlatform(
                  gPlatform, "clCreateFromD3D11Texture3DKHR"),
              nullptr);
    EXPECT_NE(clGetExtensionFunctionAddressForPlatform(
                  gPlatform, "clEnqueueAcquireD3D11ObjectsKHR"),
              nullptr);
    EXPECT_NE(clGetExtensionFunctionAddressForPlatform(
                  gPlatform, "clEnqueueReleaseD3D11ObjectsKHR"),
              nullptr);

    cl_device_id associated = nullptr;
    cl_uint num_devices = 0;
    ASSERT_CL_SUCCESS(clGetDeviceIDsFromD3D11KHR(
        gPlatform, CL_D3D11_DEVICE_KHR, m_d3d_device.get(),
        CL_ALL_DEVICES_FOR_D3D11_KHR, 1, &associated, &num_devices));
    EXPECT_EQ(associated, gDevice);
    EXPECT_GE(num_devices, 1u);

    EXPECT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 0, nullptr, 0,
                                                      nullptr, nullptr));
    cl_mem ignored = nullptr;
    EXPECT_EQ(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 0, &ignored, 0, nullptr,
                                              nullptr),
              CL_INVALID_VALUE);

    cl_bool prefer_shared = CL_TRUE;
    ASSERT_CL_SUCCESS(clGetContextInfo(
        m_context, CL_CONTEXT_D3D11_PREFER_SHARED_RESOURCES_KHR,
        sizeof(prefer_shared), &prefer_shared, nullptr));
    EXPECT_EQ(prefer_shared, CL_FALSE);

    std::vector<uint8_t> original(256);
    for (size_t i = 0; i < original.size(); i++) {
        original[i] = static_cast<uint8_t>(i ^ 0x5a);
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(original.size());
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{original.data(), 0, 0};
    com_holder<ID3D11Buffer> d3d_buffer;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateBuffer(&desc, &initial, d3d_buffer.put())));

    cl_int error;
    cl_mem memory = clCreateFromD3D11BufferKHR(m_context, CL_MEM_READ_WRITE,
                                               d3d_buffer.get(), &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(memory, nullptr);

    ID3D11Resource* queried_resource = nullptr;
    ASSERT_CL_SUCCESS(clGetMemObjectInfo(memory, CL_MEM_D3D11_RESOURCE_KHR,
                                         sizeof(queried_resource),
                                         &queried_resource, nullptr));
    EXPECT_EQ(queried_resource, static_cast<ID3D11Resource*>(d3d_buffer.get()));

    cl_mem duplicate = clCreateFromD3D11BufferKHR(m_context, CL_MEM_READ_WRITE,
                                                  d3d_buffer.get(), &error);
    EXPECT_EQ(duplicate, nullptr);
    EXPECT_EQ(error, CL_INVALID_D3D11_RESOURCE_KHR);

    EXPECT_EQ(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &memory, 0, nullptr,
                                              nullptr),
              CL_D3D11_RESOURCE_NOT_ACQUIRED_KHR);

    cl_event acquire_event = nullptr;
    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &memory, 0,
                                                      nullptr, &acquire_event));
    ASSERT_CL_SUCCESS(clWaitForEvents(1, &acquire_event));
    cl_command_type command_type = 0;
    ASSERT_CL_SUCCESS(clGetEventInfo(acquire_event, CL_EVENT_COMMAND_TYPE,
                                     sizeof(command_type), &command_type,
                                     nullptr));
    EXPECT_EQ(command_type, CL_COMMAND_ACQUIRE_D3D11_OBJECTS_KHR);
    EXPECT_CL_SUCCESS(clReleaseEvent(acquire_event));

    std::vector<uint8_t> opencl_data(original.size());
    ASSERT_CL_SUCCESS(
        clEnqueueReadBuffer(m_queue, memory, CL_TRUE, 0, opencl_data.size(),
                            opencl_data.data(), 0, nullptr, nullptr));
    EXPECT_EQ(opencl_data, original);

    EXPECT_EQ(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &memory, 0, nullptr,
                                              nullptr),
              CL_D3D11_RESOURCE_ALREADY_ACQUIRED_KHR);

    std::vector<uint8_t> replacement(original.size());
    for (size_t i = 0; i < replacement.size(); i++) {
        replacement[i] = static_cast<uint8_t>(255 - i);
    }
    ASSERT_CL_SUCCESS(
        clEnqueueWriteBuffer(m_queue, memory, CL_TRUE, 0, replacement.size(),
                             replacement.data(), 0, nullptr, nullptr));

    cl_event release_event = nullptr;
    ASSERT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &memory, 0,
                                                      nullptr, &release_event));
    ASSERT_CL_SUCCESS(clWaitForEvents(1, &release_event));
    ASSERT_CL_SUCCESS(clGetEventInfo(release_event, CL_EVENT_COMMAND_TYPE,
                                     sizeof(command_type), &command_type,
                                     nullptr));
    EXPECT_EQ(command_type, CL_COMMAND_RELEASE_D3D11_OBJECTS_KHR);
    EXPECT_CL_SUCCESS(clReleaseEvent(release_event));

    EXPECT_EQ(read_buffer(d3d_buffer.get(), replacement.size()), replacement);
    EXPECT_EQ(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &memory, 0, nullptr,
                                              nullptr),
              CL_D3D11_RESOURCE_NOT_ACQUIRED_KHR);
    EXPECT_CL_SUCCESS(clReleaseMemObject(memory));
}

TEST_F(D3D11Sharing, EnablesDeviceWideMultithreadProtection) {
    com_holder<ID3D11Multithread> multithread;
    ASSERT_TRUE(SUCCEEDED(m_d3d_context->QueryInterface(
        __uuidof(ID3D11Multithread),
        reinterpret_cast<void**>(multithread.put()))));
    EXPECT_TRUE(multithread->GetMultithreadProtected());
}

TEST_F(D3D11Sharing, RejectsSingleThreadedD3D11Device) {
    com_holder<ID3D11Device> single_threaded_device;
    com_holder<ID3D11DeviceContext> single_threaded_context;
    D3D_FEATURE_LEVEL feature_level;
    ASSERT_TRUE(SUCCEEDED(
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                          D3D11_CREATE_DEVICE_SINGLETHREADED, nullptr, 0,
                          D3D11_SDK_VERSION, single_threaded_device.put(),
                          &feature_level, single_threaded_context.put())));

    const cl_context_properties properties[] = {
        CL_CONTEXT_PLATFORM,
        reinterpret_cast<cl_context_properties>(gPlatform),
        CL_CONTEXT_D3D11_DEVICE_KHR,
        reinterpret_cast<cl_context_properties>(single_threaded_device.get()),
        0,
    };
    cl_int error;
    cl_context context =
        clCreateContext(properties, 1, &gDevice, nullptr, nullptr, &error);
    EXPECT_EQ(context, nullptr);
    EXPECT_EQ(error, CL_INVALID_D3D11_DEVICE_KHR);
}

TEST_F(D3D11Sharing, KernelAccessFlagsDoNotElideOwnershipCopies) {
    std::vector<uint8_t> original(256);
    for (size_t i = 0; i < original.size(); i++) {
        original[i] = static_cast<uint8_t>(i ^ 0x6d);
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(original.size());
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{original.data(), 0, 0};

    com_holder<ID3D11Buffer> write_only_buffer;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateBuffer(&desc, &initial, write_only_buffer.put())));
    cl_int error;
    cl_mem write_only = clCreateFromD3D11BufferKHR(
        m_context, CL_MEM_WRITE_ONLY, write_only_buffer.get(), &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(write_only, nullptr);

    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &write_only,
                                                      0, nullptr, nullptr));
    std::vector<uint8_t> acquired(original.size());
    ASSERT_CL_SUCCESS(clEnqueueReadBuffer(m_queue, write_only, CL_TRUE, 0,
                                          acquired.size(), acquired.data(), 0,
                                          nullptr, nullptr));
    EXPECT_EQ(acquired, original);

    std::vector<uint8_t> partial(original.size() / 2, 0xb4);
    ASSERT_CL_SUCCESS(clEnqueueWriteBuffer(m_queue, write_only, CL_TRUE, 0,
                                           partial.size(), partial.data(), 0,
                                           nullptr, nullptr));
    ASSERT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &write_only,
                                                      0, nullptr, nullptr));
    std::vector<uint8_t> expected = original;
    std::copy(partial.begin(), partial.end(), expected.begin());
    EXPECT_EQ(read_buffer(write_only_buffer.get(), expected.size()), expected);
    EXPECT_CL_SUCCESS(clReleaseMemObject(write_only));

    com_holder<ID3D11Buffer> read_only_buffer;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateBuffer(&desc, &initial, read_only_buffer.put())));
    cl_mem read_only = clCreateFromD3D11BufferKHR(
        m_context, CL_MEM_READ_ONLY, read_only_buffer.get(), &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(read_only, nullptr);

    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &read_only, 0,
                                                      nullptr, nullptr));
    std::vector<uint8_t> replacement(original.size(), 0x39);
    ASSERT_CL_SUCCESS(
        clEnqueueWriteBuffer(m_queue, read_only, CL_TRUE, 0, replacement.size(),
                             replacement.data(), 0, nullptr, nullptr));
    ASSERT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &read_only, 0,
                                                      nullptr, nullptr));
    EXPECT_EQ(read_buffer(read_only_buffer.get(), replacement.size()),
              replacement);
    EXPECT_CL_SUCCESS(clReleaseMemObject(read_only));
}

TEST_F(D3D11Sharing, FailedDependenciesRollBackOwnership) {
    std::vector<uint8_t> original(64, 0x52);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(original.size());
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{original.data(), 0, 0};
    com_holder<ID3D11Buffer> d3d_buffer;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateBuffer(&desc, &initial, d3d_buffer.put())));

    cl_int error;
    cl_mem memory = clCreateFromD3D11BufferKHR(m_context, CL_MEM_READ_WRITE,
                                               d3d_buffer.get(), &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(memory, nullptr);

    cl_event failed_dependency = clCreateUserEvent(m_context, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(failed_dependency, nullptr);
    ASSERT_CL_SUCCESS(
        clSetUserEventStatus(failed_dependency, CL_INVALID_OPERATION));
    cl_event acquire_event = nullptr;
    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(
        m_queue, 1, &memory, 1, &failed_dependency, &acquire_event));
    EXPECT_EQ(clWaitForEvents(1, &acquire_event),
              CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST);
    EXPECT_CL_SUCCESS(clReleaseEvent(acquire_event));
    EXPECT_CL_SUCCESS(clReleaseEvent(failed_dependency));

    cl_command_queue retry_queue =
        clCreateCommandQueue(m_context, gDevice, 0, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(retry_queue, nullptr);
    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(retry_queue, 1, &memory,
                                                      0, nullptr, nullptr));

    failed_dependency = clCreateUserEvent(m_context, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_CL_SUCCESS(
        clSetUserEventStatus(failed_dependency, CL_INVALID_OPERATION));
    EXPECT_EQ(clEnqueueReleaseD3D11ObjectsKHR(retry_queue, 1, &memory, 1,
                                              &failed_dependency, nullptr),
              CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST);
    EXPECT_CL_SUCCESS(clReleaseEvent(failed_dependency));

    cl_command_queue second_retry_queue =
        clCreateCommandQueue(m_context, gDevice, 0, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(second_retry_queue, nullptr);
    EXPECT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(
        second_retry_queue, 1, &memory, 0, nullptr, nullptr));

    EXPECT_CL_SUCCESS(clReleaseCommandQueue(second_retry_queue));
    EXPECT_CL_SUCCESS(clReleaseCommandQueue(retry_queue));
    EXPECT_CL_SUCCESS(clReleaseMemObject(memory));
}

TEST_F(D3D11Sharing, EventCallbackCanReenterOwnershipApisDuringRelease) {
    constexpr size_t size = 64;
    std::vector<uint8_t> initial(size, 0x6b);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(size);
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
    com_holder<ID3D11Buffer> first_buffer;
    com_holder<ID3D11Buffer> second_buffer;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateBuffer(&desc, &data, first_buffer.put())));
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateBuffer(&desc, &data, second_buffer.put())));

    cl_int error;
    cl_mem first = clCreateFromD3D11BufferKHR(
        m_context, CL_MEM_READ_WRITE, first_buffer.get(), &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(first, nullptr);
    cl_mem second = clCreateFromD3D11BufferKHR(
        m_context, CL_MEM_READ_WRITE, second_buffer.get(), &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(second, nullptr);

    cl_command_queue callback_queue =
        clCreateCommandQueue(m_context, gDevice, 0, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(callback_queue, nullptr);
    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &first, 0,
                                                      nullptr, nullptr));
    ASSERT_CL_SUCCESS(clFinish(m_queue));

    cl_event gate = clCreateUserEvent(m_context, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(gate, nullptr);
    std::vector<uint8_t> readback(size);
    cl_event read_event = nullptr;
    ASSERT_CL_SUCCESS(clEnqueueReadBuffer(m_queue, first, CL_FALSE, 0, size,
                                          readback.data(), 1, &gate,
                                          &read_event));
    callback_acquire_data callback_data{callback_queue, second};
    ASSERT_CL_SUCCESS(clSetEventCallback(read_event, CL_COMPLETE,
                                         acquire_d3d11_from_callback,
                                         &callback_data));

    std::atomic<bool> release_started{false};
    std::atomic<cl_int> gate_result{CL_INVALID_OPERATION};
    std::thread gate_thread([&] {
        while (!release_started.load()) {
            std::this_thread::yield();
        }
        // Give the release call time to reach its implicit finish. With the
        // old lock scope, the callback below then waited on the reservation
        // mutex while finish waited for the callback, deadlocking both.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        gate_result.store(clSetUserEventStatus(gate, CL_COMPLETE));
    });
    release_started.store(true);
    const cl_int release_result = clEnqueueReleaseD3D11ObjectsKHR(
        m_queue, 1, &first, 0, nullptr, nullptr);
    gate_thread.join();

    EXPECT_CL_SUCCESS(release_result);
    EXPECT_CL_SUCCESS(gate_result.load());
    EXPECT_CL_SUCCESS(callback_data.result.load());
    EXPECT_CL_SUCCESS(clFinish(callback_queue));
    if (callback_data.result.load() == CL_SUCCESS) {
        EXPECT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(
            callback_queue, 1, &second, 0, nullptr, nullptr));
    }

    EXPECT_CL_SUCCESS(clReleaseEvent(read_event));
    EXPECT_CL_SUCCESS(clReleaseEvent(gate));
    EXPECT_CL_SUCCESS(clReleaseCommandQueue(callback_queue));
    EXPECT_CL_SUCCESS(clReleaseMemObject(second));
    EXPECT_CL_SUCCESS(clReleaseMemObject(first));
}

TEST_F(D3D11Sharing, Texture2DRoundTrip) {
    constexpr UINT width = 7;
    constexpr UINT height = 5;
    constexpr size_t pixel_size = 4;
    std::vector<uint8_t> original(width * height * pixel_size);
    for (size_t i = 0; i < original.size(); i++) {
        original[i] = static_cast<uint8_t>(i + 11);
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{original.data(),
                                   static_cast<UINT>(width * pixel_size), 0};
    com_holder<ID3D11Texture2D> texture;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateTexture2D(&desc, &initial, texture.put())));

    cl_int error;
    cl_mem image = clCreateFromD3D11Texture2DKHR(m_context, CL_MEM_READ_WRITE,
                                                 texture.get(), 0, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(image, nullptr);

    size_t queried_width = 0;
    cl_uint queried_subresource = ~0u;
    ASSERT_CL_SUCCESS(clGetImageInfo(
        image, CL_IMAGE_WIDTH, sizeof(queried_width), &queried_width, nullptr));
    ASSERT_CL_SUCCESS(clGetImageInfo(image, CL_IMAGE_D3D11_SUBRESOURCE_KHR,
                                     sizeof(queried_subresource),
                                     &queried_subresource, nullptr));
    EXPECT_EQ(queried_width, width);
    EXPECT_EQ(queried_subresource, 0u);

    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &image, 0,
                                                      nullptr, nullptr));
    std::vector<uint8_t> opencl_data(original.size());
    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {width, height, 1};
    ASSERT_CL_SUCCESS(clEnqueueReadImage(
        m_queue, image, CL_TRUE, origin, region, width * pixel_size, 0,
        opencl_data.data(), 0, nullptr, nullptr));
    EXPECT_EQ(opencl_data, original);

    std::vector<uint8_t> replacement(original.size(), 0xa7);
    ASSERT_CL_SUCCESS(clEnqueueWriteImage(
        m_queue, image, CL_TRUE, origin, region, width * pixel_size, 0,
        replacement.data(), 0, nullptr, nullptr));
    ASSERT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &image, 0,
                                                      nullptr, nullptr));

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    com_holder<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateTexture2D(&staging_desc, nullptr, staging.put())));
    m_d3d_context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    for (UINT y = 0; y < height; y++) {
        EXPECT_EQ(
            memcmp(static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                   replacement.data() + y * width * pixel_size,
                   width * pixel_size),
            0);
    }
    m_d3d_context->Unmap(staging.get(), 0);
    EXPECT_CL_SUCCESS(clReleaseMemObject(image));
}

TEST_F(D3D11Sharing, Texture3DRoundTrip) {
    constexpr UINT width = 4;
    constexpr UINT height = 3;
    constexpr UINT depth = 2;
    constexpr size_t pixel_size = 4;
    const size_t row_pitch = width * pixel_size;
    const size_t slice_pitch = row_pitch * height;
    std::vector<uint8_t> original(slice_pitch * depth);
    for (size_t i = 0; i < original.size(); i++) {
        original[i] = static_cast<uint8_t>(i ^ 0xc3);
    }

    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Depth = depth;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{original.data(),
                                   static_cast<UINT>(row_pitch),
                                   static_cast<UINT>(slice_pitch)};
    com_holder<ID3D11Texture3D> texture;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateTexture3D(&desc, &initial, texture.put())));

    cl_int error;
    cl_mem image = clCreateFromD3D11Texture3DKHR(m_context, CL_MEM_READ_WRITE,
                                                 texture.get(), 0, &error);
    ASSERT_CL_SUCCESS(error);
    ASSERT_NE(image, nullptr);

    ASSERT_CL_SUCCESS(clEnqueueAcquireD3D11ObjectsKHR(m_queue, 1, &image, 0,
                                                      nullptr, nullptr));
    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {width, height, depth};
    std::vector<uint8_t> opencl_data(original.size());
    ASSERT_CL_SUCCESS(clEnqueueReadImage(
        m_queue, image, CL_TRUE, origin, region, row_pitch, slice_pitch,
        opencl_data.data(), 0, nullptr, nullptr));
    EXPECT_EQ(opencl_data, original);

    std::vector<uint8_t> replacement(original.size(), 0x39);
    ASSERT_CL_SUCCESS(clEnqueueWriteImage(
        m_queue, image, CL_TRUE, origin, region, row_pitch, slice_pitch,
        replacement.data(), 0, nullptr, nullptr));
    ASSERT_CL_SUCCESS(clEnqueueReleaseD3D11ObjectsKHR(m_queue, 1, &image, 0,
                                                      nullptr, nullptr));

    D3D11_TEXTURE3D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    com_holder<ID3D11Texture3D> staging;
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_device->CreateTexture3D(&staging_desc, nullptr, staging.put())));
    m_d3d_context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(
        m_d3d_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    for (UINT z = 0; z < depth; z++) {
        for (UINT y = 0; y < height; y++) {
            EXPECT_EQ(
                memcmp(static_cast<uint8_t*>(mapped.pData) +
                           z * mapped.DepthPitch + y * mapped.RowPitch,
                       replacement.data() + z * slice_pitch + y * row_pitch,
                       row_pitch),
                0);
        }
    }
    m_d3d_context->Unmap(staging.get(), 0);
    EXPECT_CL_SUCCESS(clReleaseMemObject(image));
}

} // namespace
