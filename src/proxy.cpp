// Proxy d3d9.dll: o exe carrega "d3d9.dll" em RCreateRenderer (0x011AFEA0); esta DLL fica na pasta do jogo,
// encadeia o DXVK de cojvr\ e inicia o mod no primeiro Direct3DCreate9*.
#include "mod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HMODULE g_self;
char g_moddir[MAX_PATH];
static char g_ini[MAX_PATH];

int cfg_int(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_ini); }
void cfg_str(const char* sec, const char* key, const char* def, char* out, int n) { GetPrivateProfileStringA(sec, key, def, out, n, g_ini); }

float cfg_float(const char* sec, const char* key, float def) {
	char s[32];
	GetPrivateProfileStringA(sec, key, "", s, sizeof s, g_ini);
	return *s ? (float)atof(s) : def;
}

// Exportações do d3d9 do DXVK (src/d3d9/d3d9.def). Índices usados pelos stubs abaixo.
static const char* const k_names[] = {
	"Direct3DShaderValidatorCreate9", "PSGPError", "PSGPSampleTexture", "D3DPERF_BeginEvent", "D3DPERF_EndEvent",
	"D3DPERF_GetStatus", "D3DPERF_QueryRepeatFrame", "D3DPERF_SetMarker", "D3DPERF_SetOptions", "D3DPERF_SetRegion",
	"DebugSetLevel", "DebugSetMute", "Direct3D9EnableMaximizedWindowedModeShim", "Direct3DCreate9",
	"Direct3DCreate9Ex", "Direct3DCreate9On12", "Direct3DCreate9On12Ex", (const char*)16 /* ForceHybridEnumeration */};
constexpr int k_first_create = 13, k_last_create = 16, k_count = sizeof k_names / sizeof *k_names;

extern "C" {
void* real_fn[k_count];
void __stdcall resolve_fn(int i);
}

static void load_real() {
	char gamedir[MAX_PATH];
	GetModuleFileNameA(g_self, gamedir, MAX_PATH);
	*(strrchr(gamedir, '\\') + 1) = 0;
	snprintf(g_moddir, MAX_PATH, "%scojvr\\", gamedir);
	snprintf(g_ini, MAX_PATH, "%scojvr.ini", g_moddir);
	char path[MAX_PATH];
	snprintf(path, MAX_PATH, "%scojvr.log", g_moddir);
	log_open(path);
	logf("CoJ VR %s, proxy em %s", COJVR_VERSION, gamedir);

	// [env] do INI vale antes do DXVK carregar (ele lê DXVK_* na inicialização)
	static char env[8192];
	GetPrivateProfileSectionA("env", env, sizeof env, g_ini);
	for (char* p = env; *p; p += strlen(p) + 1) {
		char* eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;
		SetEnvironmentVariableA(p, eq + 1);
		logf("env %s=%s", p, eq + 1);
	}

	vr_init();
	// sem VSync no modo de vídeo do olho, mesmo se o DXVK ignorar o intervalo do D3DPP
	char cfg[4];
	if ((g_vr || cfg_int("stereo", "fake_hmd", 0)) && !GetEnvironmentVariableA("DXVK_CONFIG", cfg, sizeof cfg)) {
		SetEnvironmentVariableA("DXVK_CONFIG", "d3d9.presentInterval=0");
		logf("env DXVK_CONFIG=d3d9.presentInterval=0");
	}

	char rel[MAX_PATH];
	GetPrivateProfileStringA("dxvk", "path", "dxvk_d3d9.dll", rel, MAX_PATH, g_ini);
	snprintf(path, MAX_PATH, "%s%s", g_moddir, rel);
	HMODULE real = LoadLibraryA(path);
	if (!real) {
		logf("ERRO: LoadLibrary(%s) falhou, GetLastError %lu", path, GetLastError());
		MessageBoxA(NULL, path, "CoJ VR: nao consegui carregar o DXVK", MB_ICONERROR);
		ExitProcess(1);
	}
	logf("DXVK carregado: %s em %p", path, (void*)real);
	for (int i = 0; i < k_count; i++)
		if (!(real_fn[i] = (void*)GetProcAddress(real, k_names[i])))
			logf("aviso: exportação %d ausente no DXVK (chamar = crash)", i);  // ponytail: só avisa; o jogo não usa as ausentes
}

// ponytail: sem trava; a primeira chamada ao d3d9 vem da thread principal (RCreateRenderer)
void __stdcall resolve_fn(int i) {
	static bool loaded, inited;
	if (!loaded) {
		loaded = true;
		load_real();
	}
	if (i >= k_first_create && i <= k_last_create && !inited) {
		inited = true;
		mod_init();
	}
}

// Stubs genéricos: preservam a pilha da chamada (qualquer assinatura stdcall), resolvem e saltam para o DXVK.
#define STUB(i, name)                                                                                         \
	extern "C" __attribute__((naked)) void stub_##name() {                                                    \
		asm("pushal\n\tpushl $" #i "\n\tcall _resolve_fn@4\n\tpopal\n\tjmp *_real_fn+" #i "*4");            \
	}
STUB(0, Direct3DShaderValidatorCreate9)
STUB(1, PSGPError)
STUB(2, PSGPSampleTexture)
STUB(3, D3DPERF_BeginEvent)
STUB(4, D3DPERF_EndEvent)
STUB(5, D3DPERF_GetStatus)
STUB(6, D3DPERF_QueryRepeatFrame)
STUB(7, D3DPERF_SetMarker)
STUB(8, D3DPERF_SetOptions)
STUB(9, D3DPERF_SetRegion)
STUB(10, DebugSetLevel)
STUB(11, DebugSetMute)
STUB(12, Direct3D9EnableMaximizedWindowedModeShim)
STUB(13, Direct3DCreate9)
STUB(14, Direct3DCreate9Ex)
STUB(15, Direct3DCreate9On12)
STUB(16, Direct3DCreate9On12Ex)
STUB(17, Direct3D9ForceHybridEnumeration)

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = h;
		DisableThreadLibraryCalls(h);
	}
	return TRUE;
}
