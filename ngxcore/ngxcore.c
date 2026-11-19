/*
 * Copyright 2026 Sercan Firat, Nivogli
 * SPDX-License-Identifier: MIT
 *
 * Minimal Windows NGX core stand-in (_nvngx.dll / nvngx.dll) for Vulkan DLSS
 * applications under Wine/macOS. Implements the Vulkan NGX entry points using
 * the public NVIDIA DLSS SDK parameter names.
 *
 * Registry load path: HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath
 * (or next to the application as nvngx.dll). Log: NGXSTUB_LOG, else C:\ngxstub.log.
 *
 * NVSDK_NGX_Parameter is an MSVC C++ interface with overloaded virtuals. MSVC
 * groups the overloads and puts each group in reverse declaration order, so the
 * vtable is constructed manually (param_vtbl).
 *
 * EvaluateFeature parses the NVSDK_NGX_Resource_VK pointers for Color, Output,
 * Depth and MotionVectors (VkImage handles pass through winevulkan 1:1) and,
 * when NGXSTUB_MFX=1, records vkCmdPushConstants with a magic layout and an
 * 88-byte payload for the host Vulkan driver (KosmicKrisp with MESA_KK_MFX=1).
 *
 * Build: x86_64-w64-mingw32-clang -O1 -shared -o nvngx.dll ngxcore.c
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

typedef uint32_t res_t;
#define OK 0x1u
#define FAIL 0xBAD00000u
#define FAIL_NOTFOUND (FAIL | 4)

static FILE *logf_;
static CRITICAL_SECTION lk;
static void logm(const char *fmt, ...)
{
	EnterCriticalSection(&lk);
	if (!logf_) {
		char p[MAX_PATH]; DWORD n = GetEnvironmentVariableA("NGXSTUB_LOG", p, sizeof p);
		logf_ = fopen(n && n < sizeof p ? p : "C:\\ngxstub.log", "a");
	}
	if (logf_) {
		SYSTEMTIME t; GetLocalTime(&t);
		fprintf(logf_, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
		va_list a; va_start(a, fmt); vfprintf(logf_, fmt, a); va_end(a);
		fputc('\n', logf_); fflush(logf_);
	}
	LeaveCriticalSection(&lk);
}

/* ---- parameter store ---- */
enum { T_ULL, T_F, T_D, T_UI, T_I, T_PTR };
typedef struct { char name[96]; int type; union { uint64_t u; float f; double d; uint32_t ui; int32_t i; void *p; } v; } entry;
typedef struct param { const void *const *vtbl; int n, cap; entry *e; int quiet; } param;

static entry *find(param *p, const char *name)
{
	for (int i = 0; i < p->n; i++) if (!strcmp(p->e[i].name, name)) return &p->e[i];
	return NULL;
}
static entry *put(param *p, const char *name, int type)
{
	entry *e = find(p, name);
	if (!e) {
		if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->e = realloc(p->e, p->cap * sizeof *p->e); }
		e = &p->e[p->n++]; memset(e, 0, sizeof *e); snprintf(e->name, sizeof e->name, "%s", name);
	}
	e->type = type; return e;
}
static double as_d(entry *e)
{
	switch (e->type) { case T_ULL: return (double)e->v.u; case T_F: return e->v.f; case T_D: return e->v.d;
	case T_UI: return e->v.ui; case T_I: return e->v.i; default: return (double)(uintptr_t)e->v.p; }
}
static uint64_t as_u(entry *e)
{
	switch (e->type) { case T_F: return (uint64_t)e->v.f; case T_D: return (uint64_t)e->v.d;
	case T_UI: return e->v.ui; case T_I: return (uint64_t)(int64_t)e->v.i; default: return e->v.u; }
}

/* Per-frame Set/Get calls are logged for the first 3 frames only: logging every one (~3000 lines/s, each
 * flushed) would slow the DLSS side of a timed run. */
static unsigned evals;
#define CHATTY(p) (!(p)->quiet && evals < 3)

#define SET(slot, ctype, T, field, fmt) \
	static void set_##slot(param *p, const char *name, ctype v) \
	{ if (CHATTY(p)) logm("  Set[%d] %s = " fmt, slot, name, v); put(p, name, T)->v.field = v; }
SET(0, void *, T_PTR, p, "%p")
SET(1, void *, T_PTR, p, "%p (d3d12)")
SET(2, void *, T_PTR, p, "%p (d3d11)")
SET(3, int32_t, T_I, i, "%d")
SET(4, uint32_t, T_UI, ui, "%u")
SET(5, double, T_D, d, "%g")
SET(6, float, T_F, f, "%g")
SET(7, uint64_t, T_ULL, u, "%llu")

#define GET(slot, ctype, conv) \
	static res_t get_##slot(param *p, const char *name, ctype *out) \
	{ entry *e = find(p, name); \
	  if (CHATTY(p)) logm("  Get[%d] %s%s", slot, name, e ? "" : " (missing)"); \
	  if (!e || !out) return FAIL; *out = (ctype)(conv); return OK; }
GET(8, void *, e->v.p)
GET(9, void *, e->v.p)
GET(10, void *, e->v.p)
GET(11, int32_t, (int64_t)as_u(e))
GET(12, uint32_t, as_u(e))
GET(13, double, as_d(e))
GET(14, float, as_d(e))
GET(15, uint64_t, as_u(e))
static void reset_16(param *p) { logm("  Reset[16]"); p->n = 0; }

static const void *const param_vtbl[] = {
	set_0, set_1, set_2, set_3, set_4, set_5, set_6, set_7,
	get_8, get_9, get_10, get_11, get_12, get_13, get_14, get_15, reset_16,
};
static param *new_param(void) { param *p = calloc(1, sizeof *p); p->vtbl = param_vtbl; return p; }

/* DLSS's render-size ratios per NVSDK_NGX_PerfQuality_Value. NGXSTUB_RATIO (e.g. 1.7 = FSR 3 Balanced) overrides
 * all of them, so a timed DLSS run renders at the same size as the FSR 3 run it is compared with. */
static const double ratio[] = { 2.0, 1.0 / 0.58, 1.5, 3.0, 1.3, 1.0 };

static res_t optimal_settings(param *p)
{
	entry *w = find(p, "Width"), *h = find(p, "Height"), *q = find(p, "PerfQualityValue");
	unsigned W = w ? (unsigned)as_u(w) : 0, H = h ? (unsigned)as_u(h) : 0, Q = q ? (unsigned)as_u(q) : 1;
	double r = ratio[Q < 6 ? Q : 1];
	char v[16]; DWORD n = GetEnvironmentVariableA("NGXSTUB_RATIO", v, sizeof v);
	if (n && n < sizeof v && atof(v) >= 1.0) r = atof(v);
	unsigned ow = (unsigned)(W / r + 0.5), oh = (unsigned)(H / r + 0.5);
	logm("OptimalSettings %ux%u quality %u -> %ux%u", W, H, Q, ow, oh);
	put(p, "OutWidth", T_UI)->v.ui = ow; put(p, "OutHeight", T_UI)->v.ui = oh;
	put(p, "DLSS.Get.Dynamic.Max.Render.Width", T_UI)->v.ui = W; put(p, "DLSS.Get.Dynamic.Max.Render.Height", T_UI)->v.ui = H;
	put(p, "DLSS.Get.Dynamic.Min.Render.Width", T_UI)->v.ui = W / 3; put(p, "DLSS.Get.Dynamic.Min.Render.Height", T_UI)->v.ui = H / 3;
	put(p, "Sharpness", T_F)->v.f = 0.0f;
	return OK;
}
static res_t get_stats(param *p) { put(p, "SizeInBytes", T_ULL)->v.u = 0; return OK; }

static void fill_caps(param *p)
{
	p->quiet = 1;
	put(p, "SuperSampling.Available", T_I)->v.i = 1;
	put(p, "SuperSampling.NeedsUpdatedDriver", T_I)->v.i = 0;
	put(p, "SuperSampling.MinDriverVersionMajor", T_UI)->v.ui = 0;
	put(p, "SuperSampling.MinDriverVersionMinor", T_UI)->v.ui = 0;
	put(p, "SuperSampling.FeatureInitResult", T_I)->v.i = OK;
	put(p, "SuperSamplingDenoising.Available", T_I)->v.i = 0;
	put(p, "FrameGeneration.Available", T_I)->v.i = 0;
	put(p, "ImageSuperResolution.Available", T_I)->v.i = 0;
	put(p, "DLSSOptimalSettingsCallback", T_PTR)->v.p = (void *)optimal_settings;
	put(p, "DLSSGetStatsCallback", T_PTR)->v.p = (void *)get_stats;
	p->quiet = 0;
}

static param *caps;
static unsigned next_handle = 1;

/* ---- exports ---- */
#define API __declspec(dllexport)
typedef uint64_t u64;

static res_t init_any(const char *fn, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h, u64 i)
{
	logm("%s(%llx, %llx, %llx, %llx, %llx, %llx, %llx, %llx, %llx)", fn, a, b, c, d, e, f, g, h, i);
	if (!caps) { caps = new_param(); fill_caps(caps); }
	return OK;
}
#define INIT(fn) API res_t fn(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h, u64 i, u64 j, u64 k) \
	{ (void)j; (void)k; return init_any(#fn, a, b, c, d, e, f, g, h, i); }
INIT(NVSDK_NGX_VULKAN_Init)
INIT(NVSDK_NGX_VULKAN_Init_Ext)
INIT(NVSDK_NGX_VULKAN_Init_Ext2)
INIT(NVSDK_NGX_VULKAN_Init_ProjectID)
INIT(NVSDK_NGX_VULKAN_Init_ProjectID_Ext)
INIT(NVSDK_NGX_VULKAN_Init_with_ProjectID)

API res_t NVSDK_NGX_VULKAN_RequiredExtensions(unsigned *ic, const char ***ie, unsigned *dc, const char ***de)
{
	static const char *none[1];
	logm("RequiredExtensions");
	if (ic) *ic = 0; if (ie) *ie = none; if (dc) *dc = 0; if (de) *de = none;
	return OK;
}
API res_t NVSDK_NGX_VULKAN_Shutdown(void) { logm("Shutdown"); return OK; }
API res_t NVSDK_NGX_VULKAN_Shutdown1(void *dev) { logm("Shutdown1(%p)", dev); return OK; }
API res_t NVSDK_NGX_VULKAN_GetParameters(param **out)
{
	logm("GetParameters"); if (!caps) { caps = new_param(); fill_caps(caps); }
	*out = caps; return OK;
}
API res_t NVSDK_NGX_VULKAN_GetCapabilityParameters(param **out)
{
	param *p = new_param(); fill_caps(p); logm("GetCapabilityParameters -> %p", p); *out = p; return OK;
}
API res_t NVSDK_NGX_VULKAN_AllocateParameters(param **out)
{
	param *p = new_param(); logm("AllocateParameters -> %p", p); *out = p; return OK;
}
API res_t NVSDK_NGX_VULKAN_DestroyParameters(param *p)
{
	logm("DestroyParameters(%p)", p); if (p && p != caps) { free(p->e); free(p); } return OK;
}
API res_t NVSDK_NGX_VULKAN_GetScratchBufferSize(int feature, const param *p, size_t *out)
{
	logm("GetScratchBufferSize(feature %d)", feature); if (out) *out = 0; return OK;
}

static void dump(const char *what, const param *p)
{
	if (!p) return;
	for (int i = 0; i < p->n; i++) {
		const entry *e = &p->e[i];
		switch (e->type) {
		case T_F: logm("  %s %s = %g", what, e->name, e->v.f); break;
		case T_D: logm("  %s %s = %g", what, e->name, e->v.d); break;
		case T_PTR: logm("  %s %s = %p", what, e->name, e->v.p); break;
		default: logm("  %s %s = %llu", what, e->name, (unsigned long long)as_u((entry *)e));
		}
	}
}
static res_t create(const char *fn, void *dev, void *cmd, int feature, const param *p, unsigned **out)
{
	logm("%s(dev %p, cmd %p, feature %d, params %p)", fn, dev, cmd, feature, p);
	dump("create", p);
	if (feature != 1) return FAIL | 1;	/* only NVSDK_NGX_Feature_SuperSampling */
	unsigned *h = malloc(sizeof *h); *h = next_handle++; *out = h;
	logm("  -> handle %u", *h);
	return OK;
}
API res_t NVSDK_NGX_VULKAN_CreateFeature(void *cmd, int feature, const param *p, unsigned **out)
{ return create("CreateFeature", NULL, cmd, feature, p, out); }
API res_t NVSDK_NGX_VULKAN_CreateFeature1(void *dev, void *cmd, int feature, const param *p, unsigned **out)
{ return create("CreateFeature1", dev, cmd, feature, p, out); }
API res_t NVSDK_NGX_VULKAN_ReleaseFeature(unsigned *h)
{ logm("ReleaseFeature(%u)", h ? *h : 0); free(h); return OK; }

/* NVSDK_NGX_Resource_VK, x64 layout (src/ngx-sdk/include/nvsdk_ngx_defs_vk.h). The union starts with
 * ImageViewInfo; Type @48 says which member is live (0 = IMAGEVIEW, 1 = BUFFER). */
typedef struct { uint32_t aspect, baseMip, levelCount, baseArray, layerCount; } vk_subres;
typedef struct {
	uint64_t  ImageView;	/* 0  */
	uint64_t  Image;	/* 8  */
	vk_subres Sub;		/* 16 */
	uint32_t  Format;	/* 36 */
	uint32_t  Width;	/* 40 */
	uint32_t  Height;	/* 44 */
	uint32_t  Type;		/* 48 */
	uint8_t   ReadWrite;	/* 52 */
} ngx_res_vk;		/* padded to 56 */

static void log_resource(const param *p, const char *name)
{
	entry *e = find((param *)p, name);
	if (!e || e->type != T_PTR || !e->v.p) { logm("  res %-14s (none)", name); return; }
	const ngx_res_vk *r = e->v.p;
	if (r->Type != 0) { logm("  res %-14s struct %p type %u (not an image view)", name, (void *)r, r->Type); return; }
	logm("  res %-14s struct %p  Image 0x%llx  ImageView 0x%llx  %ux%u  fmt %u  rw %u",
		name, (void *)r, (unsigned long long)r->Image, (unsigned long long)r->ImageView,
		r->Width, r->Height, r->Format, r->ReadWrite);
}
static void log_frame_inputs(const param *p)
{
	log_resource(p, "Color");
	log_resource(p, "Output");
	log_resource(p, "Depth");
	log_resource(p, "MotionVectors");
	entry *jx = find((param *)p, "Jitter.Offset.X"), *jy = find((param *)p, "Jitter.Offset.Y");
	entry *mx = find((param *)p, "MV.Scale.X"), *my = find((param *)p, "MV.Scale.Y");
	entry *rs = find((param *)p, "Reset"), *sh = find((param *)p, "Sharpness");
	logm("  frame jitter (%g,%g)  mvscale (%g,%g)  reset %lld  sharpness %g",
		jx ? as_d(jx) : 0.0, jy ? as_d(jy) : 0.0, mx ? as_d(mx) : 0.0, my ? as_d(my) : 0.0,
		rs ? (long long)as_u(rs) : 0, sh ? as_d(sh) : 0.0);
}

/* Hand the frame to the host Vulkan driver's MetalFX intercept. The Windows
 * DLL cannot reach the host command buffer directly, so it records
 * vkCmdPushConstants with a magic pipeline-layout value: winevulkan unwraps
 * cmd and passes layout and pValues through, and the host driver encodes
 * MTL4FXTemporalScaler instead of push constants (when MESA_KK_MFX=1). Keep
 * in sync with struct kk_mfx_info in KosmicKrisp's kk_cmd_metalfx.c. */
#define MFX_LAYOUT 0x4D564B4D46583100ULL	/* "MVKMFX1" */
#define MFX_MAGIC  0x3158464DU			/* "MFX1" */
typedef struct {
	uint32_t magic, size;
	uint64_t color, output, depth, motion;
	uint32_t inWidth, inHeight, outWidth, outHeight;
	float jitterX, jitterY, mvScaleX, mvScaleY;
	uint32_t reset, flags;
	float sharpness, preExposure;
} mfx_info;
_Static_assert(sizeof(mfx_info) == 88, "mfx_info must stay 88 bytes");
typedef void (*PFN_push)(void *cmd, uint64_t layout, uint32_t stages, uint32_t offset, uint32_t size, const void *data);

static PFN_push mfx_push(void)
{
	static int state;	/* 0 unknown, 1 on, 2 off */
	static PFN_push push;
	if (!state) {
		char v[8]; DWORD n = GetEnvironmentVariableA("NGXSTUB_MFX", v, sizeof v);
		HMODULE vk = GetModuleHandleA("vulkan-1.dll");
		if (!vk) vk = LoadLibraryA("vulkan-1.dll");
		push = (n == 1 && v[0] == '1' && vk) ? (PFN_push)GetProcAddress(vk, "vkCmdPushConstants") : NULL;
		state = push ? 1 : 2;
		logm("MetalFX trampoline %s (NGXSTUB_MFX %s, vulkan-1 %p, vkCmdPushConstants %p)",
			push ? "ON" : "off", n ? v : "unset", (void *)vk, (void *)push);
	}
	return push;
}

static const ngx_res_vk *res_of(const param *p, const char *name)
{
	entry *e = find((param *)p, name);
	const ngx_res_vk *r = e && e->type == T_PTR ? e->v.p : NULL;
	return r && r->Type == 0 ? r : NULL;
}
static double num(const param *p, const char *name, double dflt)
{
	entry *e = find((param *)p, name);
	return e ? as_d(e) : dflt;
}

static int mfx_record(void *cmd, const param *p)
{
	PFN_push push = mfx_push();
	const ngx_res_vk *c = res_of(p, "Color"), *o = res_of(p, "Output");
	const ngx_res_vk *d = res_of(p, "Depth"), *m = res_of(p, "MotionVectors");
	if (!push || !cmd || !c || !o) return 0;
	mfx_info i = { MFX_MAGIC, sizeof i };
	i.color = c->Image; i.output = o->Image;
	i.depth = d ? d->Image : 0; i.motion = m ? m->Image : 0;
	i.inWidth = (uint32_t)num(p, "DLSS.Render.Subrect.Dimensions.Width", c->Width);
	i.inHeight = (uint32_t)num(p, "DLSS.Render.Subrect.Dimensions.Height", c->Height);
	if (!i.inWidth) i.inWidth = c->Width;
	if (!i.inHeight) i.inHeight = c->Height;
	i.outWidth = o->Width; i.outHeight = o->Height;
	i.jitterX = (float)num(p, "Jitter.Offset.X", 0); i.jitterY = (float)num(p, "Jitter.Offset.Y", 0);
	i.mvScaleX = (float)num(p, "MV.Scale.X", 1); i.mvScaleY = (float)num(p, "MV.Scale.Y", 1);
	i.reset = (uint32_t)num(p, "Reset", 0);
	i.flags = (uint32_t)num(p, "DLSS.Feature.Create.Flags", 0);
	i.sharpness = (float)num(p, "Sharpness", 0); i.preExposure = (float)num(p, "DLSS.Pre.Exposure", 1);
	push(cmd, MFX_LAYOUT, 0, 0, sizeof i, &i);
	return 1;
}

API res_t NVSDK_NGX_VULKAN_EvaluateFeature(void *cmd, const unsigned *h, const param *p, void *cb)
{
	evals++;
	int mfx = mfx_record(cmd, p);
	if (evals <= 3 || evals % 600 == 0) {
		logm("EvaluateFeature #%u (cmd %p, handle %u)%s", evals, cmd, h ? *h : 0, mfx ? " -> MetalFX recorded" : "");
		if (evals <= 3) log_frame_inputs(p);
		if (evals <= 2) dump("eval", p);
	}
	return OK;
}
API res_t NVSDK_NGX_VULKAN_EvaluateFeature_C(void *cmd, const unsigned *h, const param *p, void *cb)
{ return NVSDK_NGX_VULKAN_EvaluateFeature(cmd, h, p, cb); }

typedef struct { int supported; unsigned min_arch; char min_os[255]; } feature_req;
API res_t NVSDK_NGX_VULKAN_GetFeatureRequirements(void *inst, void *pd, const void *info, feature_req *out)
{
	logm("GetFeatureRequirements"); if (out) { memset(out, 0, sizeof *out); strcpy(out->min_os, "10.0.0"); } return OK;
}
API res_t NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(const void *info, uint32_t *n, void **props)
{ logm("GetFeatureInstanceExtensionRequirements"); if (n) *n = 0; if (props) *props = NULL; return OK; }
API res_t NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(void *inst, void *pd, const void *info, uint32_t *n, void **props)
{ logm("GetFeatureDeviceExtensionRequirements"); if (n) *n = 0; if (props) *props = NULL; return OK; }
API res_t NVSDK_NGX_UpdateFeature(const void *app, int feature) { logm("UpdateFeature(%d)", feature); return OK; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
	if (why == DLL_PROCESS_ATTACH) { InitializeCriticalSection(&lk); logm("loaded (pid %lu)", GetCurrentProcessId()); }
	return TRUE;
}
