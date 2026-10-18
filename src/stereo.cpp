// Estéreo: o hook do CLevel::vf162 renderiza as views do jogo duas vezes por frame, uma por olho, com a câmera e a
// projeção de cada olho. Aqui também ficam os hooks do frame_script (CLevel::vf161), do RenderTask e do lock do
// renderer (CRenderer::vf41), o FramePipelining desligado, os efeitos de tela desligados, sondas de diagnóstico e
// a tabela de configurações [stereo], ajustável em tempo real por SET no pad.txt.
#include "mod.h"
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- Configuração: [stereo] do INI, sobrescrita em tempo real por "SET <chave> <valor>" ---
namespace {
struct Setting {
	char key[32];
	float val;
};
Setting g_set[256];  // ponytail: tabela fixa; além de 256 chaves, as novas ficam no padrão (aviso no log)
int g_nset;
CRITICAL_SECTION g_set_cs;
INIT_ONCE g_set_once = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK init_set_cs(INIT_ONCE*, void*, void**) {
	InitializeCriticalSection(&g_set_cs);
	return TRUE;
}

Setting* find_setting(const char* key) {
	for (int i = 0; i < g_nset; i++)
		if (!strcmp(g_set[i].key, key)) return &g_set[i];
	return NULL;
}
}  // namespace

float setting(const char* key, float def) {
	InitOnceExecuteOnce(&g_set_once, init_set_cs, NULL, NULL);
	EnterCriticalSection(&g_set_cs);
	Setting* s = find_setting(key);
	if (!s && g_nset < 256) {  // primeiro uso da chave: lê o INI uma vez
		s = &g_set[g_nset++];
		snprintf(s->key, sizeof s->key, "%s", key);
		s->val = cfg_float("stereo", key, def);
	}
	float v = s ? s->val : def;
	static bool warned;
	if (!s && !warned) warned = true, logf("setting: tabela cheia; '%s' fica no padrão", key);
	LeaveCriticalSection(&g_set_cs);
	return v;
}

void setting_set(const char* key, float val) {
	setting(key, val);  // cria a entrada se ela ainda não existir
	EnterCriticalSection(&g_set_cs);
	if (Setting* s = find_setting(key)) s->val = val;
	LeaveCriticalSection(&g_set_cs);
	logf("SET %s = %g", key, val);
}

// --- Leituras do engine e estado dos hooks ---
namespace {
inline LONGLONG qpc() {
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}
inline unsigned present_count() { return *(volatile unsigned*)game::present_counter; }
inline char* cgame() { return *(char**)game::cgame; }
inline const char* level_name(char* lvl) {
	const char* n = *(const char**)(lvl + 0x2C);
	return n ? n : "?";
}
// Nível de gameplay. Os recursos (.rls) falam em "LawmanModule", mas na build de PC o CLevel do jogo se chama "LevelDI".
inline bool is_game_level(char* lvl) { return !_stricmp(level_name(lvl), "LevelDI"); }

// Diagnóstico: registra no log cada nome de nível novo visto por um hook (até 16 por hook).
void note_level(const char* hook, const char** seen, char* lvl) {
	const char* n = level_name(lvl);
	for (int i = 0; i < 16; i++) {
		if (seen[i] && !strcmp(seen[i], n)) return;
		if (!seen[i]) {
			seen[i] = n;
			logf("%s: nível novo '%s' (%p, vtable %p) +0x3FC=%d", hook, n, (void*)lvl, *(void**)lvl, *(int*)(lvl + 0x3FC));
			return;
		}
	}
}

volatile DWORD g_tid_script, g_tid_eyes;
volatile unsigned g_game_pc = ~0u;  // contador de Present do último vf161 do nível do jogo (frame de gameplay)
volatile unsigned g_eyes_present = ~0u;  // contador de Present do último vf162 do LevelDI
volatile LONG g_dump_views = 1;        // pedir o dump das views no próximo vf162 do LevelDI
LONG g_eyes_calls, g_eyes_double, g_eyes_badret, g_tid_frames, g_tid_same;
void* g_rets[8];
int g_nrets;

// Diagnóstico: lista de views da cópia de render (TLS do engine = 2 dentro do RenderTask) e campos da câmera do
// GameView.
void dump_views(char* lvl) {
	auto head = (char*(__stdcall*)(char*, char))game::view_list_head;
	int i = 0;
	char* first_enabled = NULL;
	for (char* v = head(lvl + 0x600, 0); v && i < 16; v = *(char**)(v + 0x10), i++) {
		if (v[0x24] && !first_enabled) first_enabled = v;
		logf("  view %d %p '%.32s' habilitada=%d +0x2FA=%d", i, (void*)v, v + 0x270, v[0x24], v[0x2FA]);
		if (strcmp(v + 0x270, "GameView")) continue;
		char* c = v + 0x38;  // CRendererCamera
		float* f = (float*)c;
		logf("  GameView cam: pos %.1f %.1f %.1f | L %.3f R %.3f B %.3f T %.3f N %.3f F %.1f | bias %g | +10C %g %g %g |"
		     " +220 %d %d %d | LOD %g %g %g %g | 1ª habilitada=%d",
		     f[0x40 / 4], f[0x50 / 4], f[0x60 / 4], f[0xE4 / 4], f[0xE8 / 4], f[0xEC / 4], f[0xF0 / 4], f[0xF4 / 4],
		     f[0xF8 / 4], f[0xFC / 4], f[0x10C / 4], f[0x110 / 4], f[0x114 / 4], c[0x220], c[0x221], c[0x222],
		     *(float*)(v + 0x25C), *(float*)(v + 0x260), *(float*)(v + 0x264), *(float*)(v + 0x268),
		     first_enabled == v);
	}
	char* nv = *(char**)game::nv_stereo;
	logf("  congelar culling=%d, NV stereo %p%s", *(char*)game::cull_freeze, (void*)nv,
	     nv && *(int*)(nv + 0x10) ? " (ATIVO!)" : "");
}

// --- Estado por olho e sondas ---
volatile LONG g_eye = 2;  // olho do vf162 em curso (0 = esquerdo, 1 = direito, 2 = fora do laço); lido pelos jobs de FX
LONG g_dynvb[2];          // pico de CRenderer+0x45C (VB dinâmico) depois de cada olho na janela

// Bloco "frame anterior" do CRenderer: View, VP e Proj em +0x10..+0xBF, posição e direção da câmera em
// +0x100..+0x117, gravados pelo engine depois do desenho. É salvo e restaurado por olho; sem isso cada olho veria o
// "frame anterior" do outro. O +0xC0..+0xFF (SCATTERING) não depende do olho e fica de fora.
struct Prev {
	char a[0xB0], b[0x18];
	bool ok;
} g_prev[2];
unsigned g_loop_pc = ~0u;  // contador de Present do último laço de olhos

// Ponteiro para uma variável do var-set do nível (CLevel::vf146 = *(CLevel+0x90)) no buffer do contexto da thread:
// frame (1) na thread principal fora do RenderTask, render (2) dentro dele. O offset é buscado uma vez na tabela de
// descritores do CVarMgr (varmgr+0x128, ordenada; entrada = {?, offset, nome}) e fica em cache. name tem de ser um
// literal: o cache compara o ponteiro, não o texto.
float* level_var(char* lvl, const char* name) {
	static struct {
		const char* name;
		int off;
	} cache[32];
	static int n;
	char* vm = *(char**)0x02E658D4;
	char* vs = *(char**)(lvl + 0x90);
	if (!vm || !vs) return NULL;
	int i = 0;
	while (i < n && cache[i].name != name) i++;
	if (i == n) {
		if (n == 32) return NULL;
		cache[n].name = name, cache[n].off = -1, n++;
		auto cs = (CRITICAL_SECTION*)(vm + 0x1D8);
		EnterCriticalSection(cs);
		char** arr = *(char***)(vm + 0x128);
		for (int k = 0, cnt = *(int*)(vm + 0x12C); k < cnt; k++)
			if (!strcmp(arr[k] + 8, name)) cache[i].off = *(int*)(arr[k] + 4);
		LeaveCriticalSection(cs);
		logf("var %s: offset %d", name, cache[i].off);
	}
	int off = cache[i].off;
	if (off < 0) return NULL;
	auto buf = ((int*(__stdcall*)(char*))0x007EEB60)(vs);  // buffer da thread: {dados, tamanho}
	return (unsigned)off + 4 <= (unsigned)buf[1] ? (float*)(buf[0] + off) : NULL;
}

// f_pp_sun_glow_on: o render_script multiplica o valor a cada vf152 e nada o restaura entre os dois olhos; sem
// regravar antes de cada olho, o brilho do sol sairia mais fraco e diferente no olho direito.
inline float* sun_glow(char* lvl) { return level_var(lvl, "f_pp_sun_glow_on"); }

// Efeitos de tela zerados no buffer de frame logo depois do frame_script; o render herda os valores no sync.
// PRV, heat e shoots usam render targets persistentes e levariam a imagem de um olho para o outro; os demais saem por
// conforto em VR. "SET effects 0" devolve os efeitos ao jogo.
void effects_off(char* lvl) {
	if (!setting("effects", 1)) return;
	static const char* const k_vars[] = {"f_pp_prv_copy", "f_pp_prv_lrp", "f_pp_prv_mul", "f_pp_prv_add", "f_pp_heat",
	                                     "f_player_shoot_opacity", "f_pp_blur_on", "f_pp_blur_camera_motion_factor",
	                                     "f_pp_shake_on", "f_pp_aberration_on", "f_pp_blur_radial_on", "f_pp_noise",
	                                     "f_hud_frame_on"};
	static const char* const k_hit[] = {"f_pp_game_player_hit_on", "f_pp_overlays"};
	for (const char* n : k_vars)
		if (float* v = level_var(lvl, n)) *v = 0;
	if (video_eye_mode())  // garante a cena direto no backbuffer (fora do modo do olho o letterbox precisa do scaling)
		if (float* v = level_var(lvl, "f_pp_screen_scaling_on")) *v = 0;
	if (!setting("hit_fx", 0))  // malhas 2D do efeito de dano, sem disparidade entre os olhos
		for (const char* n : k_hit)
			if (float* v = level_var(lvl, n)) *v = 0;
	if (!setting("sun_glow", 1))
		if (float* v = sun_glow(lvl)) *v = 0;
}

char* find_game_view(char* lvl) {
	auto head = (char*(__stdcall*)(char*, char))game::view_list_head;
	for (char* v = head(lvl + 0x600, 0); v; v = *(char**)(v + 0x10))
		if (!strcmp(v + 0x270, "GameView")) return v;
	return NULL;
}

// Sonda do tick de FX (FUN_004DA630), no call site único 0x004DA73B. Os 10 bytes a partir de 0x004DA733
// ("mov ecx,[ebx+10]; mov eax,[ecx]; mov edx,[eax+24]; call edx") viram "call fx_thunk" + 5 NOPs. O vf9 do FX é
// thiscall sem argumentos de pilha, então o thunk refaz as cargas, chama o original e mede depois. Roda nos jobs de
// FX, fora da thread principal: por isso os contadores são atômicos.
struct VtCount {
	void* vt;
	LONG n[2];
} g_vtc[128];
LONG g_fx_calls[2], g_em_calls[2], g_em_sim[2];
LONG64 g_em_dt_us[2];  // soma do avanço do +0x4EC (µs) nas chamadas que simularam

// vf17 (simulação) dos emissores: slot 17 das 7 vtables que herdam o do CFXEmiter; o vf9 só remonta as partículas
// (vf18/vf19) quando ele devolve != 0. Por padrão o olho direito não simula e desenha o estado deixado pelo
// esquerdo, e o tempo total simulado fica igual ao do mono. Sem isso cerca de 35 % das simulações caíam no olho
// direito e as partículas ficavam diferentes entre os olhos. "SET fx_sim_d 1" volta ao comportamento original.
typedef uint32_t(__thiscall* vf17_t)(char*);
constexpr uintptr_t k_vf17 = 0x006A8DA0;
vf17_t o_vf17 = (vf17_t)k_vf17;
volatile LONG g_fx_sim_d;  // cópia por frame do setting (os jobs de FX não leem o setting)

uint32_t __fastcall hk_vf17(char* fx, void*) { return g_eye == 1 && !g_fx_sim_d ? 0 : o_vf17(fx); }

inline bool is_emitter(char* fx) {
	void* f = (*(void***)fx)[17];
	return f == (void*)k_vf17 || f == (void*)hk_vf17;
}

extern "C" uint32_t fx_pre(char* fx) {
	int eye = g_eye;
	if (eye > 1) return 0;
	void* vt = *(void**)fx;
	for (unsigned i = ((uintptr_t)vt >> 3) % 128, k = 0; k < 128; i = (i + 1) % 128, k++) {
		void* cur = g_vtc[i].vt;
		if (!cur) cur = InterlockedCompareExchangePointer(&g_vtc[i].vt, vt, NULL);
		if (!cur || cur == vt) {
			InterlockedIncrement(&g_vtc[i].n[eye]);
			break;
		}
	}
	InterlockedIncrement(&g_fx_calls[eye]);
	return is_emitter(fx) ? *(uint32_t*)(fx + 0x4EC) : 0;  // tempo da última simulação (bits do float)
}

extern "C" void fx_post(uint32_t before, char* fx) {
	int eye = g_eye;
	if (eye > 1 || !is_emitter(fx)) return;
	InterlockedIncrement(&g_em_calls[eye]);
	float b, a = *(float*)(fx + 0x4EC);
	memcpy(&b, &before, 4);
	if (a == b) return;
	InterlockedIncrement(&g_em_sim[eye]);
	if (a > b) InterlockedExchangeAdd64(&g_em_dt_us[eye], (LONG64)((a - b) * 1e6f));
}

extern "C" void fx_thunk();
asm(".globl _fx_thunk\n"
    "_fx_thunk:\n"
    "  mov 0x10(%ebx), %ecx\n"
    "  push %ecx\n"  // fica como 2º argumento do fx_post (fx)
    "  push %ecx\n"  // argumento do fx_pre
    "  call _fx_pre\n"
    "  mov %eax, (%esp)\n"  // troca pelo retorno do fx_pre: 1º argumento do fx_post (valor antes)
    "  mov 4(%esp), %ecx\n"
    "  mov (%ecx), %eax\n"
    "  call *0x24(%eax)\n"  // vf9 original, com ECX = fx
    "  call _fx_post\n"  // fx_post(antes, fx)
    "  add $8, %esp\n"
    "  ret\n");

// Sonda do Lock do VB de instâncias do CSimpleObjects (a chamada que retorna em 0x004B4156). O engine trava esse VB
// sem D3DLOCK_DISCARD; com dois olhos ele é reusado mais cedo e o DXVK pode ter de esperar a GPU, então o tempo é
// medido por olho. O hook troca o slot 11 (Lock) da vtable de vertex buffer do DXVK, obtida criando um VB descartável
// dentro do hook do vf162 (na thread que desenha).
typedef HRESULT(__stdcall* vblock_t)(void*, UINT, UINT, void**, DWORD);
vblock_t o_vblock;

HRESULT __stdcall hk_vblock(void* vb, UINT off, UINT size, void** pp, DWORD flags) {
	if (__builtin_return_address(0) != (void*)0x004B4156) return o_vblock(vb, off, size, pp, flags);
	static LONG logged;
	if (InterlockedIncrement(&logged) <= 3) logf("estéreo: Lock do VB de instâncias: off %u size %u flags %#lx", off, size, flags);
	LONGLONG t0 = qpc();
	HRESULT hr = o_vblock(vb, off, size, pp, flags);
	int eye = g_eye;
	stats_add(eye == 0 ? "vblock.E" : eye == 1 ? "vblock.D" : "vblock.mono", qpc_ms(qpc() - t0));
	return hr;
}

void hook_vblock() {
	auto dev = *(IDirect3DDevice9**)game::device;
	IDirect3DVertexBuffer9* vb = NULL;
	if (!dev || FAILED(dev->CreateVertexBuffer(64, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL)))
		return logf("estéreo: VB descartável falhou; sem sonda do Lock");
	void** vt = *(void***)vb;
	o_vblock = (vblock_t)patch_ptr((uintptr_t)&vt[11], (void*)hk_vblock);
	vb->Release();
	logf("estéreo: Lock do VB (vtable %p, orig %p) enganchado", (void*)vt, (void*)o_vblock);
}

inline bool shot_pending() { return g_shot_request == 1; }

// Filtro do estéreo, além de ser o nível do jogo: estéreo ligado e fora da TELA forçada (mode 2), fora do editor
// (CGame+0x98), frame_script fora do vf162 (CLevel+0x3FC != 0; senão ele rodaria duas vezes), fora da thread de
// loading, um laço de olhos por Present, chamado de um dos dois call sites do gameplay (retornos 0x0040AF07, em
// FUN_0040AE70, e 0x0040AFE7, em FUN_0040AF60), com WaitGetPoses neste frame e com a CENA liberada.
bool stereo_ok(char* lvl, void* ret) {
	char* g = cgame();
	HANDLE ld = *(HANDLE*)game::loading_thread;
	return setting("stereo", 1) != 0 && setting("mode", 0) != 2 && g && *(int*)(g + 0x98) == 0 &&
	       *(int*)(lvl + 0x3FC) != 0 && !(ld && GetThreadId(ld) == GetCurrentThreadId()) &&
	       present_count() != g_loop_pc && (ret == (void*)0x0040AF07 || ret == (void*)0x0040AFE7) && vr_eyes_allowed() &&
	       player_scene();
}

// Transformação olho -> cabeça (3x4, metros) do HMD; sem HMD, uma sintética a partir de fake_ipd_mm.
void eye_to_head(int eye, float E[12]) {
	if (vr_eye_to_head(eye, E)) return;
	static const float I[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
	memcpy(E, I, sizeof I);
	E[3] = (eye ? 1 : -1) * setting("fake_ipd_mm", 64) / 2000;  // sem HMD: olho esquerdo em x negativo (mm -> m, metade do IPD)
}

// Tangentes l, r, b, t do frustum do olho no plano z = -1 (do HMD ou, sem HMD, de fake_fov_y e do aspecto W/H).
void eye_tangents(int eye, float T[4]) {
	if (vr_eye_tangents(eye, T[0], T[1], T[2], T[3])) return;
	auto pp = (D3DPRESENT_PARAMETERS*)0x02E64C58;  // D3DPRESENT_PARAMETERS do engine
	float t = tanf(setting("fake_fov_y", 96) * 0.5f * 3.14159265f / 180), r = t * pp->BackBufferWidth / pp->BackBufferHeight;
	T[0] = -r, T[1] = r, T[2] = -t, T[3] = t;
}

// Pose do olho a partir do centro C (mundo da câmera, 3x4 row-major: col0 direita, col1 cima, col2 trás, col3
// posição em cm) e de E = olho -> cabeça (metros): R = Rc·Re, p = pc + k·Rc·te, com k = 100·ipd_scale (m -> cm).
// Escreve o mundo (+0x34) e a view (+0x04, inversa rígida) da CRendererCamera e chama UpdateProjection (P, VP,
// cantos, planos). T = NULL mantém a projeção do jogo; senão grava L, R, B, T no plano near (N = near_cm) em
// +0xE4..+0xF4. Far (+0xF8), InfiniteFar (+0x220), DepthBias e o offset de clip ficam como o jogo deixou.
void write_eye_cam(char* cam, const float* C, const float* E, float k, const float* T) {
	if (T) {
		float n = setting("near_cm", 5), *f = (float*)(cam + 0xE4);
		f[0] = n * T[0], f[1] = n * T[1], f[2] = n * T[2], f[3] = n * T[3], f[4] = n;
	}
	float W[12], V[12];
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			float v = 0;
			for (int j = 0; j < 3; j++) v += C[r * 4 + j] * E[j * 4 + c];
			W[r * 4 + c] = c < 3 ? v : C[r * 4 + 3] + k * v;
		}
	}
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) V[r * 4 + c] = W[c * 4 + r];
		V[r * 4 + 3] = -(W[r] * W[3] + W[4 + r] * W[7] + W[8 + r] * W[11]);
	}
	memcpy(cam + 0x34, W, sizeof W);
	memcpy(cam + 0x04, V, sizeof V);
	((void(__stdcall*)(char*))0x00808C10)(cam);  // CCamera::UpdateProjection
}

// --- Hook do CLevel::vf162 (render das views) ---
typedef void(__thiscall* vf162_t)(char*);
vf162_t o_vf162;

// Laço de olhos: chama o vf162 original uma vez por olho, com a câmera de cada olho, e copia o resultado para a
// textura do olho. Antes de cada olho restaura o "frame anterior" daquele olho e o sun glow e zera o "já renderizada"
// do GameView (+0x2FA), senão o segundo vf162 pularia a view.
void eye_loop(char* lvl, char* gv) {
	char* r = *(char**)game::crenderer;
	unsigned pc = present_count();
	if (pc != g_loop_pc + 1) g_prev[0].ok = g_prev[1].ok = false;  // houve frame mono: o "frame anterior" salvo não vale
	g_loop_pc = pc;
	static bool vb_tried;
	if (!vb_tried) vb_tried = true, hook_vblock();
	g_fx_sim_d = setting("fx_sim_d", 0) != 0;
	float* glow = sun_glow(lvl);
	float glow0 = glow ? *glow : 0;
	static int nfx;
	if (nfx < 3 || shot_pending()) {
		nfx++;
		float *blur = level_var(lvl, "f_pp_blur_on"), *prv = level_var(lvl, "f_pp_prv_lrp"),
		      *shoot = level_var(lvl, "f_player_shoot_opacity"), *frame = level_var(lvl, "f_hud_frame_on");
		logf("estéreo: no render blur %g prv_lrp %g shoot %g moldura %g (effects %g)", blur ? *blur : -1, prv ? *prv : -1,
		     shoot ? *shoot : -1, frame ? *frame : -1, setting("effects", 1));
	}
	static int nlog;
	float after[2] = {0, 0};
	auto dev = *(IDirect3DDevice9**)game::device;
	IDirect3DSurface9* out[2] = {NULL, NULL};
	bool shot = g_shot_request == 1 && InterlockedCompareExchange(&g_shot_request, 0, 1) == 1;
	if (shot && setting("duel_probe", 0)) weapon_probe();  // diagnóstico: estado da arma no coldre do duelo no frame do SHOT
	char* cam = gv + 0x38;  // CRendererCamera da cópia de render (recriada a cada sync: não restaurar ao sair)
	float C[12], E[2][12], k = 100 * setting("ipd_scale", 1), H[12];
	memcpy(C, cam + 0x34, sizeof C);
	if (head_render(H)) memcpy(C, H, sizeof C);  // late latch: pose de render do WaitGetPoses sobre a âncora do update
	static LONGLONG e_at;
	static float e_cache[2][12], t_cache[2][4];
	if (qpc_ms(qpc() - e_at) > 1000) {  // olho -> cabeça e frustum mudam pouco: relidos uma vez por segundo
		e_at = qpc();
		for (int i = 0; i < 2; i++) eye_to_head(i, e_cache[i]), eye_tangents(i, t_cache[i]);
		auto pp = (D3DPRESENT_PARAMETERS*)0x02E64C58;
		static int warned;
		for (int i = 0; i < 2 && warned < 4; i++) {  // confere o frustum: tangentes em ordem e aspecto do backbuffer (2 %)
			float* t = t_cache[i];
			float asp = (t[1] - t[0]) / (t[3] - t[2]), want = (float)pp->BackBufferWidth / pp->BackBufferHeight;
			if (t[1] <= t[0] || t[3] <= t[2] || fabsf(asp / want - 1) > 0.02f)
				warned++, logf("estéreo: frustum do olho %d suspeito: l %g r %g b %g t %g, aspecto %g (olho %g)", i, t[0], t[1],
				               t[2], t[3], asp, want);
		}
	}
	memcpy(E, e_cache, sizeof E);
	bool hmd_proj = setting("hmd_proj", 1) != 0;
	static int nproj;
	if (nproj < 1 || (shot && nproj < 8)) {
		nproj++;
		float* f = (float*)(cam + 0xE4);
		logf("estéreo: projeção do jogo L %g R %g B %g T %g N %g F %g; do olho E l %g r %g b %g t %g x near %g (hmd_proj %d)",
		     f[0], f[1], f[2], f[3], f[4], f[5], t_cache[0][0], t_cache[0][1], t_cache[0][2], t_cache[0][3],
		     setting("near_cm", 5), hmd_proj);
	}
	if (setting("lod_force", 1))
		for (int i = 0; i < 4; i++) *(float*)(gv + 0x25C + 4 * i) = 1;  // LOD s, s², 1/s, 1/s² = 1
	static int npose;
	for (int eye = 0; eye < 2; eye++) {
		Prev& p = g_prev[eye];
		if (p.ok) memcpy(r + 0x10, p.a, sizeof p.a), memcpy(r + 0x100, p.b, sizeof p.b);
		if (glow && setting("glow_restore", 1)) *glow = glow0;
		gv[0x2FA] = 0;
		write_eye_cam(cam, C, E[eye], k, hmd_proj ? t_cache[eye] : NULL);
		if (npose < 2 || shot) {
			npose += eye;
			float* W = (float*)(cam + 0x34);
			float d[3] = {W[3] - C[3], W[7] - C[7], W[11] - C[11]};
			logf("estéreo: olho %d p-pc (%.2f %.2f %.2f) cm, na direita %.2f cima %.2f trás %.2f; te (%.4f %.4f %.4f) m", eye,
			     d[0], d[1], d[2], d[0] * C[0] + d[1] * C[4] + d[2] * C[8], d[0] * C[1] + d[1] * C[5] + d[2] * C[9],
			     d[0] * C[2] + d[1] * C[6] + d[2] * C[10], E[eye][3], E[eye][7], E[eye][11]);
		}
		g_eye = eye;
		LONGLONG t0 = qpc();
		o_vf162(lvl);
		stats_add(eye ? "eyeD" : "eyeE", qpc_ms(qpc() - t0));
		// Copia o RT0 ao sair do vf162: sem letterbox ele é o backbuffer; com letterbox (fora do modo do olho) é um RT
		// intermediário, e o backbuffer ainda tem o frame anterior.
		IDirect3DSurface9* rt;
		D3DSURFACE_DESC d;
		if (SUCCEEDED(dev->GetRenderTarget(0, &rt))) {
			rt->GetDesc(&d);
			if ((out[eye] = vr_eye_surface(eye, d.Width, d.Height))) dev->StretchRect(rt, NULL, out[eye], NULL, D3DTEXF_NONE);
			rt->Release();
		}
		memcpy(p.a, r + 0x10, sizeof p.a), memcpy(p.b, r + 0x100, sizeof p.b), p.ok = true;
		LONG vb = *(LONG*)(r + 0x45C);
		if (vb > g_dynvb[eye]) g_dynvb[eye] = vb;
		if (glow) after[eye] = *glow;
	}
	g_eye = 2;
	for (int eye = 0; eye < 2 && shot; eye++)
		if (out[eye]) dump_surface(out[eye], eye ? "_D" : "_E");
	if (shot) g_shot_request = 2;  // o Present grava o backbuffer com o mesmo N
	if (glow && (nlog++ < 3 || shot)) logf("estéreo: sun_glow base %g, depois do E %g, depois do D %g", glow0, after[0], after[1]);
}

void __fastcall hk_vf162(char* lvl, void*) {
	void* ret = __builtin_return_address(0);
	bool gl = is_game_level(lvl);
	static const char* seen[16];
	note_level("olhos", seen, lvl);
	bool known = false;
	for (int i = 0; i < g_nrets; i++) known |= g_rets[i] == ret;
	if (!known && g_nrets < 8) {
		g_rets[g_nrets++] = ret;
		logf("olhos: novo retorno %p (nível '%s')", ret, level_name(lvl));
	}
	if (gl) {
		LONG n = InterlockedIncrement(&g_eyes_calls);
		if (n <= 600)
			logf("olhos %ld ret %p +0x3FC=%d frame %u tid %lu", n, ret, *(int*)(lvl + 0x3FC),
			     cgame() ? *(unsigned*)(cgame() + 0xAC) : 0, GetCurrentThreadId());
		if (ret != (void*)0x0040AF07 && ret != (void*)0x0040AFE7) InterlockedIncrement(&g_eyes_badret);  // fora dos call sites esperados
		unsigned pc = present_count();
		if (pc == g_eyes_present) InterlockedIncrement(&g_eyes_double);
		g_eyes_present = pc;
		g_tid_eyes = GetCurrentThreadId();
		if (InterlockedExchange(&g_dump_views, 0)) dump_views(lvl);
		char* gv;
		if (stereo_ok(lvl, ret) && (gv = find_game_view(lvl))) return eye_loop(lvl, gv);
	}
	LONGLONG t0 = qpc();
	if (gl) g_eye = 0;  // mono no nível do jogo conta como olho esquerdo: linha de base das sondas com "SET stereo 0"
	o_vf162(lvl);
	g_eye = 2;
	if (gl) stats_add("vf162", qpc_ms(qpc() - t0));
}

// --- Hook do CLevel::vf161 (frame_script) ---
typedef void(__thiscall* vf161_t)(char*);
vf161_t o_vf161;

void __fastcall hk_vf161(char* lvl, void*) {
	o_vf161(lvl);
	static const char* seen[16];
	note_level("script", seen, lvl);
	if (!is_game_level(lvl)) return;
	g_tid_script = GetCurrentThreadId();
	g_game_pc = present_count();
	player_update_end(lvl);
	// FramePipelining 0 faz o RenderTask rodar inline na thread principal, e assim o vf161, o vf162 e o Present
	// rodam na mesma thread. Grava na primeira vez e a cada "SET frame_pipelining" (-1 = valores originais do jogo);
	// só fora do editor (CGame+0x98 == 0), como exige o handler de console do engine.
	static int applied = -2;
	static char orig[2];
	char* g = cgame();
	int fp = (int)setting("frame_pipelining", 0);
	if (fp != applied && g && *(int*)(g + 0x98) == 0) {
		if (applied == -2) orig[0] = g[0x9A4], orig[1] = g[0x9A5];
		applied = fp;
		logf("FramePipelining: config %d, efetivo %d -> %s (thread principal %lu)", g[0x9A4], g[0x9A5],
		     fp < 0 ? "original" : fp ? "1" : "0", g_tid_script);
		g[0x9A4] = fp < 0 ? orig[0] : (char)fp;
		g[0x9A5] = fp < 0 ? orig[1] : (char)fp;
	}
	effects_off(lvl);
}

// --- RenderTask (cdecl, agendado pelo push em 0x0040BD32; o hook troca o imm32 desse push) ---
typedef void(__cdecl* task_t)(void*);
task_t o_render_task;

void __cdecl hk_render_task(void* ctx) {
	bool game_frame = g_game_pc == present_count();
	vr_render_task_pre(game_frame);
	LONGLONG t0 = qpc();
	o_render_task(ctx);
	stats_add("RenderTask", qpc_ms(qpc() - t0));
	vr_render_task_post(game_frame);
}

// --- vf41: lock do renderer (thiscall, 1 arg: nome de quem pede) ---
typedef void(__thiscall* vf41_t)(void*, const char*);
vf41_t o_vf41;

void __fastcall hk_vf41(void* r, void*, const char* who) {
	LONGLONG t0 = qpc();
	o_vf41(r, who);
	double ms = qpc_ms(qpc() - t0);
	DWORD tid = GetCurrentThreadId();
	static HANDLE last_h;
	static DWORD loading_tid;
	HANDLE h = *(HANDLE*)game::loading_thread;
	if (h != last_h) last_h = h, loading_tid = h ? GetThreadId(h) : 0;
	const char* cls = tid == g_tid_script ? "vf41.principal" : tid == loading_tid ? "vf41.loading" : "vf41.outras";
	stats_add(cls, ms);
	static LONG slow;
	if (ms > 1.0 && InterlockedIncrement(&slow) <= 20) logf("vf41: %.2f ms esperando (%s, '%s')", ms, cls, who ? who : "?");
}

void patch_code32(uintptr_t addr, uint32_t val) {
	DWORD old;
	VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &old);
	*(volatile uint32_t*)addr = val;
	VirtualProtect((void*)addr, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)addr, 4);
}
}  // namespace

// Chamado no começo do Present do engine: conta os frames de gameplay em que o Present, o vf162 e o vf161 rodaram
// na mesma thread (sai no relatório).
void stereo_present() {
	if (g_eyes_present != present_count()) return;  // frame sem vf162 do LevelDI
	InterlockedIncrement(&g_tid_frames);
	DWORD tid = GetCurrentThreadId();
	if (tid == g_tid_eyes && tid == g_tid_script) InterlockedIncrement(&g_tid_same);
}

bool stereo_frame() { return g_loop_pc == present_count(); }

void stereo_report() {
	char* r = *(char**)game::crenderer;
	logf("estéreo: frames de gameplay %ld, Present, olhos e script na mesma thread em %ld; vf162 2x no mesmo Present %ld; retorno inesperado %ld;"
	     " CRenderer+0x178=%d",
	     InterlockedExchange(&g_tid_frames, 0), InterlockedExchange(&g_tid_same, 0), InterlockedExchange(&g_eyes_double, 0),
	     InterlockedExchange(&g_eyes_badret, 0), r ? r[0x178] : -1);
	g_dump_views = 1;
	// Tick de FX por olho. O esperado é a mesma contagem por vtable nos dois olhos e emissores sem simular no direito.
	if (!g_fx_calls[0] && !g_fx_calls[1]) return;
	int nvt = 0, diff = 0;
	for (auto& v : g_vtc) {
		if (!v.vt) continue;
		LONG e = InterlockedExchange(&v.n[0], 0), d = InterlockedExchange(&v.n[1], 0);
		nvt += e || d;
		if (e != d && diff++ < 4) logf("  partículas vtable %p: E %ld D %ld", v.vt, e, d);
	}
	LONG sim[2] = {InterlockedExchange(&g_em_sim[0], 0), InterlockedExchange(&g_em_sim[1], 0)};
	LONG64 dt[2] = {InterlockedExchange64(&g_em_dt_us[0], 0), InterlockedExchange64(&g_em_dt_us[1], 0)};
	logf("partículas: chamadas E %ld D %ld, vtables %d (contagem diferente em %d); emissores E %ld (simularam %ld, dt médio %.0f µs,"
	     " total %.1f s) D %ld (simularam %ld, dt médio %.0f µs, total %.1f s); VB dinâmico máx. depois do E %ld, do D %ld",
	     InterlockedExchange(&g_fx_calls[0], 0), InterlockedExchange(&g_fx_calls[1], 0), nvt, diff,
	     InterlockedExchange(&g_em_calls[0], 0), sim[0], sim[0] ? (double)dt[0] / sim[0] : 0.0, dt[0] / 1e6,
	     InterlockedExchange(&g_em_calls[1], 0), sim[1], sim[1] ? (double)dt[1] / sim[1] : 0.0, dt[1] / 1e6,
	     InterlockedExchange(&g_dynvb[0], 0), InterlockedExchange(&g_dynvb[1], 0));
}

void stereo_init() {
	if (*(uint32_t*)game::render_task_imm != game::render_task)
		return logf("estéreo: push do RenderTask não confere em %p; hooks do estéreo desligados", (void*)game::render_task_imm);
	o_vf162 = (vf162_t)patch_ptr(game::CLevel_vf162_slot, (void*)hk_vf162);
	o_vf161 = (vf161_t)patch_ptr(game::CLevel_vf161_slot, (void*)hk_vf161);
	o_vf41 = (vf41_t)patch_ptr(game::CRenderer_vf41_slot, (void*)hk_vf41);
	o_render_task = (task_t)game::render_task;
	patch_code32(game::render_task_imm, (uint32_t)(uintptr_t)hk_render_task);
	// vf17 dos emissores (suprime a simulação no olho direito)
	static const uintptr_t vf17_slots[] = {0x012CA2D8, 0x012CA330, 0x012CA3A8, 0x012CA420, 0x012CA498, 0x012CA510, 0x012CA568};
	for (uintptr_t a : vf17_slots)
		if (*(uintptr_t*)a == k_vf17) patch_ptr(a, (void*)hk_vf17);
		else logf("estéreo: slot do vf17 em %p não confere", (void*)a);
	// Call site do tick de FX (sonda)
	static const uint8_t fx_site[10] = {0x8B, 0x4B, 0x10, 0x8B, 0x01, 0x8B, 0x50, 0x24, 0xFF, 0xD2};
	auto site = (uint8_t*)0x004DA733;
	if (memcmp(site, fx_site, 10)) {
		logf("estéreo: call site das partículas não confere; sem sonda de partículas");
	} else {
		DWORD old;
		VirtualProtect(site, 10, PAGE_EXECUTE_READWRITE, &old);
		site[0] = 0xE8;
		*(int32_t*)(site + 1) = (int32_t)((uintptr_t)fx_thunk - ((uintptr_t)site + 5));
		memset(site + 5, 0x90, 5);
		VirtualProtect(site, 10, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 10);
	}
	logf("estéreo: hooks CLevel::vf162 (orig %p), vf161 (orig %p), CRenderer::vf41 (orig %p), RenderTask", (void*)o_vf162,
	     (void*)o_vf161, (void*)o_vf41);
}
