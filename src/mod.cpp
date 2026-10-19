// Núcleo do mod: checagem da versão do exe, log de exceções, hook do Present do CRenderer (FPS e relatórios),
// captura de tela (SHOT) e hook do XInput, que junta os controles VR e o pad de teste.
#include "mod.h"
#include <d3d9.h>
#include <xinput.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

bool g_version_ok;

void* patch_ptr(uintptr_t addr, void* val) {
	DWORD old;
	VirtualProtect((void*)addr, sizeof(void*), PAGE_READWRITE, &old);
	void* prev = InterlockedExchangePointer((void**)addr, val);
	VirtualProtect((void*)addr, sizeof(void*), old, &old);
	return prev;
}

static bool check_version() {
	auto base = (uint8_t*)GetModuleHandleA(NULL);
	if ((uintptr_t)base != game::base) {
		logf("exe carregado em %p, não em 0x400000: endereços fixos inválidos", (void*)base);
		return false;
	}
	auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	auto text = IMAGE_FIRST_SECTION(nt);
	logf("exe TimeDateStamp %08lx SizeOfImage %08lx .text %08lx", nt->FileHeader.TimeDateStamp,
	     nt->OptionalHeader.SizeOfImage, text->Misc.VirtualSize);
	if (nt->FileHeader.TimeDateStamp != game::timestamp || nt->OptionalHeader.SizeOfImage != game::size_of_image ||
	    text->Misc.VirtualSize != game::text_vsize)
		return false;
	uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64
	for (const uint8_t *p = base + text->VirtualAddress, *e = p + text->Misc.VirtualSize; p < e; p++)
		h = (h ^ *p) * 0x100000001b3ULL;
	logf(".text fnv1a64 %016llx (esperado %016llx)", (unsigned long long)h, (unsigned long long)game::text_fnv1a64);
	// ponytail: sem busca por padrão de bytes (AOB), porque só a build da Steam é suportada. Se surgir outra build, os
	// endereços fixos de game:: terão de ser achados por assinatura.
	return h == game::text_fnv1a64;
}

// Registra no log as 30 primeiras exceções de aviso ou erro (código >= 0x80000000), com módulo e offset. Não trata
// nada: a busca por um handler segue normalmente.
static LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
	DWORD code = e->ExceptionRecord->ExceptionCode;
	if (code < 0x80000000 || code == 0xE06D7363 /* C++ throw */) return EXCEPTION_CONTINUE_SEARCH;
	static LONG n;
	if (InterlockedIncrement(&n) <= 30) {
		void* at = e->ExceptionRecord->ExceptionAddress;
		HMODULE m = NULL;
		char mod[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)at, &m))
			GetModuleFileNameA(m, mod, MAX_PATH);
		logf("exceção %08lx em %p (%s+0x%lx) info %lx %lx", code, at, strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod,
		     (unsigned long)((uintptr_t)at - (uintptr_t)m), e->ExceptionRecord->ExceptionInformation[0],
		     e->ExceptionRecord->ExceptionInformation[1]);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// SHOT: grava uma surface em cojvr\shot_N<tag>.bmp. Roda na thread que renderiza: no Present (backbuffer, tag "")
// e, em estéreo, no fim do laço de olhos (a cópia de cada olho, "_E"/"_D", com o mesmo N). N só avança no Present.
volatile LONG g_shot_request;
int g_shot_n;

void dump_surface(IDirect3DSurface9* src, const char* tag) {
	auto dev = *(IDirect3DDevice9**)game::device;
	IDirect3DSurface9* sys = NULL;
	D3DSURFACE_DESC d;
	D3DLOCKED_RECT lr;
	src->GetDesc(&d);
	if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
	    SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
	    SUCCEEDED(dev->GetRenderTargetData(src, sys)) && SUCCEEDED(sys->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
		char path[MAX_PATH];
		snprintf(path, MAX_PATH, "%sshot_%d%s.bmp", g_moddir, g_shot_n, tag);
		HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		// SET shot_w/shot_h/shot_x/shot_y grava só um recorte, o que deixa leve uma sequência rápida de SHOTs
		UINT cw = (UINT)setting("shot_w", 0), ch = (UINT)setting("shot_h", 0), cx = (UINT)setting("shot_x", 0), cy = (UINT)setting("shot_y", 0);
		if (!cw || !ch || cx + cw > d.Width || cy + ch > d.Height) cw = d.Width, ch = d.Height, cx = cy = 0;
		DWORD row = cw * 4, w;
		BITMAPFILEHEADER fh = {0x4D42, DWORD(54 + row * ch), 0, 0, 54};
		BITMAPINFOHEADER ih = {40, LONG(cw), -LONG(ch), 1, 32, BI_RGB, 0, 0, 0, 0, 0};
		WriteFile(f, &fh, 14, &w, NULL);
		WriteFile(f, &ih, 40, &w, NULL);
		for (UINT y = cy; y < cy + ch; y++) WriteFile(f, (char*)lr.pBits + y * lr.Pitch + cx * 4, row, &w, NULL);
		CloseHandle(f);
		sys->UnlockRect();
		logf("SHOT: %s (%ux%u)", path, d.Width, d.Height);
	} else
		logf("SHOT: falhou (formato %d)", d.Format);
	if (sys) sys->Release();
}

static void dump_backbuffer() {
	IDirect3DSurface9* bb = NULL;
	if (FAILED((*(IDirect3DDevice9**)game::device)->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
		return logf("SHOT: GetBackBuffer falhou");
	dump_surface(bb, "");
	bb->Release();
}

// --- Present do CRenderer (vf9; thiscall, 3 args, ret 0xC) ---
typedef uintptr_t(__thiscall* present_t)(void*, uintptr_t, uintptr_t, uintptr_t);
static present_t o_present;

static uintptr_t __fastcall hk_present(void* self, void*, uintptr_t a, uintptr_t b, uintptr_t c) {
	stereo_present();
	focus_frame();
	// 1 = pedido (pad.txt); 2 = os olhos já foram gravados neste frame. Em frame estéreo o pedido 1 fica para o laço
	// de olhos, que grava os olhos e passa o pedido a 2.
	LONG req = g_shot_request;
	if (req == 2 || (req == 1 && !stereo_frame())) g_shot_request = 0, dump_backbuffer(), g_shot_n++;
	vr_frame_pre();
	static LARGE_INTEGER freq, last;
	static unsigned frames, total;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!total++) {
		QueryPerformanceFrequency(&freq);
		last = now;
		logf("1º Present do CRenderer %p", self);
	}
	frames++;
	if (now.QuadPart - last.QuadPart >= 5 * freq.QuadPart) {
		logf("fps %.1f", frames * double(freq.QuadPart) / (now.QuadPart - last.QuadPart));
		frames = 0;
		last = now;
		stereo_report();
		video_report();
		vr_report();
		stats_report();
	}
	uintptr_t r = o_present(self, a, b, c);
	vr_frame_post();
	return r;
}

// --- XInputGetState: os controles VR viram um pad virtual, e o pad de teste lido de cojvr\pad.txt (só com
// [dev] pad_script=1) soma por cima ---
typedef DWORD(WINAPI* xgs_t)(DWORD, XINPUT_STATE*);
static xgs_t o_xgs;
static volatile LONG g_pad = -1;  // botões do pad de teste; -1 = pad de teste ainda não ativado
static volatile LONG g_axis[6];  // LX, LY, RX, RY, LT, RT do pad de teste

static DWORD WINAPI hk_xgs(DWORD user, XINPUT_STATE* s) {
	static LONG calls;
	if (InterlockedIncrement(&calls) == 1) logf("XInputGetState: 1ª chamada (user %lu)", user);
	LONG b = g_pad;
	XINPUT_GAMEPAD v = {};
	bool vr = user == 0 && vr_gamepad(&v);  // controles VR; o pad de teste soma por cima
	if (vr) v.wButtons &= ~real_buttons(v.wButtons);  // polegar no cão/alavanca (armas realistas)
	if (user != 0 || (b < 0 && !vr)) return o_xgs(user, s);
	if (b < 0) b = 0;
	static DWORD packet;
	static LONG last[7] = {-1};
	auto pick = [](LONG test, LONG real) { return test ? test : real; };
	LONG cur[7] = {b | v.wButtons, pick(g_axis[0], v.sThumbLX), pick(g_axis[1], v.sThumbLY), pick(g_axis[2], v.sThumbRX),
	               pick(g_axis[3], v.sThumbRY), pick(g_axis[4], v.bLeftTrigger), pick(g_axis[5], v.bRightTrigger)};
	if (float d = player_dodge()) cur[1] = d > 0 ? 32767 : -32767;  // esquiva do Sense of Death pela cabeça
	bool melee = false;
	float vig = 0;
	bool cena = player_rstick(cur[3] / 32767.f);
	if (cena) {  // em CENA o giro pelo stick direito é do mod; o eixo Y do stick vira pular/agachar
		// Mapeamento em CENA: B do VR = Concentração (RB do jogo), A do VR = corpo a corpo, stick direito para cima =
		// pular (A do jogo) e para baixo = agachar (B do jogo). Menus e TELA seguem com A/B normais, e o pad de teste
		// não é remapeado. A e B também vão ao jogo sem troca no duelo (a dica do primeiro duelo da campanha só fecha
		// com o A) e quando uma dica na tela espera o "[A] to Continue" (SKIP_TUTORIAL, 204).
		char* pl = player_local();
		if (!(pl && duel_active(pl)) && !tut_expects(204)) {
			const WORD ab = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B;
			cur[0] = b | (v.wButtons & ~ab);
			if (v.wButtons & XINPUT_GAMEPAD_B) cur[0] |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
			if (v.wButtons & XINPUT_GAMEPAD_A) melee = true;
		}
		if (cur[4] > 0.7f * 32767) cur[0] |= XINPUT_GAMEPAD_A;
		else if (cur[4] < -0.7f * 32767) cur[0] |= XINPUT_GAMEPAD_B;
		bool turning = abs(cur[3]) > 0.3f * 32767;
		cur[3] = cur[4] = 0;
		bool tp = teleport_pad(cur);  // no modo teleporte o stick esquerdo mira o arco e não chega ao jogo
		// move_dir: anda na direção da cabeça (0; o jogo já anda para onde o corpo olha), da mão esquerda (1) ou da
		// direita (2). Para as mãos, gira o stick esquerdo pela diferença de yaw entre o controle e o HMD no tracking
		// (−Z = frente).
		int dir = (int)setting("move_dir", 0);
		float hmd[12], hand[12];
		if (dir && !tp && !player_dodge() && (cur[1] || cur[2]) && vr_hmd_pose(true, hmd) && vr_hand_pose(dir == 1 ? 1 : 0, true, hand)) {
			float hx = -hand[2], hz = -hand[10];
			// ponytail: com o controle quase vertical (parte horizontal da frente < 0,3) o yaw dele não é confiável, e
			// o stick fica como está (anda pela cabeça)
			if (hx * hx + hz * hz > 0.09f) {
				float d = atan2f(hx, -hz) - atan2f(-hmd[2], hmd[10]), c = cosf(d), sn = sinf(d);
				float x = cur[1] * c + cur[2] * sn, y = -cur[1] * sn + cur[2] * c;
				cur[1] = (LONG)fmaxf(-32767, fminf(32767, x)), cur[2] = (LONG)fmaxf(-32767, fminf(32767, y));
			}
		}
		// Vinheta de conforto enquanto anda (stick esquerdo) ou gira (X do stick direito); intensidade nas opções de VR
		if (!tp && (hypotf(cur[1], cur[2]) > 0.2f * 32767 || turning)) vig = setting("vignette", 0.6f);
	}
	vr_vignette(vig);
	if (ladder_busy()) cur[0] &= ~XINPUT_GAMEPAD_X;  // o X (usar) soltaria a escada durante a subida automática
	weapon_pad(cur);  // grip e especial da dinamite; dinamite pelo cinto e corpo a corpo pelo golpe da mão
	// Em CENA, o clique do stick direito do VR vira o BACK do jogo: segurado por 0,6 s, abre o menu de habilidades
	// (CHARACTER_MENU no inputs_pad.scr), já que os controles Touch não têm botão Back. Até 3 s depois de lançar a
	// dinamite o weapon_pad já trocou esse clique por LB (o especial), e aí ele não é convertido.
	if (cena && (v.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) && (cur[0] & XINPUT_GAMEPAD_RIGHT_THUMB))
		cur[0] = (cur[0] & ~XINPUT_GAMEPAD_RIGHT_THUMB) | XINPUT_GAMEPAD_BACK;
	// Corpo a corpo = clique do stick direito do jogo (MELEE_ATTACK, 170). Fica depois das conversões acima para não
	// virar BACK nem LB.
	if (melee) cur[0] |= XINPUT_GAMEPAD_RIGHT_THUMB;
	if (memcmp(cur, last, sizeof cur)) packet++, memcpy(last, cur, sizeof cur);
	memset(s, 0, sizeof *s);
	s->dwPacketNumber = packet;
	s->Gamepad.wButtons = (WORD)cur[0];  // pad de teste + controles VR, já remapeados acima
	s->Gamepad.sThumbLX = (SHORT)cur[1];
	s->Gamepad.sThumbLY = (SHORT)cur[2];
	s->Gamepad.sThumbRX = (SHORT)cur[3];
	s->Gamepad.sThumbRY = (SHORT)cur[4];
	s->Gamepad.bLeftTrigger = (BYTE)cur[5];
	s->Gamepad.bRightTrigger = (BYTE)cur[6];
	return ERROR_SUCCESS;
}

// Script do pad de teste, uma linha por passo: "A 150" (botão por 150 ms), "DOWN+A 100", "WAIT 2000", "SHOT" (print
// do backbuffer), "RECENTER" (tela VR), "SET <chave> <valor>" ([stereo] em tempo real), "MODE AUTO|CENA|TELA",
// "LX <-1..1> [ms]" / "LY" / "RX" / "RY" (analógicos) e "LT <0..1> [ms]" / "RT" (gatilhos); sem ms o eixo fica até
// o próximo comando dele. "TP x y z" teleporta o jogador para um ponto do chão do mundo do jogo. Depois de ativado, o
// pad de teste nunca "desconecta".
static void run_pad_script(char* text) {
	static const struct { const char* name; WORD bit; } k_buttons[] = {
		{"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
		{"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT}, {"START", XINPUT_GAMEPAD_START}, {"BACK", XINPUT_GAMEPAD_BACK},
		{"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}, {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER},
		{"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B},
		{"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y}};
	if (g_pad < 0) g_pad = 0, Sleep(500);
	for (char* line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		char keys[64];
		int ms = 150;
		if (sscanf(line, "%63s %d", keys, &ms) < 1) continue;
		if (!strcmp(keys, "SHOT") || !strcmp(keys, "RECENTER")) {
			*(keys[0] == 'S' ? &g_shot_request : &g_vr_recenter) = 1;
			continue;
		}
		char key[32], word[16];
		float val;
		if (!strcmp(keys, "SET") && sscanf(line, "SET %31s %f", key, &val) == 2) {
			setting_set(key, val);
			continue;
		}
		float tp[3];
		if (!strcmp(keys, "TP") && sscanf(line, "TP %f %f %f", tp, tp + 1, tp + 2) == 3) {
			teleport_request(tp), logf("pad: %s", line);
			continue;
		}
		if (!strcmp(keys, "MODE") && sscanf(line, "MODE %15s", word) == 1) {
			setting_set("mode", !strcmp(word, "CENA") ? 1 : !strcmp(word, "TELA") ? 2 : 0);
			continue;
		}
		int nargs;
		const char* ax = (keys[0] == 'L' || keys[0] == 'R') && !keys[2] ? strchr("XYT", keys[1]) : NULL;
		if (ax && *ax && (nargs = sscanf(line, "%*s %f %d", &val, &ms)) >= 1) {
			bool trig = *ax == 'T';  // gatilho: 0..1 -> 0..255; analógico: -1..1
			volatile LONG* axis = &g_axis[trig ? 4 + (keys[0] == 'R') : (keys[0] == 'R') * 2 + (*ax == 'Y')];
			float v = val < (trig ? 0 : -1) ? (trig ? 0 : -1) : val > 1 ? 1 : val;
			*axis = (LONG)(v * (trig ? 255 : 32767));
			logf("pad: %s", line);
			if (nargs == 2) Sleep(ms), *axis = 0;
			continue;
		}
		LONG bits = 0;
		for (char* k = keys; k && *k;) {
			char* plus = strchr(k, '+');
			if (plus) *plus = 0;
			for (auto& b : k_buttons)
				if (!strcmp(k, b.name)) bits |= b.bit;
			k = plus ? plus + 1 : NULL;
		}
		logf("pad: %s %d ms", line, ms);
		g_pad = bits;
		Sleep(ms);
		g_pad = 0;
		Sleep(150);
	}
}

static DWORD WINAPI pad_thread(void*) {
	char path[MAX_PATH];
	snprintf(path, MAX_PATH, "%spad.txt", g_moddir);
	for (;; Sleep(100)) {
		HANDLE f = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
		if (f == INVALID_HANDLE_VALUE) continue;
		static char text[16384];
		DWORD n = 0;
		ReadFile(f, text, sizeof text - 1, &n, NULL);
		CloseHandle(f);
		DeleteFileA(path);
		text[n] = 0;
		run_pad_script(text);
	}
}

void mod_init() {
	logf("mod_init na thread %lu", GetCurrentThreadId());
	AddVectoredExceptionHandler(0, on_exception);
	g_version_ok = check_version();
	if (!g_version_ok) {
		logf("versão do jogo não reconhecida: mod desligado, o jogo segue sem hooks");
		return;
	}
	logf("d3d9 carregado pelo exe: %p (este proxy: %p)", *(void**)game::d3d9_module, (void*)g_self);
	o_present = (present_t)patch_ptr(game::CRenderer_vf9_slot, (void*)hk_present);
	o_xgs = (xgs_t)patch_ptr(game::iat_XInputGetState, (void*)hk_xgs);
	logf("hooks: CRenderer::vf9 (orig %p), XInputGetState (orig %p)", (void*)o_present, (void*)o_xgs);
	video_init();
	stereo_init();
	player_init();
	if (cfg_int("dev", "pad_script", 0)) CloseHandle(CreateThread(NULL, 0, pad_thread, NULL, 0, NULL));
}
