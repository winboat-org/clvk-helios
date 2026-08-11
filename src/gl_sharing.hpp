// Copyright 2026 The clvk authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#ifdef _WIN32

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <windows.h>

#include "cl_headers.hpp"

// Support for cl_khr_gl_sharing on Windows.
//
// clvk is a Vulkan implementation of OpenCL, so sharing a GL texture means
// getting at the memory behind that texture and importing it into Vulkan.
// Mesa exposes exactly that through its GL interop extension, whose WGL
// entry points (wglMesaGLInteropQueryDeviceInfo / ExportObject /
// FlushObjects) are exported from opengl32.dll. On Windows the exporter hands
// back a Win32 handle -- see st_interop.c -- which is importable through
// VK_KHR_external_memory_win32.
//
// This only works when the GL implementation is Mesa. That is the case Helios
// cares about: the guest's OpenGL is Zink running on the same Venus device
// clvk itself uses. Against any other GL implementation the entry points are
// absent, loading fails cleanly, and cl_khr_gl_sharing must not be advertised.

struct cvk_gl_interop_device_info {
    uint32_t vendor_id;
    uint32_t device_id;
    std::array<uint8_t, 16> uuid;
};

constexpr uint32_t CVK_ZINK_GL_INTEROP_MAGIC = 0x314c475aU;

// Private Zink driver data returned through Mesa's versioned staging buffer.
// This must stay byte-for-byte compatible with zink_glinterop.h in Mesa.
struct cvk_zink_glinterop_export_info {
    uint32_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t object_type;
    uint32_t handle_type;
    uint32_t create_flags;
    uint32_t image_type;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t samples;
    uint32_t tiling;
    uint32_t usage;
    uint32_t sharing_mode;
    uint32_t layout;
    uint32_t released_queue_family;
    uint32_t reserved;
    uint64_t allocation_size;
    uint64_t memory_offset;
};

static_assert(sizeof(cvk_zink_glinterop_export_info) == 96);

struct cvk_gl_exported_object {
    HANDLE win32_handle; // owned; must be closed with CloseHandle
    uint64_t size;
    uint64_t offset;
    uint32_t row_stride;
    uint32_t internal_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t view_min_level;
    uint32_t view_num_levels;
    uint32_t view_min_layer;
    uint32_t view_num_layers;
    cl_GLenum target;
    cl_GLint miplevel;
    cl_GLuint object;
    uint32_t mesa_access;
    cvk_zink_glinterop_export_info zink;
};

// Wraps the Mesa GL interop entry points for one GL context. Instances are
// created per cl_context that carries CL_GL_CONTEXT_KHR, and are only usable
// while that GL context exists.
struct cvk_gl_interop {

    cvk_gl_interop(HGLRC gl_context, HDC device_context)
        : m_gl_context(gl_context), m_device_context(device_context) {}

    // Resolves the entry points and queries the GL device. Returns false when
    // the GL implementation does not expose Mesa's interop extension, in which
    // case sharing must not be offered.
    bool init();

    bool is_usable() const { return m_usable; }

    const cvk_gl_interop_device_info& device_info() const {
        return m_device_info;
    }

    // Exports the memory backing a GL texture. |out| is only written on
    // success, and its handle then belongs to the caller.
    bool export_texture(cl_GLenum target, cl_GLint miplevel, cl_GLuint texture,
                        cl_mem_flags flags, cvk_gl_exported_object* out);

    // Exports the memory backing a GL buffer object.
    bool export_buffer(cl_GLuint buffer, cl_mem_flags flags,
                       cvk_gl_exported_object* out);

    // Makes GL's pending writes to |objects| visible before CL reads them.
    // Called from clEnqueueAcquireGLObjects.
    bool flush_objects(std::vector<cvk_gl_exported_object*>& objects);

private:
    bool export_object(cl_GLenum target, cl_GLint miplevel, cl_GLuint object,
                       cl_mem_flags flags, cvk_gl_exported_object* out);

    HGLRC m_gl_context;
    HDC m_device_context;
    bool m_usable{false};
    cvk_gl_interop_device_info m_device_info{};

    // Resolved from opengl32.dll; typed as void* here so this header does not
    // have to pull in Mesa's interop declarations.
    void* m_query_device_info{nullptr};
    void* m_export_object{nullptr};
    void* m_flush_objects{nullptr};
    void* m_client_wait_sync{nullptr};
    void* m_delete_sync{nullptr};
};

#endif // _WIN32
