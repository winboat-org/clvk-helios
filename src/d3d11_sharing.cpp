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

#ifdef _WIN32

#include "d3d11_sharing.hpp"

#include <d3d11_4.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

#include "log.hpp"
#include "memory.hpp"
#include "queue.hpp"

namespace {

template <typename T> void release_com(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

IUnknown* com_identity(IUnknown* object) {
    if (object == nullptr) {
        return nullptr;
    }
    IUnknown* identity = nullptr;
    HRESULT result = object->QueryInterface(
        __uuidof(IUnknown), reinterpret_cast<void**>(&identity));
    return SUCCEEDED(result) ? identity : nullptr;
}

bool same_com_identity(IUnknown* first, IUnknown* second) {
    IUnknown* first_identity = com_identity(first);
    IUnknown* second_identity = com_identity(second);
    bool same = first_identity != nullptr && first_identity == second_identity;
    release_com(first_identity);
    release_com(second_identity);
    return same;
}

struct registered_resource {
    IUnknown* identity;
    UINT subresource;

    bool operator==(const registered_resource& other) const {
        return identity == other.identity && subresource == other.subresource;
    }
};

struct registered_resource_hash {
    size_t operator()(const registered_resource& resource) const {
        size_t pointer = reinterpret_cast<size_t>(resource.identity);
        return (pointer >> 4) ^
               (static_cast<size_t>(resource.subresource) << 1);
    }
};

std::mutex g_registered_resources_mutex;
std::unordered_set<registered_resource, registered_resource_hash>
    g_registered_resources;

bool register_resource(IUnknown* identity, UINT subresource) {
    std::lock_guard<std::mutex> lock(g_registered_resources_mutex);
    return g_registered_resources.insert({identity, subresource}).second;
}

void unregister_resource(IUnknown* identity, UINT subresource) {
    std::lock_guard<std::mutex> lock(g_registered_resources_mutex);
    g_registered_resources.erase({identity, subresource});
}

bool multiply_size(size_t first, size_t second, size_t* result) {
    if (first != 0 && second > std::numeric_limits<size_t>::max() / first) {
        return false;
    }
    *result = first * second;
    return true;
}

bool d3d11_format_to_opencl(DXGI_FORMAT format, cl_image_format* opencl,
                            size_t* element_size) {
#define CVK_D3D11_FORMAT(dxgi, order, type, size)                              \
    case dxgi:                                                                 \
        *opencl = {order, type};                                               \
        *element_size = size;                                                  \
        return true

    switch (format) {
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32G32B32A32_FLOAT, CL_RGBA, CL_FLOAT, 16);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32G32B32A32_UINT, CL_RGBA,
                         CL_UNSIGNED_INT32, 16);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32G32B32A32_SINT, CL_RGBA,
                         CL_SIGNED_INT32, 16);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16B16A16_FLOAT, CL_RGBA, CL_HALF_FLOAT,
                         8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16B16A16_UNORM, CL_RGBA,
                         CL_UNORM_INT16, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16B16A16_UINT, CL_RGBA,
                         CL_UNSIGNED_INT16, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16B16A16_SNORM, CL_RGBA,
                         CL_SNORM_INT16, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16B16A16_SINT, CL_RGBA,
                         CL_SIGNED_INT16, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_B8G8R8A8_UNORM, CL_BGRA, CL_UNORM_INT8, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8B8A8_UNORM, CL_RGBA, CL_UNORM_INT8, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8B8A8_UINT, CL_RGBA, CL_UNSIGNED_INT8,
                         4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8B8A8_SNORM, CL_RGBA, CL_SNORM_INT8, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8B8A8_SINT, CL_RGBA, CL_SIGNED_INT8, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32G32_FLOAT, CL_RG, CL_FLOAT, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32G32_UINT, CL_RG, CL_UNSIGNED_INT32, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32G32_SINT, CL_RG, CL_SIGNED_INT32, 8);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16_FLOAT, CL_RG, CL_HALF_FLOAT, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16_UNORM, CL_RG, CL_UNORM_INT16, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16_UINT, CL_RG, CL_UNSIGNED_INT16, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16_SNORM, CL_RG, CL_SNORM_INT16, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16G16_SINT, CL_RG, CL_SIGNED_INT16, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8_UNORM, CL_RG, CL_UNORM_INT8, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8_UINT, CL_RG, CL_UNSIGNED_INT8, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8_SNORM, CL_RG, CL_SNORM_INT8, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8G8_SINT, CL_RG, CL_SIGNED_INT8, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32_FLOAT, CL_R, CL_FLOAT, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32_UINT, CL_R, CL_UNSIGNED_INT32, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R32_SINT, CL_R, CL_SIGNED_INT32, 4);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16_FLOAT, CL_R, CL_HALF_FLOAT, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16_UNORM, CL_R, CL_UNORM_INT16, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16_UINT, CL_R, CL_UNSIGNED_INT16, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16_SNORM, CL_R, CL_SNORM_INT16, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R16_SINT, CL_R, CL_SIGNED_INT16, 2);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8_UNORM, CL_R, CL_UNORM_INT8, 1);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8_UINT, CL_R, CL_UNSIGNED_INT8, 1);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8_SNORM, CL_R, CL_SNORM_INT8, 1);
        CVK_D3D11_FORMAT(DXGI_FORMAT_R8_SINT, CL_R, CL_SIGNED_INT8, 1);
    default:
        return false;
    }

#undef CVK_D3D11_FORMAT
}

enum class resource_owner
{
    d3d11,
    opencl,
};

} // namespace

struct cvk_d3d11_interop::impl {
    ID3D11Device* device{nullptr};
    ID3D11DeviceContext* immediate_context{nullptr};
    ID3D11Multithread* multithread{nullptr};
};

cvk_d3d11_interop::cvk_d3d11_interop() : m_impl(std::make_unique<impl>()) {}

cvk_d3d11_interop::~cvk_d3d11_interop() {
    release_com(m_impl->multithread);
    release_com(m_impl->immediate_context);
    release_com(m_impl->device);
}

cl_int cvk_d3d11_interop::init(ID3D11Device* device) {
    if (device == nullptr) {
        return CL_INVALID_D3D11_DEVICE_KHR;
    }

    ID3D11Device* retained = nullptr;
    HRESULT result = device->QueryInterface(
        __uuidof(ID3D11Device), reinterpret_cast<void**>(&retained));
    if (FAILED(result) || retained == nullptr) {
        return CL_INVALID_D3D11_DEVICE_KHR;
    }
    if ((retained->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) !=
        0) {
        retained->Release();
        return CL_INVALID_D3D11_DEVICE_KHR;
    }

    ID3D11DeviceContext* immediate_context = nullptr;
    retained->GetImmediateContext(&immediate_context);
    if (immediate_context == nullptr) {
        retained->Release();
        return CL_INVALID_D3D11_DEVICE_KHR;
    }

    ID3D11Multithread* multithread = nullptr;
    result = immediate_context->QueryInterface(
        __uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread));
    if (FAILED(result) || multithread == nullptr) {
        immediate_context->Release();
        retained->Release();
        return CL_INVALID_D3D11_DEVICE_KHR;
    }
    multithread->SetMultithreadProtected(TRUE);

    m_impl->device = retained;
    m_impl->immediate_context = immediate_context;
    m_impl->multithread = multithread;
    return CL_SUCCESS;
}

ID3D11Device* cvk_d3d11_interop::device() const { return m_impl->device; }

ID3D11DeviceContext* cvk_d3d11_interop::immediate_context() const {
    return m_impl->immediate_context;
}

bool cvk_d3d11_interop::owns_resource(ID3D11Resource* resource) const {
    if (resource == nullptr || m_impl->device == nullptr) {
        return false;
    }
    ID3D11Device* resource_device = nullptr;
    resource->GetDevice(&resource_device);
    bool result = same_com_identity(resource_device, m_impl->device);
    release_com(resource_device);
    return result;
}

void cvk_d3d11_interop::lock() { m_impl->multithread->Enter(); }

void cvk_d3d11_interop::unlock() { m_impl->multithread->Leave(); }

struct cvk_d3d11_shared_resource::impl {
    cvk_d3d11_interop* interop{nullptr};
    cvk_d3d11_resource_kind kind{cvk_d3d11_resource_kind::buffer};
    ID3D11Resource* resource{nullptr};
    ID3D11Resource* staging{nullptr};
    IUnknown* identity{nullptr};
    UINT subresource{0};
    size_t width{0};
    size_t height{1};
    size_t depth{1};
    size_t element_size{1};
    size_t tight_row_pitch{0};
    size_t tight_slice_pitch{0};
    size_t tight_size{0};
    std::mutex ownership_mutex;
    resource_owner physical_owner{resource_owner::d3d11};
    resource_owner logical_owner{resource_owner::d3d11};
    size_t pending_transitions{0};
    bool failed_transition_chain{false};
};

cvk_d3d11_shared_resource::cvk_d3d11_shared_resource()
    : m_impl(std::make_unique<impl>()) {}

cvk_d3d11_shared_resource::~cvk_d3d11_shared_resource() {
    if (m_impl->identity != nullptr) {
        unregister_resource(m_impl->identity, m_impl->subresource);
    }
    release_com(m_impl->staging);
    release_com(m_impl->resource);
}

std::shared_ptr<cvk_d3d11_shared_resource> cvk_d3d11_shared_resource::finish(
    cvk_d3d11_interop* interop, ID3D11Resource* resource,
    ID3D11Resource* staging, IUnknown* identity, cvk_d3d11_resource_kind kind,
    UINT subresource, size_t width, size_t height, size_t depth,
    size_t element_size, cl_int* errcode_ret) {
    size_t row_pitch;
    size_t slice_pitch;
    size_t total_size;
    if (!multiply_size(width, element_size, &row_pitch) ||
        !multiply_size(row_pitch, height, &slice_pitch) ||
        !multiply_size(slice_pitch, depth, &total_size)) {
        *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        release_com(identity);
        release_com(staging);
        release_com(resource);
        return nullptr;
    }

    bool registered;
    try {
        registered = register_resource(identity, subresource);
    } catch (const std::bad_alloc&) {
        *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        release_com(identity);
        release_com(staging);
        release_com(resource);
        return nullptr;
    }
    if (!registered) {
        *errcode_ret = CL_INVALID_D3D11_RESOURCE_KHR;
        release_com(identity);
        release_com(staging);
        release_com(resource);
        return nullptr;
    }

    try {
        auto shared = std::shared_ptr<cvk_d3d11_shared_resource>(
            new cvk_d3d11_shared_resource());
        shared->m_impl->interop = interop;
        shared->m_impl->resource = resource;
        shared->m_impl->staging = staging;
        shared->m_impl->identity = identity;
        // The retained resource keeps its COM identity alive. Drop the
        // QueryInterface reference so the OpenCL object contributes exactly
        // one reference to the application resource, as required by the
        // extension.
        identity->Release();
        shared->m_impl->kind = kind;
        shared->m_impl->subresource = subresource;
        shared->m_impl->width = width;
        shared->m_impl->height = height;
        shared->m_impl->depth = depth;
        shared->m_impl->element_size = element_size;
        shared->m_impl->tight_row_pitch = row_pitch;
        shared->m_impl->tight_slice_pitch = slice_pitch;
        shared->m_impl->tight_size = total_size;
        *errcode_ret = CL_SUCCESS;
        return shared;
    } catch (const std::bad_alloc&) {
        unregister_resource(identity, subresource);
        *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        release_com(identity);
        release_com(staging);
        release_com(resource);
        return nullptr;
    }
}

namespace {

template <typename T>
T* query_resource_interface(IUnknown* resource, const IID& iid) {
    if (resource == nullptr) {
        return nullptr;
    }
    T* typed = nullptr;
    HRESULT result =
        resource->QueryInterface(iid, reinterpret_cast<void**>(&typed));
    return SUCCEEDED(result) ? typed : nullptr;
}

} // namespace

std::shared_ptr<cvk_d3d11_shared_resource>
cvk_d3d11_shared_resource::create_buffer(cvk_d3d11_interop* interop,
                                         ID3D11Buffer* resource,
                                         cl_int* errcode_ret) {
    *errcode_ret = CL_INVALID_D3D11_RESOURCE_KHR;
    if (interop == nullptr || resource == nullptr) {
        return nullptr;
    }

    ID3D11Buffer* retained = query_resource_interface<ID3D11Buffer>(
        resource, __uuidof(ID3D11Buffer));
    if (retained == nullptr || !interop->owns_resource(retained)) {
        release_com(retained);
        return nullptr;
    }

    D3D11_BUFFER_DESC desc{};
    retained->GetDesc(&desc);
    if (desc.Usage == D3D11_USAGE_IMMUTABLE || desc.ByteWidth == 0) {
        release_com(retained);
        return nullptr;
    }

    D3D11_BUFFER_DESC staging_desc{};
    staging_desc.ByteWidth = desc.ByteWidth;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

    ID3D11Buffer* staging = nullptr;
    HRESULT result =
        interop->device()->CreateBuffer(&staging_desc, nullptr, &staging);
    if (FAILED(result) || staging == nullptr) {
        *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        release_com(retained);
        return nullptr;
    }

    IUnknown* identity = com_identity(retained);
    if (identity == nullptr) {
        release_com(staging);
        release_com(retained);
        return nullptr;
    }

    return finish(interop, retained, staging, identity,
                  cvk_d3d11_resource_kind::buffer, 0, desc.ByteWidth, 1, 1, 1,
                  errcode_ret);
}

std::shared_ptr<cvk_d3d11_shared_resource>
cvk_d3d11_shared_resource::create_texture2d(cvk_d3d11_interop* interop,
                                            ID3D11Texture2D* resource,
                                            UINT subresource,
                                            cl_image_desc* image_desc,
                                            cl_image_format* image_format,
                                            cl_int* errcode_ret) {
    *errcode_ret = CL_INVALID_D3D11_RESOURCE_KHR;
    if (interop == nullptr || resource == nullptr || image_desc == nullptr ||
        image_format == nullptr) {
        return nullptr;
    }

    ID3D11Texture2D* retained = query_resource_interface<ID3D11Texture2D>(
        resource, __uuidof(ID3D11Texture2D));
    if (retained == nullptr || !interop->owns_resource(retained)) {
        release_com(retained);
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    retained->GetDesc(&desc);
    if (desc.Usage == D3D11_USAGE_IMMUTABLE || desc.MipLevels == 0 ||
        desc.ArraySize == 0 || desc.SampleDesc.Count != 1) {
        release_com(retained);
        return nullptr;
    }
    const uint64_t subresource_count =
        static_cast<uint64_t>(desc.MipLevels) * desc.ArraySize;
    if (subresource >= subresource_count) {
        *errcode_ret = CL_INVALID_VALUE;
        release_com(retained);
        return nullptr;
    }

    size_t element_size;
    if (!d3d11_format_to_opencl(desc.Format, image_format, &element_size)) {
        *errcode_ret = CL_INVALID_IMAGE_FORMAT_DESCRIPTOR;
        release_com(retained);
        return nullptr;
    }

    UINT mip_level = subresource % desc.MipLevels;
    UINT width = std::max(1u, desc.Width >> mip_level);
    UINT height = std::max(1u, desc.Height >> mip_level);

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = desc.Format;
    staging_desc.SampleDesc = {1, 0};
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D* staging = nullptr;
    HRESULT result =
        interop->device()->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(result) || staging == nullptr) {
        *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        release_com(retained);
        return nullptr;
    }

    IUnknown* identity = com_identity(retained);
    if (identity == nullptr) {
        release_com(staging);
        release_com(retained);
        return nullptr;
    }

    *image_desc = {};
    image_desc->image_type = CL_MEM_OBJECT_IMAGE2D;
    image_desc->image_width = width;
    image_desc->image_height = height;
    image_desc->image_depth = 0;

    return finish(interop, retained, staging, identity,
                  cvk_d3d11_resource_kind::texture2d, subresource, width,
                  height, 1, element_size, errcode_ret);
}

std::shared_ptr<cvk_d3d11_shared_resource>
cvk_d3d11_shared_resource::create_texture3d(cvk_d3d11_interop* interop,
                                            ID3D11Texture3D* resource,
                                            UINT subresource,
                                            cl_image_desc* image_desc,
                                            cl_image_format* image_format,
                                            cl_int* errcode_ret) {
    *errcode_ret = CL_INVALID_D3D11_RESOURCE_KHR;
    if (interop == nullptr || resource == nullptr || image_desc == nullptr ||
        image_format == nullptr) {
        return nullptr;
    }

    ID3D11Texture3D* retained = query_resource_interface<ID3D11Texture3D>(
        resource, __uuidof(ID3D11Texture3D));
    if (retained == nullptr || !interop->owns_resource(retained)) {
        release_com(retained);
        return nullptr;
    }

    D3D11_TEXTURE3D_DESC desc{};
    retained->GetDesc(&desc);
    if (desc.Usage == D3D11_USAGE_IMMUTABLE || desc.MipLevels == 0) {
        release_com(retained);
        return nullptr;
    }
    if (subresource >= desc.MipLevels) {
        *errcode_ret = CL_INVALID_VALUE;
        release_com(retained);
        return nullptr;
    }

    size_t element_size;
    if (!d3d11_format_to_opencl(desc.Format, image_format, &element_size)) {
        *errcode_ret = CL_INVALID_IMAGE_FORMAT_DESCRIPTOR;
        release_com(retained);
        return nullptr;
    }

    UINT width = std::max(1u, desc.Width >> subresource);
    UINT height = std::max(1u, desc.Height >> subresource);
    UINT depth = std::max(1u, desc.Depth >> subresource);

    D3D11_TEXTURE3D_DESC staging_desc{};
    staging_desc.Width = width;
    staging_desc.Height = height;
    staging_desc.Depth = depth;
    staging_desc.MipLevels = 1;
    staging_desc.Format = desc.Format;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture3D* staging = nullptr;
    HRESULT result =
        interop->device()->CreateTexture3D(&staging_desc, nullptr, &staging);
    if (FAILED(result) || staging == nullptr) {
        *errcode_ret = CL_OUT_OF_HOST_MEMORY;
        release_com(retained);
        return nullptr;
    }

    IUnknown* identity = com_identity(retained);
    if (identity == nullptr) {
        release_com(staging);
        release_com(retained);
        return nullptr;
    }

    *image_desc = {};
    image_desc->image_type = CL_MEM_OBJECT_IMAGE3D;
    image_desc->image_width = width;
    image_desc->image_height = height;
    image_desc->image_depth = depth;

    return finish(interop, retained, staging, identity,
                  cvk_d3d11_resource_kind::texture3d, subresource, width,
                  height, depth, element_size, errcode_ret);
}

ID3D11Resource* cvk_d3d11_shared_resource::resource() const {
    return m_impl->resource;
}

UINT cvk_d3d11_shared_resource::subresource() const {
    return m_impl->subresource;
}

size_t cvk_d3d11_shared_resource::size() const { return m_impl->tight_size; }

bool cvk_d3d11_shared_resource::begin_acquire() {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    if (m_impl->failed_transition_chain ||
        m_impl->logical_owner != resource_owner::d3d11) {
        return false;
    }
    m_impl->logical_owner = resource_owner::opencl;
    m_impl->pending_transitions++;
    return true;
}

bool cvk_d3d11_shared_resource::begin_release() {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    if (m_impl->failed_transition_chain ||
        m_impl->logical_owner != resource_owner::opencl) {
        return false;
    }
    m_impl->logical_owner = resource_owner::d3d11;
    m_impl->pending_transitions++;
    return true;
}

void cvk_d3d11_shared_resource::cancel_acquire() {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    CVK_ASSERT(m_impl->pending_transitions > 0);
    m_impl->pending_transitions--;
    m_impl->logical_owner = resource_owner::d3d11;
    if (m_impl->pending_transitions == 0) {
        m_impl->logical_owner = m_impl->physical_owner;
        m_impl->failed_transition_chain = false;
    }
}

void cvk_d3d11_shared_resource::cancel_release() {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    CVK_ASSERT(m_impl->pending_transitions > 0);
    m_impl->pending_transitions--;
    m_impl->logical_owner = resource_owner::opencl;
    if (m_impl->pending_transitions == 0) {
        m_impl->logical_owner = m_impl->physical_owner;
        m_impl->failed_transition_chain = false;
    }
}

void cvk_d3d11_shared_resource::finish_acquire(bool success) {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    CVK_ASSERT(m_impl->pending_transitions > 0);
    if (success) {
        m_impl->physical_owner = resource_owner::opencl;
    } else if (m_impl->pending_transitions > 1) {
        m_impl->failed_transition_chain = true;
    }
    m_impl->pending_transitions--;
    if (m_impl->pending_transitions == 0) {
        m_impl->logical_owner = m_impl->physical_owner;
        m_impl->failed_transition_chain = false;
    }
}

void cvk_d3d11_shared_resource::finish_release(bool success) {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    CVK_ASSERT(m_impl->pending_transitions > 0);
    if (success) {
        m_impl->physical_owner = resource_owner::d3d11;
    } else if (m_impl->pending_transitions > 1) {
        m_impl->failed_transition_chain = true;
    }
    m_impl->pending_transitions--;
    if (m_impl->pending_transitions == 0) {
        m_impl->logical_owner = m_impl->physical_owner;
        m_impl->failed_transition_chain = false;
    }
}

bool cvk_d3d11_shared_resource::is_acquired() const {
    std::lock_guard<std::mutex> lock(m_impl->ownership_mutex);
    return m_impl->logical_owner == resource_owner::opencl;
}

cl_int cvk_d3d11_shared_resource::copy_to_opencl(cvk_command_queue* queue,
                                                 cvk_mem* mem) {
    std::vector<uint8_t> data;
    try {
        data.resize(m_impl->tight_size);
    } catch (const std::bad_alloc&) {
        return CL_OUT_OF_HOST_MEMORY;
    }

    {
        std::lock_guard<cvk_d3d11_interop> lock(*m_impl->interop);
        auto* context = m_impl->interop->immediate_context();
        context->CopySubresourceRegion(m_impl->staging, 0, 0, 0, 0,
                                       m_impl->resource, m_impl->subresource,
                                       nullptr);
        context->Flush();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT result =
            context->Map(m_impl->staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(result) || mapped.pData == nullptr) {
            return CL_OUT_OF_RESOURCES;
        }

        if (m_impl->kind == cvk_d3d11_resource_kind::buffer) {
            memcpy(data.data(), mapped.pData, m_impl->tight_size);
        } else {
            auto* source = static_cast<const uint8_t*>(mapped.pData);
            for (size_t z = 0; z < m_impl->depth; z++) {
                for (size_t y = 0; y < m_impl->height; y++) {
                    memcpy(data.data() + z * m_impl->tight_slice_pitch +
                               y * m_impl->tight_row_pitch,
                           source + z * mapped.DepthPitch + y * mapped.RowPitch,
                           m_impl->tight_row_pitch);
                }
            }
        }
        context->Unmap(m_impl->staging, 0);
    }

    if (m_impl->kind == cvk_d3d11_resource_kind::buffer) {
        return clEnqueueWriteBuffer(queue, mem, CL_TRUE, 0, data.size(),
                                    data.data(), 0, nullptr, nullptr);
    }

    const size_t origin[3] = {0, 0, 0};
    const size_t region[3] = {m_impl->width, m_impl->height, m_impl->depth};
    const size_t slice_pitch =
        m_impl->kind == cvk_d3d11_resource_kind::texture3d
            ? m_impl->tight_slice_pitch
            : 0;
    return clEnqueueWriteImage(queue, mem, CL_TRUE, origin, region,
                               m_impl->tight_row_pitch, slice_pitch,
                               data.data(), 0, nullptr, nullptr);
}

cl_int cvk_d3d11_shared_resource::copy_from_opencl(cvk_command_queue* queue,
                                                   cvk_mem* mem) {
    std::vector<uint8_t> data;
    try {
        data.resize(m_impl->tight_size);
    } catch (const std::bad_alloc&) {
        return CL_OUT_OF_HOST_MEMORY;
    }

    cl_int result;
    if (m_impl->kind == cvk_d3d11_resource_kind::buffer) {
        result = clEnqueueReadBuffer(queue, mem, CL_TRUE, 0, data.size(),
                                     data.data(), 0, nullptr, nullptr);
    } else {
        const size_t origin[3] = {0, 0, 0};
        const size_t region[3] = {m_impl->width, m_impl->height, m_impl->depth};
        const size_t slice_pitch =
            m_impl->kind == cvk_d3d11_resource_kind::texture3d
                ? m_impl->tight_slice_pitch
                : 0;
        result = clEnqueueReadImage(queue, mem, CL_TRUE, origin, region,
                                    m_impl->tight_row_pitch, slice_pitch,
                                    data.data(), 0, nullptr, nullptr);
    }
    if (result != CL_SUCCESS) {
        return result;
    }

    std::lock_guard<cvk_d3d11_interop> lock(*m_impl->interop);
    auto* context = m_impl->interop->immediate_context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT map_result =
        context->Map(m_impl->staging, 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(map_result) || mapped.pData == nullptr) {
        return CL_OUT_OF_RESOURCES;
    }

    if (m_impl->kind == cvk_d3d11_resource_kind::buffer) {
        memcpy(mapped.pData, data.data(), m_impl->tight_size);
    } else {
        auto* destination = static_cast<uint8_t*>(mapped.pData);
        for (size_t z = 0; z < m_impl->depth; z++) {
            for (size_t y = 0; y < m_impl->height; y++) {
                memcpy(destination + z * mapped.DepthPitch +
                           y * mapped.RowPitch,
                       data.data() + z * m_impl->tight_slice_pitch +
                           y * m_impl->tight_row_pitch,
                       m_impl->tight_row_pitch);
            }
        }
    }
    context->Unmap(m_impl->staging, 0);
    context->CopySubresourceRegion(m_impl->resource, m_impl->subresource, 0, 0,
                                   0, m_impl->staging, 0, nullptr);
    context->Flush();
    return CL_SUCCESS;
}

cl_int cvk_command_d3d11_objects::do_action() {
    // The D3D copy is executed as an ordinary, non-batchable queue command.
    // Use a short-lived queue for the host<->Vulkan leg so calling a blocking
    // OpenCL transfer here cannot recursively enqueue onto this command's own
    // executor.
    cvk_command_queue* transfer_queue;
    try {
        std::vector<cl_queue_properties> properties;
        transfer_queue = new cvk_command_queue(
            m_queue->context(), m_queue->device(), 0, std::move(properties));
    } catch (const std::bad_alloc&) {
        return CL_OUT_OF_HOST_MEMORY;
    }
    cl_int result = transfer_queue->init();
    if (result != CL_SUCCESS) {
        transfer_queue->release();
        return result;
    }

    try {
        for (auto& object : m_objects) {
            auto& shared = object->d3d11_shared();
            result = m_acquire
                         ? shared->copy_to_opencl(transfer_queue, object)
                         : shared->copy_from_opencl(transfer_queue, object);
            if (result != CL_SUCCESS) {
                break;
            }
        }
    } catch (const std::bad_alloc&) {
        result = CL_OUT_OF_HOST_MEMORY;
    }

    transfer_queue->release();
    return result == CL_SUCCESS ? CL_COMPLETE : result;
}

bool cvk_get_d3d11_adapter_desc(cl_d3d11_device_source_khr source,
                                void* d3d_object, DXGI_ADAPTER_DESC* desc) {
    if (d3d_object == nullptr || desc == nullptr) {
        return false;
    }

    if (source == CL_D3D11_DXGI_ADAPTER_KHR) {
        IDXGIAdapter* adapter = nullptr;
        auto* object = static_cast<IDXGIAdapter*>(d3d_object);
        HRESULT result = object->QueryInterface(
            __uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
        if (FAILED(result) || adapter == nullptr) {
            return false;
        }
        result = adapter->GetDesc(desc);
        adapter->Release();
        return SUCCEEDED(result);
    }

    if (source != CL_D3D11_DEVICE_KHR) {
        return false;
    }

    ID3D11Device* device = nullptr;
    auto* object = static_cast<ID3D11Device*>(d3d_object);
    HRESULT result = object->QueryInterface(__uuidof(ID3D11Device),
                                            reinterpret_cast<void**>(&device));
    if (FAILED(result) || device == nullptr) {
        return false;
    }

    IDXGIDevice* dxgi_device = nullptr;
    result = device->QueryInterface(__uuidof(IDXGIDevice),
                                    reinterpret_cast<void**>(&dxgi_device));
    device->Release();
    if (FAILED(result) || dxgi_device == nullptr) {
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    result = dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    if (FAILED(result) || adapter == nullptr) {
        return false;
    }

    result = adapter->GetDesc(desc);
    adapter->Release();
    return SUCCEEDED(result);
}

#endif // _WIN32
