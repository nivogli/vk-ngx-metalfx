/*
 * Copyright 2026 Sercan Firat, Nivogli
 * SPDX-License-Identifier: MIT
 *
 * VK_LAYER_METALFX_nvidia_id: host Vulkan layer for running Vulkan DLSS games
 * on MetalFX-capable drivers (such as KosmicKrisp with MESA_KK_MFX=1).
 *
 *  - vkGetPhysicalDeviceProperties(2): reports vendorID 0x10DE, deviceID 0x2704
 *    (GeForce RTX 4080) and an NVIDIA-encoded driver version;
 *  - NVLAYER_NVX=1: advertises VK_NVX_binary_import and VK_NVX_image_view_handle,
 *    strips them from vkCreateDevice before the driver sees them, and stubs their
 *    entry points;
 *  - NVLAYER_LUID=<low hex>:<high hex>: optionally overrides the device LUID;
 *  - Log: $NVLAYER_LOG (default /tmp/nvlayer.log). NVLAYER_SPOOF=0 logs only.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define EXPORT __attribute__((visibility("default")))
#define NV_VENDOR 0x10DE
#define NV_DEVICE 0x2704
#define NV_DRIVER ((581u << 22) | (57u << 14))

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf;

static void
logm(const char *fmt, ...)
{
   pthread_mutex_lock(&lock);
   if (!logf) {
      const char *p = getenv("NVLAYER_LOG");
      logf = fopen(p ? p : "/tmp/nvlayer.log", "a");
   }
   if (logf) {
      va_list ap;
      va_start(ap, fmt);
      vfprintf(logf, fmt, ap);
      va_end(ap);
      fflush(logf);
   }
   pthread_mutex_unlock(&lock);
}

static int
spoof(void)
{
   const char *s = getenv("NVLAYER_SPOOF");
   return !s || s[0] != '0';
}

/* Dispatchable handles start with the loader's dispatch table pointer: one key per instance / device. */
#define KEY(h) (*(void **)(h))
struct entry {
   void *key;
   PFN_vkGetInstanceProcAddr gipa;
   PFN_vkGetDeviceProcAddr gdpa;
   VkInstance instance;
};
static struct entry table[64];

static struct entry *
find(void *key)
{
   for (int i = 0; i < 64; i++)
      if (table[i].key == key)
         return &table[i];
   return NULL;
}

static struct entry *
add(void *key)
{
   struct entry *e = find(key);
   for (int i = 0; !e && i < 64; i++)
      if (!table[i].key)
         e = &table[i];
   if (e) {
      memset(e, 0, sizeof(*e));
      e->key = key;
   }
   return e;
}

static void
log_pnext(const char *what, const void *pnext)
{
   for (const VkBaseInStructure *s = pnext; s; s = s->pNext)
      logm("  %s pNext sType %d\n", what, (int)s->sType);
}

static void
log_features(const VkPhysicalDeviceFeatures *f)
{
   const VkBool32 *b = (const VkBool32 *)f;
   logm("  VkPhysicalDeviceFeatures on (index):");
   for (unsigned i = 0; i < sizeof(*f) / sizeof(VkBool32); i++)
      if (b[i])
         logm(" %u", i);
   logm("\n");
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *inst)
{
   VkLayerInstanceCreateInfo *li = (VkLayerInstanceCreateInfo *)ci->pNext;
   while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
      li = (VkLayerInstanceCreateInfo *)li->pNext;
   if (!li)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   li->u.pLayerInfo = li->u.pLayerInfo->pNext;
   PFN_vkCreateInstance next = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

   logm("vkCreateInstance app '%s' engine '%s' api 0x%x spoof %d\n",
        ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName ? ci->pApplicationInfo->pApplicationName : "",
        ci->pApplicationInfo && ci->pApplicationInfo->pEngineName ? ci->pApplicationInfo->pEngineName : "",
        ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0, spoof());
   for (uint32_t i = 0; i < ci->enabledLayerCount; i++)
      logm("  instance layer %s\n", ci->ppEnabledLayerNames[i]);
   for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
      logm("  instance extension %s\n", ci->ppEnabledExtensionNames[i]);
   log_pnext("instance", ci->pNext);

   VkResult r = next(ci, a, inst);
   logm("  -> %d\n", r);
   if (r == VK_SUCCESS) {
      pthread_mutex_lock(&lock);
      struct entry *e = add(KEY(*inst));
      if (e) {
         e->gipa = gipa;
         e->instance = *inst;
      }
      pthread_mutex_unlock(&lock);
   }
   return r;
}

static PFN_vkVoidFunction
inst_next(void *key, const char *name)
{
   struct entry *e = find(key);
   return e ? e->gipa(e->instance, name) : NULL;
}

static void
patch_props(VkPhysicalDeviceProperties *p)
{
   logm("vkGetPhysicalDeviceProperties: %s vendor 0x%x device 0x%x driver 0x%x%s\n", p->deviceName, p->vendorID,
        p->deviceID, p->driverVersion, spoof() ? " -> NVIDIA" : "");
   if (spoof()) {
      p->vendorID = NV_VENDOR;
      p->deviceID = NV_DEVICE;
      p->driverVersion = NV_DRIVER;
   }
}

static VKAPI_ATTR void VKAPI_CALL
nv_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
   ((PFN_vkGetPhysicalDeviceProperties)inst_next(KEY(pd), "vkGetPhysicalDeviceProperties"))(pd, p);
   patch_props(p);
}

/* NVLAYER_LUID=<low hex>:<high hex> (Wine's DEVPKEY_GPU_LUID for the GPU, e.g. 3f4:0) replaces the
 * driver's deviceLUID and marks it valid. MoltenVK reports the IORegistry ID with deviceLUIDValid false; NGX
 * stops there ("unable to fetch LUID") and the game's DXGI adapter-by-LUID lookup finds nothing. */
static void
patch_luid(uint8_t *luid, VkBool32 *valid, const char *what)
{
   const char *s = getenv("NVLAYER_LUID");
   uint32_t lo, hi;
   memcpy(&lo, luid, 4);
   memcpy(&hi, luid + 4, 4);
   logm("  %s LUID %x:%x valid %u", what, lo, hi, *valid);
   if (s && sscanf(s, "%x:%x", &lo, &hi) == 2) {
      memcpy(luid, &lo, 4);
      memcpy(luid + 4, &hi, 4);
      *valid = VK_TRUE;
      logm(" -> %x:%x valid 1", lo, hi);
   }
   logm("\n");
}

static VKAPI_ATTR void VKAPI_CALL
nv_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
   ((PFN_vkGetPhysicalDeviceProperties2)inst_next(KEY(pd), "vkGetPhysicalDeviceProperties2"))(pd, p);
   patch_props(&p->properties);
   for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext)
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
         VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *)s;
         logm("  driver properties: id %d name '%s' info '%s'\n", d->driverID, d->driverName, d->driverInfo);
      } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
         VkPhysicalDeviceIDProperties *d = (VkPhysicalDeviceIDProperties *)s;
         patch_luid(d->deviceLUID, &d->deviceLUIDValid, "ID properties");
      } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
         VkPhysicalDeviceVulkan11Properties *d = (VkPhysicalDeviceVulkan11Properties *)s;
         patch_luid(d->deviceLUID, &d->deviceLUIDValid, "Vulkan 1.1 properties");
      } else {
         logm("  properties2 pNext sType %d\n", (int)s->sType);
      }
}

static int
fake_nvx(void)
{
   const char *s = getenv("NVLAYER_NVX");
   return s && s[0] == '1';
}

static const VkExtensionProperties nvx_exts[] = {
   { VK_NVX_BINARY_IMPORT_EXTENSION_NAME, VK_NVX_BINARY_IMPORT_SPEC_VERSION },
   { VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME, VK_NVX_IMAGE_VIEW_HANDLE_SPEC_VERSION },
};
#define NVX_COUNT (sizeof(nvx_exts) / sizeof(nvx_exts[0]))

static int
is_nvx(const char *name)
{
   for (unsigned i = 0; i < NVX_COUNT; i++)
      if (!strcmp(name, nvx_exts[i].extensionName))
         return 1;
   return 0;
}

static VkResult
copy_exts(const VkExtensionProperties *src, uint32_t count, uint32_t *n, VkExtensionProperties *p)
{
   if (!p) {
      *n = count;
      return VK_SUCCESS;
   }
   uint32_t c = *n < count ? *n : count;
   memcpy(p, src, c * sizeof(*p));
   *n = c;
   return c < count ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p)
{
   if (layer && (!strcmp(layer, "VK_LAYER_METALFX_nvidia_id") ||
                 !strcmp(layer, "VK_LAYER_ENSHROUDED_nvidia_id")))
      return copy_exts(nvx_exts, fake_nvx() ? NVX_COUNT : 0, n, p);
   PFN_vkEnumerateDeviceExtensionProperties next =
      (PFN_vkEnumerateDeviceExtensionProperties)inst_next(KEY(pd), "vkEnumerateDeviceExtensionProperties");
   if (layer || !fake_nvx()) {
      VkResult r = next(pd, layer, n, p);
      if (p)
         logm("vkEnumerateDeviceExtensionProperties: %u extensions (layer %s) -> %d\n", *n, layer ? layer : "-", r);
      return r;
   }
   /* The driver's list with the two NVX extensions appended (MoltenVK and KK have neither). */
   uint32_t dn = 0;
   VkResult r = next(pd, NULL, &dn, NULL);
   if (r != VK_SUCCESS)
      return r;
   VkExtensionProperties *all = calloc(dn + NVX_COUNT, sizeof(*all));
   if (!all)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   r = next(pd, NULL, &dn, all);
   if (r < 0) {
      free(all);
      return r;
   }
   uint32_t total = dn;
   for (unsigned i = 0; i < NVX_COUNT; i++) {
      int have = 0;
      for (uint32_t j = 0; j < dn; j++)
         have |= !strcmp(all[j].extensionName, nvx_exts[i].extensionName);
      if (!have)
         all[total++] = nvx_exts[i];
   }
   r = copy_exts(all, total, n, p);
   free(all);
   if (p)
      logm("vkEnumerateDeviceExtensionProperties: %u extensions incl. %u fake NVX -> %d\n", *n, total - dn, r);
   return r;
}

/* VK_NVX_binary_import / VK_NVX_image_view_handle stubs. Each call is logged (launches and handle queries
 * only for the first few hundred) and nothing reaches the driver. */
static uint64_t nvx_next_handle = 0x4e560000;
static unsigned nvx_launches, nvx_handles;

static uint64_t
nvx_handle(void)
{
   pthread_mutex_lock(&lock);
   uint64_t h = ++nvx_next_handle;
   pthread_mutex_unlock(&lock);
   return h;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_CreateCuModuleNVX(VkDevice dev, const VkCuModuleCreateInfoNVX *ci, const VkAllocationCallbacks *a, VkCuModuleNVX *m)
{
   const unsigned char *d = ci->pData;
   *m = (VkCuModuleNVX)nvx_handle();
   logm("vkCreateCuModuleNVX: %zu bytes, first bytes %02x %02x %02x %02x %02x %02x %02x %02x -> 0x%llx\n",
        ci->dataSize, ci->dataSize > 0 ? d[0] : 0, ci->dataSize > 1 ? d[1] : 0, ci->dataSize > 2 ? d[2] : 0,
        ci->dataSize > 3 ? d[3] : 0, ci->dataSize > 4 ? d[4] : 0, ci->dataSize > 5 ? d[5] : 0,
        ci->dataSize > 6 ? d[6] : 0, ci->dataSize > 7 ? d[7] : 0, (unsigned long long)(uintptr_t)*m);
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_CreateCuFunctionNVX(VkDevice dev, const VkCuFunctionCreateInfoNVX *ci, const VkAllocationCallbacks *a, VkCuFunctionNVX *f)
{
   *f = (VkCuFunctionNVX)nvx_handle();
   logm("vkCreateCuFunctionNVX: module 0x%llx '%s' -> 0x%llx\n", (unsigned long long)(uintptr_t)ci->module,
        ci->pName ? ci->pName : "", (unsigned long long)(uintptr_t)*f);
   return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
nv_DestroyCuModuleNVX(VkDevice dev, VkCuModuleNVX m, const VkAllocationCallbacks *a)
{
   logm("vkDestroyCuModuleNVX: 0x%llx\n", (unsigned long long)(uintptr_t)m);
}

static VKAPI_ATTR void VKAPI_CALL
nv_DestroyCuFunctionNVX(VkDevice dev, VkCuFunctionNVX f, const VkAllocationCallbacks *a)
{
   logm("vkDestroyCuFunctionNVX: 0x%llx\n", (unsigned long long)(uintptr_t)f);
}

static VKAPI_ATTR void VKAPI_CALL
nv_CmdCuLaunchKernelNVX(VkCommandBuffer cmd, const VkCuLaunchInfoNVX *li)
{
   if (nvx_launches++ < 300)
      logm("vkCmdCuLaunchKernelNVX (dropped): function 0x%llx grid %ux%ux%u block %ux%ux%u shared %u params %zu extras %zu\n",
           (unsigned long long)(uintptr_t)li->function, li->gridDimX, li->gridDimY, li->gridDimZ, li->blockDimX,
           li->blockDimY, li->blockDimZ, li->sharedMemBytes, li->paramCount, li->extraCount);
}

static VKAPI_ATTR uint32_t VKAPI_CALL
nv_GetImageViewHandleNVX(VkDevice dev, const VkImageViewHandleInfoNVX *info)
{
   uint32_t h = (uint32_t)nvx_handle();
   if (nvx_handles++ < 300)
      logm("vkGetImageViewHandleNVX: view 0x%llx type %d sampler 0x%llx -> 0x%x\n",
           (unsigned long long)(uintptr_t)info->imageView, (int)info->descriptorType,
           (unsigned long long)(uintptr_t)info->sampler, h);
   return h;
}

static VKAPI_ATTR uint64_t VKAPI_CALL
nv_GetImageViewHandle64NVX(VkDevice dev, const VkImageViewHandleInfoNVX *info)
{
   uint64_t h = nvx_handle();
   if (nvx_handles++ < 300)
      logm("vkGetImageViewHandle64NVX: view 0x%llx type %d -> 0x%llx\n", (unsigned long long)(uintptr_t)info->imageView,
           (int)info->descriptorType, (unsigned long long)h);
   return h;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_GetImageViewAddressNVX(VkDevice dev, VkImageView view, VkImageViewAddressPropertiesNVX *p)
{
   p->deviceAddress = 0;
   p->size = 0;
   if (nvx_handles++ < 300)
      logm("vkGetImageViewAddressNVX: view 0x%llx -> address 0\n", (unsigned long long)(uintptr_t)view);
   return VK_SUCCESS;
}

static VKAPI_ATTR uint64_t VKAPI_CALL
nv_GetDeviceCombinedImageSamplerIndexNVX(VkDevice dev, uint64_t view, uint64_t sampler)
{
   logm("vkGetDeviceCombinedImageSamplerIndexNVX: view %llu sampler %llu\n", (unsigned long long)view,
        (unsigned long long)sampler);
   return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nv_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *dev)
{
   VkLayerDeviceCreateInfo *li = (VkLayerDeviceCreateInfo *)ci->pNext;
   while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
      li = (VkLayerDeviceCreateInfo *)li->pNext;
   if (!li)
      return VK_ERROR_INITIALIZATION_FAILED;
   PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
   PFN_vkGetDeviceProcAddr gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
   li->u.pLayerInfo = li->u.pLayerInfo->pNext;
   struct entry *ie = find(KEY(pd));
   PFN_vkCreateDevice next = (PFN_vkCreateDevice)gipa(ie ? ie->instance : NULL, "vkCreateDevice");

   logm("vkCreateDevice: %u queue infos\n", ci->queueCreateInfoCount);
   for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
      logm("  device extension %s\n", ci->ppEnabledExtensionNames[i]);
   if (ci->pEnabledFeatures)
      log_features(ci->pEnabledFeatures);
   for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
         log_features(&((const VkPhysicalDeviceFeatures2 *)s)->features);
      else if (s->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
         logm("  device pNext sType %d\n", (int)s->sType);
   }
   /* The driver never sees the fake NVX extensions. */
   VkDeviceCreateInfo dci = *ci;
   const char **names = calloc(ci->enabledExtensionCount + 1, sizeof(*names));
   if (!names)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   dci.enabledExtensionCount = 0;
   for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
      if (fake_nvx() && is_nvx(ci->ppEnabledExtensionNames[i]))
         logm("  (fake, not passed to the driver) %s\n", ci->ppEnabledExtensionNames[i]);
      else
         names[dci.enabledExtensionCount++] = ci->ppEnabledExtensionNames[i];
   }
   dci.ppEnabledExtensionNames = names;
   VkResult r = next(pd, &dci, a, dev);
   free(names);
   logm("  -> %d\n", r);
   if (r == VK_SUCCESS) {
      pthread_mutex_lock(&lock);
      struct entry *e = add(KEY(*dev));
      if (e)
         e->gdpa = gdpa;
      pthread_mutex_unlock(&lock);
   }
   return r;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nv_GetDeviceProcAddr(VkDevice dev, const char *name);

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nv_GetInstanceProcAddr(VkInstance inst, const char *name)
{
#define HOOK(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction)nv_##fn
   HOOK(CreateInstance);
   HOOK(GetInstanceProcAddr);
   HOOK(GetPhysicalDeviceProperties);
   HOOK(GetPhysicalDeviceProperties2);
   if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
      return (PFN_vkVoidFunction)nv_GetPhysicalDeviceProperties2;
   HOOK(EnumerateDeviceExtensionProperties);
   HOOK(CreateDevice);
   HOOK(GetDeviceProcAddr);
#undef HOOK
   if (!inst)
      return NULL;
   return inst_next(KEY(inst), name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
nv_GetDeviceProcAddr(VkDevice dev, const char *name)
{
   if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)nv_GetDeviceProcAddr;
   if (fake_nvx()) {
#define NVX(fn) if (!strcmp(name, "vk" #fn)) { logm("vkGetDeviceProcAddr %s -> stub\n", name); return (PFN_vkVoidFunction)nv_##fn; }
      NVX(CreateCuModuleNVX);
      NVX(CreateCuFunctionNVX);
      NVX(DestroyCuModuleNVX);
      NVX(DestroyCuFunctionNVX);
      NVX(CmdCuLaunchKernelNVX);
      NVX(GetImageViewHandleNVX);
      NVX(GetImageViewHandle64NVX);
      NVX(GetImageViewAddressNVX);
      NVX(GetDeviceCombinedImageSamplerIndexNVX);
#undef NVX
   }
   struct entry *e = find(KEY(dev));
   return e ? e->gdpa(dev, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
   if (v->loaderLayerInterfaceVersion > 2)
      v->loaderLayerInterfaceVersion = 2;
   v->pfnGetInstanceProcAddr = nv_GetInstanceProcAddr;
   v->pfnGetDeviceProcAddr = nv_GetDeviceProcAddr;
   v->pfnGetPhysicalDeviceProcAddr = NULL;
   return VK_SUCCESS;
}
