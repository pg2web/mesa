/* SPDX-License-Identifier: MIT */

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <setupapi.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vgpu_api.h"
#include "util/u_debug.h"
#include "drm-uapi/virtgpu_drm.h"

DEFINE_GUID(GUID_DEVINTERFACE_VGPU, 0x31c22912, 0x7210, 0x11ed, 0xbf, 0x22, 0xbc, 0xe9, 0x2f, 0xa2, 0xe2, 0x2d);

#define VGPU_IOCTL(fn, method) CTL_CODE(FILE_DEVICE_UNKNOWN, fn, method, FILE_ANY_ACCESS)
#define IOCTL_VGPU_CONTEXT_INIT       VGPU_IOCTL(0x800, METHOD_OUT_DIRECT)
#define IOCTL_VGPU_DESTROY_CONTEXT    VGPU_IOCTL(0x801, METHOD_IN_DIRECT)
#define IOCTL_VGPU_MAP                VGPU_IOCTL(0x802, METHOD_OUT_DIRECT)
#define IOCTL_VGPU_EXECBUFFER         VGPU_IOCTL(0x803, METHOD_IN_DIRECT)
#define IOCTL_VGPU_GETPARAM           VGPU_IOCTL(0x804, METHOD_OUT_DIRECT)
#define IOCTL_VGPU_GET_CAPS           VGPU_IOCTL(0x805, METHOD_OUT_DIRECT)
#define IOCTL_VGPU_RESOURCE_CREATE    VGPU_IOCTL(0x806, METHOD_OUT_DIRECT)
#define IOCTL_VGPU_RESOURCE_CLOSE     VGPU_IOCTL(0x807, METHOD_IN_DIRECT)
#define IOCTL_VGPU_TRANSFER_FROM_HOST VGPU_IOCTL(0x808, METHOD_IN_DIRECT)
#define IOCTL_VGPU_TRANSFER_TO_HOST   VGPU_IOCTL(0x809, METHOD_IN_DIRECT)
#define IOCTL_VGPU_WAIT               VGPU_IOCTL(0x810, METHOD_OUT_DIRECT)
#define IOCTL_VGPU_RESOURCE_CREATE_BLOB VGPU_IOCTL(0x811, METHOD_OUT_DIRECT)

#define VGPU_EXECBUF_FENCE_FD_IN  0x01
#define VGPU_EXECBUF_FENCE_FD_OUT 0x02

struct vgpu_get_caps { uint32_t cap_set_id, cap_set_ver; };
struct vgpu_context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct vgpu_resource_create {
   uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
   uint32_t bo_handle, res_handle, size, stride;
};
struct vgpu_resource_create_resp { uint32_t bo_handle, res_handle; };
struct vgpu_resource_create_blob {
   uint32_t blob_mem, blob_flags, bo_handle, res_handle;
   uint64_t size;
   uint32_t pad, cmd_size;
   uint64_t cmd, blob_id;
};
struct vgpu_box { uint32_t x, y, z, w, h, d; };
struct vgpu_transfer {
   uint32_t bo_handle;
   struct vgpu_box box;
   uint32_t level, offset, stride, layer_stride, pad;
};
struct vgpu_wait { uint32_t handle, flags; };
struct vgpu_map { uint64_t offset; uint32_t handle, pad; };
struct vgpu_close { uint32_t handle, pad; };
struct vgpu_execbuffer {
   uint32_t flags, size;
   uint64_t command, bo_handles;
   uint32_t num_bo_handles, ring_idx;
   uint64_t in_fence_fd, out_fence_fd;
};

_Static_assert(sizeof(struct vgpu_resource_create) == 56, "vgpu_resource_create layout");
_Static_assert(sizeof(struct vgpu_transfer) == 48, "vgpu_transfer layout");
_Static_assert(sizeof(struct vgpu_execbuffer) == 48, "vgpu_execbuffer layout");
_Static_assert(sizeof(struct vgpu_context_init) == 16, "vgpu_context_init layout");
_Static_assert(sizeof(struct vgpu_resource_create_blob) == 48, "vgpu_resource_create_blob layout");

bool
VgpuDebugEnabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      char value[8];
      DWORD n = GetEnvironmentVariableA("VIRGL_PRESENT_DEBUG", value, sizeof(value));
      enabled = n > 0 && n < sizeof(value) && value[0] != '0';
   }
   return enabled;
}

void
VgpuDebugLog(const char *fmt, ...)
{
   char line[512];
   va_list args;
   int len;

   if (!VgpuDebugEnabled())
      return;

   len = snprintf(line, sizeof(line), "[virgl:%lu] ", GetCurrentProcessId());
   va_start(args, fmt);
   vsnprintf(line + len, sizeof(line) - len, fmt, args);
   va_end(args);
   OutputDebugStringA(line);
   fputs(line, stderr);
}

static int
vgpu_ioctl(HANDLE h, DWORD code, void *in, DWORD in_size, void *out, DWORD out_size, const char *what)
{
   DWORD returned = 0;

   if (!DeviceIoControl(h, code, in, in_size, out, out_size, &returned, NULL)) {
      DWORD err = GetLastError();
      _debug_printf("vgpu: %s failed (err=%lu)\n", what, err);
      errno = err == ERROR_ACCESS_DENIED ? EACCES :
              err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_NO_SYSTEM_RESOURCES ? ENOMEM :
              err == ERROR_OPERATION_ABORTED || err == ERROR_CANCELLED ? EINTR : EINVAL;
      return -1;
   }
   return 0;
}

HANDLE
GetHandleFromVgpu(HDC hDC)
{
   SP_DEVICE_INTERFACE_DATA ifdata = { sizeof(ifdata) };
   PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail = NULL;
   HANDLE handle = NULL;
   HDEVINFO set;
   DWORD size = 0;

   (void)hDC;

   set = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_VGPU, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
   if (set == INVALID_HANDLE_VALUE) {
      _debug_printf("vgpu: SetupDiGetClassDevs failed (err=%lu)\n", GetLastError());
      return NULL;
   }

   if (!SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_VGPU, 0, &ifdata)) {
      _debug_printf("vgpu: no vgpu device present (err=%lu)\n", GetLastError());
      goto out;
   }

   SetupDiGetDeviceInterfaceDetailA(set, &ifdata, NULL, 0, &size, NULL);
   detail = malloc(size);
   if (!detail)
      goto out;
   detail->cbSize = sizeof(*detail);
   if (!SetupDiGetDeviceInterfaceDetailA(set, &ifdata, detail, size, NULL, NULL)) {
      _debug_printf("vgpu: SetupDiGetDeviceInterfaceDetail failed (err=%lu)\n", GetLastError());
      goto out;
   }

   VgpuDebugLog("opening %s\n", detail->DevicePath);
   handle = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
   if (handle == INVALID_HANDLE_VALUE) {
      _debug_printf("vgpu: CreateFile failed (err=%lu)\n", GetLastError());
      handle = NULL;
   }

out:
   free(detail);
   SetupDiDestroyDeviceInfoList(set);
   return handle;
}

void
DestroyVirglContext(HANDLE handle)
{
   if (!handle)
      return;
   vgpu_ioctl(handle, IOCTL_VGPU_DESTROY_CONTEXT, NULL, 0, NULL, 0, "DESTROY_CONTEXT");
   CloseHandle(handle);
}

static int
ctl_get_param(HANDLE h, struct drm_virtgpu_getparam *gp)
{
   uint64_t param = gp->param, value = 0;

   if (vgpu_ioctl(h, IOCTL_VGPU_GETPARAM, &param, sizeof(param), &value, sizeof(value), "GETPARAM"))
      return -1;
   *(int *)(uintptr_t)gp->value = (int)value;
   return 0;
}

static int
ctl_get_caps(HANDLE h, struct drm_virtgpu_get_caps *gc)
{
   struct vgpu_get_caps in = { gc->cap_set_id, gc->cap_set_ver };

   return vgpu_ioctl(h, IOCTL_VGPU_GET_CAPS, &in, sizeof(in),
                     (void *)(uintptr_t)gc->addr, gc->size, "GET_CAPS");
}

static int
ctl_context_init(HANDLE h, struct drm_virtgpu_context_init *init)
{
   struct vgpu_context_init in = { init->num_params, 0, init->ctx_set_params };

   return vgpu_ioctl(h, IOCTL_VGPU_CONTEXT_INIT, &in, sizeof(in), NULL, 0, "CONTEXT_INIT");
}

static int
ctl_resource_create(HANDLE h, struct drm_virtgpu_resource_create *rc)
{
   struct vgpu_resource_create in = {
      .target = rc->target, .format = rc->format, .bind = rc->bind,
      .width = rc->width, .height = rc->height, .depth = rc->depth,
      .array_size = rc->array_size, .last_level = rc->last_level,
      .nr_samples = rc->nr_samples, .flags = rc->flags,
      .size = rc->size, .stride = rc->stride,
   };
   struct vgpu_resource_create_resp out = { 0 };

   if (vgpu_ioctl(h, IOCTL_VGPU_RESOURCE_CREATE, &in, sizeof(in), &out, sizeof(out), "RESOURCE_CREATE"))
      return -1;
   rc->bo_handle = out.bo_handle;
   rc->res_handle = out.res_handle;
   return 0;
}

static int
ctl_resource_create_blob(HANDLE h, struct drm_virtgpu_resource_create_blob *rc)
{
   struct vgpu_resource_create_blob in = {
      .blob_mem = rc->blob_mem, .blob_flags = rc->blob_flags,
      .size = rc->size, .cmd_size = rc->cmd_size,
      .cmd = rc->cmd, .blob_id = rc->blob_id,
   };
   struct vgpu_resource_create_resp out = { 0 };

   if (vgpu_ioctl(h, IOCTL_VGPU_RESOURCE_CREATE_BLOB, &in, sizeof(in), &out, sizeof(out), "RESOURCE_CREATE_BLOB"))
      return -1;
   rc->bo_handle = out.bo_handle;
   rc->res_handle = out.res_handle;
   return 0;
}

static int
ctl_resource_close(HANDLE h, struct drm_gem_close *gc)
{
   struct vgpu_close in = { gc->handle, 0 };

   return vgpu_ioctl(h, IOCTL_VGPU_RESOURCE_CLOSE, &in, sizeof(in), NULL, 0, "RESOURCE_CLOSE");
}

static int
ctl_map(HANDLE h, struct drm_virtgpu_map *map)
{
   struct vgpu_map in = { 0, map->handle, 0 };
   uint64_t ptr = 0;

   if (vgpu_ioctl(h, IOCTL_VGPU_MAP, &in, sizeof(in), &ptr, sizeof(ptr), "MAP"))
      return -1;
   map->offset = ptr;
   return 0;
}

static int
ctl_transfer(HANDLE h, bool to_host, const struct drm_virtgpu_3d_transfer_to_host *t)
{
   struct vgpu_transfer in = {
      .bo_handle = t->bo_handle,
      .box = { t->box.x, t->box.y, t->box.z, t->box.w, t->box.h, t->box.d },
      .level = t->level, .offset = t->offset,
      .stride = t->stride, .layer_stride = t->layer_stride,
   };

   return vgpu_ioctl(h, to_host ? IOCTL_VGPU_TRANSFER_TO_HOST : IOCTL_VGPU_TRANSFER_FROM_HOST,
                     &in, sizeof(in), NULL, 0, to_host ? "TRANSFER_TO_HOST" : "TRANSFER_FROM_HOST");
}

static int
ctl_wait(HANDLE h, struct drm_virtgpu_3d_wait *w)
{
   struct vgpu_wait in = { w->handle, w->flags };
   uint32_t busy = 0;

   if (vgpu_ioctl(h, IOCTL_VGPU_WAIT, &in, sizeof(in), &busy, sizeof(busy), "WAIT"))
      return -1;
   return busy ? 1 : 0;
}

static int
ctl_execbuffer(HANDLE h, struct drm_virtgpu_execbuffer *eb)
{
   struct vgpu_execbuffer in = {
      .flags = eb->flags & (VGPU_EXECBUF_FENCE_FD_IN | VGPU_EXECBUF_FENCE_FD_OUT),
      .size = eb->size,
      .command = eb->command,
      .bo_handles = eb->bo_handles,
      .num_bo_handles = eb->num_bo_handles,
      .in_fence_fd = (eb->flags & VGPU_EXECBUF_FENCE_FD_IN) ? (uint64_t)(uintptr_t)eb->in_fence_fd : 0,
      .out_fence_fd = (eb->flags & VGPU_EXECBUF_FENCE_FD_OUT) ? (uint64_t)(uintptr_t)eb->out_fence_fd : 0,
   };

   if (eb->size == 0)
      return 0;
   return vgpu_ioctl(h, IOCTL_VGPU_EXECBUFFER, &in, sizeof(in), NULL, 0, "EXECBUFFER");
}

int
drmIoctl(HANDLE fd, unsigned long request, void *arg)
{
   switch (request) {
   case DRM_IOCTL_VIRTGPU_GETPARAM:
      return ctl_get_param(fd, arg);
   case DRM_IOCTL_VIRTGPU_GET_CAPS:
      return ctl_get_caps(fd, arg);
   case DRM_IOCTL_VIRTGPU_CONTEXT_INIT:
      return ctl_context_init(fd, arg);
   case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE:
      return ctl_resource_create(fd, arg);
   case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB:
      return ctl_resource_create_blob(fd, arg);
   case DRM_IOCTL_GEM_CLOSE:
      return ctl_resource_close(fd, arg);
   case DRM_IOCTL_VIRTGPU_MAP:
      return ctl_map(fd, arg);
   case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:
      return ctl_transfer(fd, true, arg);
   case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:
      return ctl_transfer(fd, false, (const struct drm_virtgpu_3d_transfer_to_host *)arg);
   case DRM_IOCTL_VIRTGPU_WAIT:
      return ctl_wait(fd, arg);
   case DRM_IOCTL_VIRTGPU_EXECBUFFER:
      return ctl_execbuffer(fd, arg);
   default:
      errno = ENOSYS;
      return -1;
   }
}

int
drmCloseBufferHandle(HANDLE fd, uint32_t handle)
{
   struct drm_gem_close gc = { .handle = handle };
   return ctl_resource_close(fd, &gc);
}

drmVersionPtr
drmGetVersion(HANDLE fd)
{
   (void)fd;
   drmVersionPtr version = (drmVersionPtr)calloc(1, sizeof(drmVersion));
   if (version) {
      version->version_major = 0;
      version->version_minor = 1;
   }
   return version;
}

void
drmFreeVersion(drmVersionPtr ptr)
{
   free(ptr);
}

int
drmPrimeHandleToFD(HANDLE fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)handle;
   (void)flags;
   (void)prime_fd;
   errno = ENOSYS;
   return -1;
}

int
drmPrimeFDToHandle(HANDLE fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   (void)prime_fd;
   (void)handle;
   errno = ENOSYS;
   return -1;
}
