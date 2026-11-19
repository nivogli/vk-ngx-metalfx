/* Smoke test: loads _nvngx.dll / nvngx.dll and calls Init, capability parameters, and optimal settings. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef uint32_t (*init_t)(uint64_t, const wchar_t *, void *, void *, void *, int, void *);
typedef uint32_t (*caps_t)(void ***);
typedef uint32_t (*geti_t)(void *, const char *, int *);
typedef uint32_t (*getp_t)(void *, const char *, void **);
typedef uint32_t (*getui_t)(void *, const char *, unsigned *);
typedef void (*setui_t)(void *, const char *, unsigned);
typedef void (*seti_t)(void *, const char *, int);
typedef uint32_t (*cb_t)(void *);
int main(int argc, char **argv)
{
	HMODULE m = LoadLibraryA(argc > 1 ? argv[1] : "_nvngx.dll");
	if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
	printf("Init_Ext %x\n", ((init_t)GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_Ext"))(1, L"C:\\", 0, 0, 0, 0x15, 0));
	void **p; printf("GetCapabilityParameters %x\n", ((caps_t)GetProcAddress(m, "NVSDK_NGX_VULKAN_GetCapabilityParameters"))(&p));
	void **vt = *(void ***)p; int avail = -1; void *cb = 0; unsigned w = 0, h = 0;
	printf("Get[11] SuperSampling.Available %x -> %d\n", ((geti_t)vt[11])(p, "SuperSampling.Available", &avail), avail);
	printf("Get[8] callback %x -> %p\n", ((getp_t)vt[8])(p, "DLSSOptimalSettingsCallback", &cb), cb);
	((setui_t)vt[4])(p, "Width", 2560); ((setui_t)vt[4])(p, "Height", 1440); ((seti_t)vt[3])(p, "PerfQualityValue", 1);
	printf("callback %x\n", ((cb_t)cb)(p));
	((getui_t)vt[12])(p, "OutWidth", &w); ((getui_t)vt[12])(p, "OutHeight", &h);
	printf("Balanced 2560x1440 -> %ux%u\n", w, h);
	return 0;
}
