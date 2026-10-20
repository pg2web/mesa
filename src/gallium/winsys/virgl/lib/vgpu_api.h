/* SPDX-License-Identifier: MIT */

/* User-mode transport between the virgl winsys and the vgpu-accel kernel driver (vgpu.sys) */
#ifndef VGPU_API_H
#define VGPU_API_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

HANDLE GetHandleFromVgpu(HDC hDC);
void DestroyVirglContext(HANDLE handle);
bool VgpuDebugEnabled(void);
void VgpuDebugLog(const char *fmt, ...);

int drmIoctl(HANDLE fd, unsigned long request, void *arg);
int drmCloseBufferHandle(HANDLE fd, uint32_t handle);
int drmPrimeHandleToFD(HANDLE fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(HANDLE fd, int prime_fd, uint32_t *handle);
drmVersionPtr drmGetVersion(HANDLE fd);
void drmFreeVersion(drmVersionPtr ptr);

#endif /* VGPU_API_H */
