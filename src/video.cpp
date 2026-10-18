// Modo de vídeo do olho. O hook do Apply impõe o tamanho do olho, em janela, sem VSync, sem moldura e sem letterbox.
// O gravador do Video.scr recebe uma cópia ("sombra") com os valores do usuário, então o arquivo dele não muda. Só
// age com VR ativo ou com [stereo] fake_hmd=1.
#include "mod.h"
#include <d3d9.h>
#include <math.h>
#include <string.h>

namespace {
constexpr uintptr_t k_apply = 0x00484760, k_writer = 0x00483CF0;
constexpr uintptr_t k_d3dpp = 0x02E64C58, k_bb_format = 0x02E65FF4;

// Valores do usuário capturados no 1º Apply, antes de o mod sobrescrevê-los: o que o gravador gravaria sem o mod.
bool g_snap;
char g_snap_cur[0x1C];  // bloco atual (W, H, BPP, FSAA, Fullscreen, MaxRefresh, VSync) = pendente do 1º Apply
char g_snap_border[2];  // +0x6D4/+0x6D5 (moldura)
char g_snap_vp[8];      // +0x6F8/+0x6FC (RenderViewportProportion)
char g_shadow[0x700];

inline char* cgame() { return *(char**)game::cgame; }

void eye_size(int& w, int& h) {
	float s = setting("render_scale", 1);
	int ew = g_eye_w ? g_eye_w : (int)setting("fake_eye_w", 2528), eh = g_eye_h ? g_eye_h : (int)setting("fake_eye_h", 2704);
	w = ((int)ceilf(ew * s) + 15) & ~15;
	h = ((int)ceilf(eh * s) + 15) & ~15;
}

// Hook de char __stdcall Apply(VideoSettings*), chamado no boot (0x00484F04) e pelo menu (0x004752FC).
char __stdcall hk_apply(char* vs) {
	auto apply = (char(__stdcall*)(char*))k_apply;
	if (!video_eye_mode()) return apply(vs);
	int* d = (int*)vs;
	if (!g_snap) {
		g_snap = true;
		memcpy(g_snap_cur, vs + 0x1C, 0x1C);
		memcpy(g_snap_border, vs + 0x6D4, 2);
		memcpy(g_snap_vp, vs + 0x6F8, 8);
		logf("vídeo: usuário atual %dx%d bpp %d fs %d vsync %d | pendente %dx%d bpp %d fs %d refresh %d vsync %d | moldura"
		     " %d/%d viewport %d,%d", d[0], d[1], d[2], vs[0x10], vs[0x18], d[7], d[8], d[9], vs[0x2C], d[12], vs[0x34],
		     vs[0x6D4], vs[0x6D5], d[0x6F8 / 4], d[0x6FC / 4]);
		if (setting("force_argb", 1)) *(int*)k_bb_format = D3DFMT_A8R8G8B8;  // senão o DXVK dá X8R8G8B8 em janela
	}
	int w, h;
	eye_size(w, h);
	d[7] = w, d[8] = h;
	d[9] = d[2];         // BPP pendente = atual: blocos diferentes forçam um Reset espúrio
	vs[0x2C] = 0;        // janela: em tela cheia o modo seria encaixado num modo enumerado
	vs[0x34] = 0;        // sem VSync (PresentationInterval IMMEDIATE)
	vs[0x6D4] = vs[0x6D5] = 0;  // sem moldura
	d[0x6F8 / 4] = d[0x6FC / 4] = -1;  // sem letterbox: cena no backbuffer inteiro
	char ok = apply(vs);
	auto pp = (D3DPRESENT_PARAMETERS*)k_d3dpp;
	logf("vídeo: Apply %dx%d -> %d; D3DPP %ux%u formato %d janela %d intervalo %#x", w, h, ok, pp->BackBufferWidth,
	     pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed, pp->PresentationInterval);
	hook_reset();
	// Espelho: a janela fica do tamanho do olho; o DXVK escala o backbuffer para a janela (a WndProc ignora WM_SIZE).
	HWND hwnd = cgame() ? *(HWND*)(cgame() + 0x324) : NULL;
	int mh = (int)setting("mirror_height", 1080);
	if (ok && hwnd && mh > 0 && mh < h) SetWindowPos(hwnd, NULL, 0, 0, mh * w / h, mh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	focus_install(hwnd);  // subclasse da WndProc que mantém o foco da janela com VR
	return ok;
}

// O gravador do Video.scr (usercall: ECX = VideoSettings*, EAX = caminho) recebe a sombra com os valores do usuário.
extern "C" char* vs_shadow(char* live) {
	if (!g_snap) return live;
	memcpy(g_shadow, live, sizeof g_shadow);
	memcpy(g_shadow, g_snap_cur, sizeof g_snap_cur);
	memcpy(g_shadow + 0x6D4, g_snap_border, sizeof g_snap_border);
	memcpy(g_shadow + 0x6F8, g_snap_vp, sizeof g_snap_vp);
	logf("vídeo: Video.scr gravado com o modo do usuário (%dx%d)", *(int*)g_shadow, *(int*)(g_shadow + 4));
	return g_shadow;
}

extern "C" void vs_writer_thunk();
asm(".globl _vs_writer_thunk\n"
    "_vs_writer_thunk:\n"
    "  push %eax\n"  // caminho
    "  push %ecx\n"
    "  call _vs_shadow\n"
    "  add $4, %esp\n"
    "  mov %eax, %ecx\n"
    "  pop %eax\n"
    "  push $0x00483CF0\n"  // salta para o gravador; ele volta direto ao call site
    "  ret\n");

inline bool calls(uintptr_t site, uintptr_t target) {
	return *(uint8_t*)site == 0xE8 && site + 5 + *(int32_t*)(site + 1) == target;
}

void patch_call(uintptr_t site, void* to) {
	auto p = (uint8_t*)site;
	DWORD old;
	VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
	*(int32_t*)(p + 1) = (int32_t)((uintptr_t)to - (site + 5));
	VirtualProtect(p, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 5);
}
}  // namespace

bool video_eye_mode() { return g_vr || setting("fake_hmd", 0) != 0; }

void video_init() {
	if (!video_eye_mode()) return logf("vídeo: sem VR e sem fake_hmd; modo de vídeo do usuário");
	// Tudo ou nada: sem o gravador desviado, o Video.scr do usuário seria regravado com o modo do olho.
	static const uintptr_t applies[] = {0x00484F04, 0x004752FC}, writers[] = {0x00408D3B, 0x004752BA, 0x004752DC};
	for (uintptr_t a : applies)
		if (!calls(a, k_apply)) return logf("vídeo: call do Apply em %p não confere; modo do olho desligado", (void*)a);
	for (uintptr_t a : writers)
		if (!calls(a, k_writer)) return logf("vídeo: call do gravador em %p não confere; modo do olho desligado", (void*)a);
	for (uintptr_t a : applies) patch_call(a, (void*)hk_apply);
	for (uintptr_t a : writers) patch_call(a, (void*)vs_writer_thunk);
	int w, h;
	eye_size(w, h);
	logf("vídeo: modo do olho %dx%d (%s)", w, h, g_vr ? "HMD" : "fake_hmd");
}

// Leituras de validação para o relatório de 5 s: backbuffer, viewport 3D, faixa da UI e aspecto.
void video_report() {
	char* g = cgame();
	if (!video_eye_mode() || !g) return;
	auto pp = (D3DPRESENT_PARAMETERS*)k_d3dpp;
	int* vp = (int*)(g + 0x5C8);
	int* ui = (int*)(g + 0x5B8);
	logf("vídeo: BB %ux%u fmt %d janela %d intervalo %#x | viewport 3D %d %d %d %d | UI %d %d %d %d | aspecto %g",
	     pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->Windowed, pp->PresentationInterval, vp[0],
	     vp[1], vp[2], vp[3], ui[0], ui[1], ui[2], ui[3], *(float*)(g + 0x36C));
}
