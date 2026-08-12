// Copyright 2018-2023 The clvk authors.
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

#include <cstring>

#include "device.hpp"
#ifdef _WIN32
#include "d3d11_sharing.hpp"
#include "gl_sharing.hpp"
#endif
#include "objects.hpp"
#include "unit.hpp"

using cvk_printf_callback_t = void(CL_CALLBACK*)(const char* buffer, size_t len,
                                                 size_t complete,
                                                 void* user_data);

using cvk_context_callback_pointer_type = void(CL_CALLBACK*)(cl_context context,
                                                             void* user_data);
struct cvk_context_callback {
    cvk_context_callback_pointer_type pointer;
    void* data;
};

struct cvk_command_queue;

struct cvk_context : public _cl_context,
                     refcounted,
                     object_magic_header<object_magic::context> {

    cvk_context(cvk_device* device, const cl_context_properties* props,
                void* user_data)
        : m_device(device), m_printf_buffersize(0), m_printf_callback(nullptr),
          m_user_data(user_data), m_interop_user_sync(false) {

        if (props) {
            while (*props) {
                // Save name
                m_properties.push_back(*props);
                // Save value
                m_properties.push_back(*(props + 1));
                props += 2;
            }
            m_properties.push_back(*props);
        }
    }

    virtual ~cvk_context() {
        for (auto cbi = m_destuctor_callbacks.rbegin();
             cbi != m_destuctor_callbacks.rend(); ++cbi) {
            auto cb = *cbi;
            cb.pointer(this, cb.data);
        }
        free_image_init_command_queue();
    }

    cl_int init() {
        std::unordered_set<cl_context_properties> seen;
#ifdef _WIN32
        HGLRC gl_context = nullptr;
        HDC gl_device_context = nullptr;
        ID3D11Device* d3d11_device = nullptr;
        bool has_d3d11_device = false;
#endif
        for (unsigned i = 0; i < m_properties.size(); i += 2) {
            auto property = m_properties[i];
            if (seen.count(property) > 0) {
                return CL_INVALID_PROPERTY;
            }
            seen.insert(property);
            switch (property) {
            case CL_CONTEXT_PLATFORM: {
                cl_platform_id platform = (cl_platform_id)m_properties[i + 1];
                if (platform == nullptr ||
                    !icd_downcast(platform)->is_valid()) {
                    return CL_INVALID_PLATFORM;
                }
            } break;
            case CL_PRINTF_BUFFERSIZE_ARM:
                if (!config.printf_buffer_size.set) {
                    m_printf_buffersize = m_properties[i + 1];
                }
                break;
            case CL_PRINTF_CALLBACK_ARM:
                m_printf_callback = (cvk_printf_callback_t)m_properties[i + 1];
                break;
            case CL_CONTEXT_INTEROP_USER_SYNC:
                if (m_properties[i + 1] != CL_FALSE &&
                    m_properties[i + 1] != CL_TRUE) {
                    return CL_INVALID_PROPERTY;
                }
                m_interop_user_sync = m_properties[i + 1] == CL_TRUE;
                break;
#ifdef _WIN32
            case CL_GL_CONTEXT_KHR:
                gl_context = reinterpret_cast<HGLRC>(m_properties[i + 1]);
                break;
            case CL_WGL_HDC_KHR:
                gl_device_context = reinterpret_cast<HDC>(m_properties[i + 1]);
                break;
            case CL_CONTEXT_D3D11_DEVICE_KHR:
                has_d3d11_device = true;
                d3d11_device =
                    reinterpret_cast<ID3D11Device*>(m_properties[i + 1]);
                break;
#endif
            case 0:
                break;
            default:
                return CL_INVALID_PROPERTY;
            }
        }
#ifdef _WIN32
        if ((gl_context == nullptr) != (gl_device_context == nullptr)) {
            return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
        }
        if (gl_context != nullptr) {
            if (!m_device->is_vulkan_extension_enabled(
                    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
                return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
            }
            m_gl_interop =
                std::make_unique<cvk_gl_interop>(gl_context, gl_device_context);
            if (!m_gl_interop->init() ||
                memcmp(m_gl_interop->device_info().uuid.data(),
                       m_device->uuid(), CL_UUID_SIZE_KHR) != 0) {
                m_gl_interop.reset();
                return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
            }
        }
        if (has_d3d11_device) {
            m_d3d11_interop = std::make_unique<cvk_d3d11_interop>();
            cl_int result = m_d3d11_interop->init(d3d11_device);
            if (result != CL_SUCCESS) {
                m_d3d11_interop.reset();
                return result;
            }
        }
#endif
        return CL_SUCCESS;
    }

    const std::vector<cl_context_properties>& properties() const {
        return m_properties;
    }

    cvk_device* device() const { return m_device; }
#ifdef _WIN32
    cvk_gl_interop* gl_interop() const { return m_gl_interop.get(); }
    cvk_d3d11_interop* d3d11_interop() const { return m_d3d11_interop.get(); }
#endif
    bool interop_user_sync() const { return m_interop_user_sync; }
    unsigned num_devices() const { return 1u; }
    bool has_device(const cvk_device* device) const {
        return device == m_device;
    }

    void add_destructor_callback(cvk_context_callback_pointer_type ptr,
                                 void* user_data) {
        cvk_context_callback cb = {ptr, user_data};
        std::lock_guard<std::mutex> lock(m_callbacks_lock);
        m_destuctor_callbacks.push_back(cb);
    }

    bool is_mem_alloc_size_valid(size_t size) {
        // TODO support multiple devices
        return size <= m_device->max_mem_alloc_size();
    }

    int get_property_value_index(const int prop) {
        for (unsigned i = 0; i < m_properties.size(); i += 2) {
            if (m_properties[i] == prop) {
                return i + 1;
            }
        }
        return -1;
    }

    size_t get_printf_buffersize() {
        if (m_printf_buffersize) {
            return m_printf_buffersize;
        } else {
            return config.printf_buffer_size;
        }
    }
    cvk_printf_callback_t get_printf_callback() { return m_printf_callback; }
    void* get_printf_userdata() { return m_user_data; }

    cvk_command_queue* get_or_create_image_init_command_queue();
    void free_image_init_command_queue();

private:
    refcounted_holder<cvk_device> m_device;
    std::mutex m_callbacks_lock;
    std::vector<cvk_context_callback> m_destuctor_callbacks;
    std::vector<cl_context_properties> m_properties;
    size_t m_printf_buffersize;
    cvk_printf_callback_t m_printf_callback;
    void* m_user_data;
    bool m_interop_user_sync;

    std::mutex m_queue_image_init_lock;
    cvk_command_queue* m_queue_image_init = nullptr;
#ifdef _WIN32
    std::unique_ptr<cvk_gl_interop> m_gl_interop;
    std::unique_ptr<cvk_d3d11_interop> m_d3d11_interop;
#endif
};

static inline cvk_context* icd_downcast(cl_context context) {
    return static_cast<cvk_context*>(context);
}

using cvk_context_holder = refcounted_holder<cvk_context>;

template <object_magic magic>
struct api_object : public refcounted, object_magic_header<magic> {

    api_object(cvk_context* context) : m_context(context) {}
    cvk_context* context() const { return m_context; }

protected:
    cvk_context_holder m_context;
};
