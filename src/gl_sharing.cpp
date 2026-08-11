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

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <limits>

#include "gl_sharing.hpp"
#include "log.hpp"

namespace {

// Mesa's GL interop ABI, from GL/mesa_glinterop.h. clvk is an out-of-tree
// consumer, so the layout is restated here rather than depending on a Mesa
// header. The version fields let Mesa reject mismatches safely.
constexpr uint32_t kDeviceInteropVersion = 4;
constexpr uint32_t kExportInteropVersion = 2;
constexpr size_t kUuidSize = 16;

struct mesa_glinterop_device_info {
    uint32_t version;
    uint32_t pci_segment_group;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_data_size;
    void* driver_data;
    char device_uuid[kUuidSize];
};

struct mesa_glinterop_export_in {
    uint32_t version;
    unsigned target;
    unsigned obj;
    unsigned miplevel;
    uint32_t access;
    uint32_t out_driver_data_size;
    void* out_driver_data;
};

struct mesa_glinterop_export_out {
    uint32_t version;
    HANDLE win32_handle;
    unsigned internal_format;
    ptrdiff_t buf_offset;
    ptrdiff_t buf_size;
    unsigned view_minlevel;
    unsigned view_numlevels;
    unsigned view_minlayer;
    unsigned view_numlayers;
    uint32_t out_driver_data_written;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t stride;
    uint64_t modifier;
};

struct mesa_glinterop_flush_out {
    uint32_t version;
    void** sync;
    int* fence_fd;
};

constexpr uint32_t kAccessReadWrite = 0;
constexpr uint32_t kAccessReadOnly = 1;
constexpr uint32_t kAccessWriteOnly = 2;
constexpr int kInteropSuccess = 0;

using query_device_info_fn = int(WINAPI*)(HDC, HGLRC,
                                          mesa_glinterop_device_info*);
using export_object_fn = int(WINAPI*)(HDC, HGLRC, mesa_glinterop_export_in*,
                                      mesa_glinterop_export_out*);
using flush_objects_fn = int(WINAPI*)(HDC, HGLRC, unsigned,
                                      mesa_glinterop_export_in*, void*);
using gl_client_wait_sync_fn = unsigned(WINAPI*)(void*, unsigned, uint64_t);
using gl_delete_sync_fn = void(WINAPI*)(void*);

constexpr unsigned kGlSyncFlushCommandsBit = 0x00000001;
constexpr unsigned kGlAlreadySignaled = 0x911A;
constexpr unsigned kGlConditionSatisfied = 0x911C;

bool valid_wgl_proc(PROC proc) {
    auto value = reinterpret_cast<uintptr_t>(proc);
    return proc != nullptr && value != 1 && value != 2 && value != 3 &&
           value != static_cast<uintptr_t>(-1);
}

uint32_t access_from_flags(cl_mem_flags flags) {
    if (flags & CL_MEM_READ_ONLY) {
        return kAccessReadOnly;
    }
    if (flags & CL_MEM_WRITE_ONLY) {
        return kAccessWriteOnly;
    }
    return kAccessReadWrite;
}

} // namespace

bool cvk_gl_interop::init() {
    HMODULE opengl = GetModuleHandleW(L"opengl32.dll");
    if (opengl == nullptr) {
        cvk_warn_fn("no opengl32.dll in this process, GL sharing unavailable");
        return false;
    }

    using wgl_get_proc_address_fn = PROC(WINAPI*)(LPCSTR);
    using wgl_get_current_context_fn = HGLRC(WINAPI*)();
    using wgl_get_current_dc_fn = HDC(WINAPI*)();
    using wgl_make_current_fn = BOOL(WINAPI*)(HDC, HGLRC);

    auto get_proc_address = reinterpret_cast<wgl_get_proc_address_fn>(
        GetProcAddress(opengl, "wglGetProcAddress"));
    auto get_current_context = reinterpret_cast<wgl_get_current_context_fn>(
        GetProcAddress(opengl, "wglGetCurrentContext"));
    auto get_current_dc = reinterpret_cast<wgl_get_current_dc_fn>(
        GetProcAddress(opengl, "wglGetCurrentDC"));
    auto make_current = reinterpret_cast<wgl_make_current_fn>(
        GetProcAddress(opengl, "wglMakeCurrent"));
    if (!get_proc_address || !get_current_context || !get_current_dc ||
        !make_current) {
        return false;
    }

    HGLRC previous_context = get_current_context();
    HDC previous_dc = get_current_dc();
    bool changed_context = previous_context != m_gl_context;
    if (changed_context && !make_current(m_device_context, m_gl_context)) {
        cvk_warn_fn("could not make the requested GL context current");
        return false;
    }

    m_query_device_info = reinterpret_cast<void*>(
        get_proc_address("wglMesaGLInteropQueryDeviceInfo"));
    m_export_object = reinterpret_cast<void*>(
        get_proc_address("wglMesaGLInteropExportObject"));
    m_flush_objects = reinterpret_cast<void*>(
        get_proc_address("wglMesaGLInteropFlushObjects"));
    m_client_wait_sync =
        reinterpret_cast<void*>(get_proc_address("glClientWaitSync"));
    m_delete_sync = reinterpret_cast<void*>(get_proc_address("glDeleteSync"));

    bool have_entrypoints =
        valid_wgl_proc(reinterpret_cast<PROC>(m_query_device_info)) &&
        valid_wgl_proc(reinterpret_cast<PROC>(m_export_object)) &&
        valid_wgl_proc(reinterpret_cast<PROC>(m_flush_objects)) &&
        valid_wgl_proc(reinterpret_cast<PROC>(m_client_wait_sync)) &&
        valid_wgl_proc(reinterpret_cast<PROC>(m_delete_sync));
    if (!have_entrypoints) {
        cvk_info_fn("GL implementation does not expose the required Mesa "
                    "interop entry points");
        if (changed_context) {
            make_current(previous_dc, previous_context);
        }
        return false;
    }

    mesa_glinterop_device_info info{};
    info.version = kDeviceInteropVersion;
    int ret = reinterpret_cast<query_device_info_fn>(m_query_device_info)(
        m_device_context, m_gl_context, &info);

    if (changed_context) {
        make_current(previous_dc, previous_context);
    }
    if (ret != kInteropSuccess || info.version < 3) {
        cvk_warn_fn("Mesa GL interop device query failed (%d, version %u)", ret,
                    info.version);
        return false;
    }

    m_device_info.vendor_id = info.vendor_id;
    m_device_info.device_id = info.device_id;
    std::copy_n(reinterpret_cast<const uint8_t*>(info.device_uuid), kUuidSize,
                m_device_info.uuid.begin());
    m_usable = true;
    return true;
}

bool cvk_gl_interop::export_object(cl_GLenum target, cl_GLint miplevel,
                                   cl_GLuint object, cl_mem_flags flags,
                                   cvk_gl_exported_object* out) {
    if (!m_usable || out == nullptr) {
        return false;
    }

    mesa_glinterop_export_in in{};
    in.version = kExportInteropVersion;
    in.target = target;
    in.obj = object;
    in.miplevel = static_cast<unsigned>(miplevel);
    in.access = access_from_flags(flags);
    cvk_zink_glinterop_export_info driver_info{};
    in.out_driver_data_size = sizeof(driver_info);
    in.out_driver_data = &driver_info;

    mesa_glinterop_export_out result{};
    result.version = kExportInteropVersion;
    int ret = reinterpret_cast<export_object_fn>(m_export_object)(
        m_device_context, m_gl_context, &in, &result);
    if (ret != kInteropSuccess) {
        cvk_warn_fn("Mesa GL interop export of object %u failed (%d)", object,
                    ret);
        return false;
    }
    if (result.win32_handle == nullptr) {
        cvk_warn_fn("Mesa GL interop export of object %u produced no handle",
                    object);
        return false;
    }
    if (result.out_driver_data_written < sizeof(driver_info) ||
        driver_info.magic != CVK_ZINK_GL_INTEROP_MAGIC ||
        driver_info.version != 1 ||
        driver_info.struct_size < sizeof(driver_info)) {
        cvk_warn_fn("Mesa GL interop export of object %u produced no valid "
                    "Zink metadata",
                    object);
        CloseHandle(result.win32_handle);
        return false;
    }

    out->win32_handle = result.win32_handle;
    out->size = result.buf_size > 0 ? static_cast<uint64_t>(result.buf_size)
                                    : driver_info.allocation_size;
    out->offset = static_cast<uint64_t>(result.buf_offset);
    out->row_stride = result.stride;
    out->internal_format = result.internal_format;
    out->width = result.width;
    out->height = result.height;
    out->depth = result.depth;
    out->view_min_level = result.view_minlevel;
    out->view_num_levels = result.view_numlevels;
    out->view_min_layer = result.view_minlayer;
    out->view_num_layers = result.view_numlayers;
    out->target = target;
    out->miplevel = miplevel;
    out->object = object;
    out->mesa_access = in.access;
    out->zink = driver_info;
    return true;
}

bool cvk_gl_interop::export_texture(cl_GLenum target, cl_GLint miplevel,
                                    cl_GLuint texture, cl_mem_flags flags,
                                    cvk_gl_exported_object* out) {
    return export_object(target, miplevel, texture, flags, out);
}

bool cvk_gl_interop::export_buffer(cl_GLuint buffer, cl_mem_flags flags,
                                   cvk_gl_exported_object* out) {
    constexpr cl_GLenum kArrayBuffer = 0x8892;
    return export_object(kArrayBuffer, 0, buffer, flags, out);
}

bool cvk_gl_interop::flush_objects(
    std::vector<cvk_gl_exported_object*>& exported) {
    if (!m_usable || exported.empty()) {
        return m_usable;
    }

    std::vector<mesa_glinterop_export_in> objects(exported.size());
    for (size_t i = 0; i < exported.size(); i++) {
        objects[i].version = kExportInteropVersion;
        objects[i].target = exported[i]->target;
        objects[i].obj = exported[i]->object;
        objects[i].miplevel = static_cast<unsigned>(exported[i]->miplevel);
        objects[i].access = exported[i]->mesa_access;
    }

    void* sync = nullptr;
    mesa_glinterop_flush_out flush_out{1, &sync, nullptr};
    int ret = reinterpret_cast<flush_objects_fn>(m_flush_objects)(
        m_device_context, m_gl_context, static_cast<unsigned>(objects.size()),
        objects.data(), &flush_out);
    if (ret != kInteropSuccess) {
        cvk_warn_fn("Mesa GL interop flush of %zu objects failed (%d)",
                    objects.size(), ret);
        return false;
    }
    if (sync == nullptr) {
        cvk_warn_fn("Mesa GL interop flush returned no GL sync object");
        return false;
    }

    auto wait_sync =
        reinterpret_cast<gl_client_wait_sync_fn>(m_client_wait_sync);
    auto delete_sync = reinterpret_cast<gl_delete_sync_fn>(m_delete_sync);
    unsigned wait_result = wait_sync(sync, kGlSyncFlushCommandsBit,
                                     std::numeric_limits<uint64_t>::max());
    delete_sync(sync);
    if (wait_result != kGlAlreadySignaled &&
        wait_result != kGlConditionSatisfied) {
        cvk_warn_fn("waiting for Mesa GL interop sync failed (0x%x)",
                    wait_result);
        return false;
    }

    // Refresh layout and queue-family ownership after Mesa has flushed and
    // released the resources. ExportObject returns a duplicate handle; only
    // the updated driver metadata is needed here.
    for (auto* object : exported) {
        cl_mem_flags flags = CL_MEM_READ_WRITE;
        if (object->mesa_access == kAccessReadOnly) {
            flags = CL_MEM_READ_ONLY;
        } else if (object->mesa_access == kAccessWriteOnly) {
            flags = CL_MEM_WRITE_ONLY;
        }
        cvk_gl_exported_object refreshed{};
        if (!export_object(object->target, object->miplevel, object->object,
                           flags, &refreshed)) {
            return false;
        }
        CloseHandle(refreshed.win32_handle);
        refreshed.win32_handle = nullptr;
        object->zink = refreshed.zink;
    }
    return true;
}

#endif // _WIN32
