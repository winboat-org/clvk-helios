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

#pragma once

#ifdef _WIN32

#include <cstddef>
#include <dxgi.h>
#include <memory>

#include "cl_headers.hpp"

struct cvk_command_queue;
struct cvk_mem;

// D3D11 and clvk do not necessarily use the same Vulkan device. The interop
// implementation therefore uses a copy-backed data store at acquire/release
// boundaries. This is also the required fallback for ordinary D3D11 resources
// which were not created with D3D11_RESOURCE_MISC_SHARED.
class cvk_d3d11_interop {
public:
    cvk_d3d11_interop();
    ~cvk_d3d11_interop();

    cvk_d3d11_interop(const cvk_d3d11_interop&) = delete;
    cvk_d3d11_interop& operator=(const cvk_d3d11_interop&) = delete;

    cl_int init(ID3D11Device* device);
    ID3D11Device* device() const;
    ID3D11DeviceContext* immediate_context() const;
    bool owns_resource(ID3D11Resource* resource) const;

    // ID3D11DeviceContext is not thread-safe. These methods enter the D3D
    // runtime's device-wide critical section, which also serializes calls made
    // by the application and by other OpenCL contexts using the same device.
    void lock();
    void unlock();

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

enum class cvk_d3d11_resource_kind
{
    buffer,
    texture2d,
    texture3d,
};

// Metadata and the D3D11 staging resource associated with one cl_mem. The
// object retains the original D3D11 resource for the cl_mem lifetime and owns
// the duplicate-registration reservation required by the extension.
class cvk_d3d11_shared_resource {
public:
    static std::shared_ptr<cvk_d3d11_shared_resource>
    create_buffer(cvk_d3d11_interop* interop, ID3D11Buffer* resource,
                  cl_int* errcode_ret);
    static std::shared_ptr<cvk_d3d11_shared_resource>
    create_texture2d(cvk_d3d11_interop* interop, ID3D11Texture2D* resource,
                     UINT subresource, cl_image_desc* image_desc,
                     cl_image_format* image_format, cl_int* errcode_ret);
    static std::shared_ptr<cvk_d3d11_shared_resource>
    create_texture3d(cvk_d3d11_interop* interop, ID3D11Texture3D* resource,
                     UINT subresource, cl_image_desc* image_desc,
                     cl_image_format* image_format, cl_int* errcode_ret);

    ~cvk_d3d11_shared_resource();

    cvk_d3d11_shared_resource(const cvk_d3d11_shared_resource&) = delete;
    cvk_d3d11_shared_resource&
    operator=(const cvk_d3d11_shared_resource&) = delete;

    ID3D11Resource* resource() const;
    UINT subresource() const;
    size_t size() const;

    bool begin_acquire();
    bool begin_release();
    void finish_acquire(bool success);
    void finish_release(bool success);
    bool is_acquired() const;

    cl_int copy_to_opencl(cvk_command_queue* queue, cvk_mem* mem);
    cl_int copy_from_opencl(cvk_command_queue* queue, cvk_mem* mem);

private:
    cvk_d3d11_shared_resource();

    static std::shared_ptr<cvk_d3d11_shared_resource>
    finish(cvk_d3d11_interop* interop, ID3D11Resource* resource,
           ID3D11Resource* staging, IUnknown* identity,
           cvk_d3d11_resource_kind kind, UINT subresource, size_t width,
           size_t height, size_t depth, size_t element_size,
           cl_int* errcode_ret);

    struct impl;
    std::unique_ptr<impl> m_impl;
};

// Resolves either an ID3D11Device or an IDXGIAdapter to its adapter
// descriptor. Used by clGetDeviceIDsFromD3D11KHR for LUID association.
bool cvk_get_d3d11_adapter_desc(cl_d3d11_device_source_khr source,
                                void* d3d_object, DXGI_ADAPTER_DESC* desc);

#endif // _WIN32
