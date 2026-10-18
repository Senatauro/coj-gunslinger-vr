// Dicas e tutoriais do jogo em VR. Modos do mod: CENA = o jogo em 3D no headset; TELA = o jogo numa tela plana virtual.
// - Log: anota cada dica mostrada (classe, ações que ela espera, se pausa o jogo, CENA ou TELA, textos do objeto), cada
//   ação esperada que ela recebe e como ela sai; e, uma vez por chave, os textos do jogo que citam botões (%KEY, &[A]&).
// - Frases de VR: com o VR ligado (vr_texts 1), as chaves de cojvr\vr_texts.txt devolvem a frase escrita para o VR, com
//   os glifos do próprio jogo onde o botão do Touch é o mesmo do controle.
// - Ações feitas pelo mod: tut_action() conclui a dica que espera uma ação que o mod executa por chamada direta (troca de
//   arma pelo menu do Y, agachar com o corpo, mira do duelo pela cabeça), e tut_expects(204) deixa o A e o B do VR
//   chegarem ao jogo enquanto uma dica espera o "[A] to Continue".
#include "mod.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace {
// Classes de dica do tutorials.scr. Todas são IInputTarget: vf2 recebe uma ação (thiscall (ação, valor, flag), ret 0xC),
// vf23 mostra a dica (e a registra como ouvinte do InputsManagerDI, FUN_00AC7370) e vf24 a esconde (FUN_00AC7420).
// Ações esperadas: vetor em +0x74, quantidade em +0x78 (a dica com pausa sem ActionInput recebe a 204, SKIP_TUTORIAL).
// A flag de concluída fica em +0x58 nas dicas sem pausa (TutorialHint::vf2/vf9) e em +0x84 nas com pausa
// (TutorialPauseHint::vf2/vf9, que também tira o jogador da lista de ouvintes, PlayerDI+0x19DC |= 2, e pausa o tempo,
// *(*(GameDI+0xCB0)+9) = 1). Várias classes compartilham as mesmas funções nos slots; fn[] guarda o endereço original,
// que tutorial_init() confere antes de trocar o ponteiro.
struct Cls {
	uintptr_t vt;
	const char* name;
	bool pause;
	uintptr_t fn[3];  // vf2, vf23, vf24
};
const Cls k_cls[] = {
    {0x014A8384, "Hint", false, {0x00FEC810, 0x00FEC5D0, 0x00FEC670}},
    {0x014A8424, "PauseHint", true, {0x00FECFB0, 0x00FED090, 0x00FED1D0}},
    {0x014A8584, "EventHint", false, {0x00FEC810, 0x00FEC5D0, 0x00FEC670}},
    {0x014A87D4, "EventPauseHint", true, {0x00FECFB0, 0x00FED090, 0x00FED1D0}},
    {0x014A8494, "InventoryHint", false, {0x00FEC810, 0x00FED8C0, 0x00FED900}},
    {0x014A860C, "CmReminder", false, {0x00FEC810, 0x00FEC5D0, 0x00FEC670}},
    {0x014A8694, "ObjReminder", false, {0x00FEC810, 0x00FEC5D0, 0x00FEC670}},
    {0x014A871C, "SkillPntsReminder", false, {0x00FEC810, 0x00FEC5D0, 0x00FEC670}},
};
const int k_slot[3] = {2, 23, 24};

const Cls* cls_of(char* self) {
	uintptr_t vt = self ? *(uintptr_t*)self : 0;
	for (const Cls& c : k_cls)
		if (c.vt == vt) return &c;
	return NULL;
}

// Dicas visíveis agora (já mostradas e ainda não escondidas).
struct Live {
	char* self;
	const Cls* c;
	DWORD t0;
	int how;  // ação que a concluiu (−1 = nenhuma ainda, −2 = concluída pelo mod)
	bool down;
};
Live g_live[8];
int g_nlive;

Live* live_of(char* self) {
	for (int i = 0; i < g_nlive; i++)
		if (g_live[i].self == self) return &g_live[i];
	return NULL;
}
bool expects(char* self, int a) {
	int n = *(int*)(self + 0x78), *v = *(int**)(self + 0x74);
	for (int i = 0; v && i < n && i < 64; i++)
		if (v[i] == a) return true;
	return false;
}

const char* act_name(int a) {
	static char buf[8][12];
	static int k;
	switch (a) {
	case 6: return "STRAFE_RIGHT";
	case 7: return "STRAFE_LEFT";
	case 11: return "JUMP";
	case 14: return "DUCK_TOGGLE";
	case 15: return "DUCK";
	case 16: return "SPRINT_TOGGLE";
	case 17: return "SPRINT";
	case 20: return "EXECUTE_TRIGGER";
	case 144: return "CONCENTRATION";
	case 170: return "MELEE";
	case 171: return "GRENADE_THROW";
	case 174: return "WEAPON_SLOT1";
	case 176: return "WEAPON_SLOT3";
	case 191: return "CHARACTER_MENU";
	case 195: return "TOGGLE_DETECTIVE";
	case 197: return "AKIMBO1";
	case 204: return "SKIP_TUTORIAL";
	case 208: return "WEAPON_SLOT5";
	case 210: return "DUEL_AIM_UP";
	case 212: return "DUEL_AIM_LEFT";
	}
	char* b = buf[k++ & 7];
	snprintf(b, sizeof buf[0], "%d", a);
	return b;
}

// Junta os textos (ASCII ou UTF-16) apontados pelos primeiros campos do objeto; é assim que o nome do tutorial e o
// InfoText aparecem no log sem conhecer o layout da classe. readable() evita ler memória que não está mapeada.
bool readable(const void* p, size_t n) {
	MEMORY_BASIC_INFORMATION mi;
	if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
	return (const char*)p + n <= (const char*)mi.BaseAddress + mi.RegionSize;
}
void obj_strings(char* self, char* out, int cap) {
	int k = 0;
	out[0] = 0;
	for (int off = 4; off < 0xC0 && k < cap - 48; off += 4) {
		char* s = *(char**)(self + off);
		if ((uintptr_t)s < 0x10000 || !readable(s, 64)) continue;
		char t[40];
		int n = 0;
		bool wide = s[1] == 0 && s[0];
		for (; n < 39; n++) {
			char ch = wide ? (((wchar_t*)s)[n] < 128 ? (char)((wchar_t*)s)[n] : '?') : s[n];
			if (!ch) break;
			if (ch < 32 || ch > 126) n = -1;
			if (n < 0) break;
			t[n] = ch;
		}
		if (n < 3) continue;
		t[n] = 0;
		k += snprintf(out + k, cap - k, " +0x%X%s'%s'", off, wide ? "w" : "", t);
	}
}

typedef void(__thiscall* in_t)(char*, int, float, char);
typedef void(__thiscall* vf_t)(char*);

// Hooks dos três slots (vf2, vf23, vf24): chamam a função original da classe e acompanham a dica no log.
void __fastcall hk_in(char* self, void*, int a, float v, char f) {
	const Cls* c = cls_of(self);
	((in_t)c->fn[0])(self, a, v, f);
	Live* l = live_of(self);
	if (!l || f || !expects(self, a)) return;
	bool down = v > 0.5f || v < -0.5f;
	if (down != l->down) logf("dicas: dica %s %p recebeu %s %.2f", c->name, (void*)self, act_name(a), v), l->down = down;
	if (self[c->pause ? 0x84 : 0x58] && l->how == -1) l->how = a;
}

void __fastcall hk_show(char* self, void*) {
	const Cls* c = cls_of(self);
	((vf_t)c->fn[1])(self);
	if (!live_of(self) && g_nlive < 8) g_live[g_nlive++] = {self, c, GetTickCount(), -1, false};
	char acts[160] = "", strs[400];
	int n = *(int*)(self + 0x78), *v = *(int**)(self + 0x74), k = 0;
	for (int i = 0; v && i < n && i < 8; i++) k += snprintf(acts + k, sizeof acts - k, "%s%s", i ? "," : "", act_name(v[i]));
	obj_strings(self, strs, sizeof strs);
	char* gd = *(char**)0x02E65DB4;
	char* pz = gd ? *(char**)(gd + 0xCB0) : NULL;
	const char* why = player_tela_reason();
	logf("dicas: dica %s %p na tela | espera %s | tempo pausado %d | estado %d | %s%s | textos:%s", c->name, (void*)self,
	     n ? acts : "nada", pz ? pz[9] : -1, gd ? *(int*)(gd + 0xC3C) : -1, why ? "TELA: " : "CENA", why ? why : "", strs);
}

void __fastcall hk_hide(char* self, void*) {
	const Cls* c = cls_of(self);
	Live* l = live_of(self);
	bool done = self[c->pause ? 0x84 : 0x58];
	((vf_t)c->fn[2])(self);
	if (!l) return;
	logf("dicas: dica %s %p saiu depois de %.1f s (%s%s)", c->name, (void*)self, (GetTickCount() - l->t0) / 1000.f,
	     !done ? "sem concluir: tempo ou evento" : l->how == -2 ? "concluída pelo mod" : "concluída pela ação ",
	     done && l->how >= 0 ? act_name(l->how) : "");
	*l = g_live[--g_nlive];
}

// --- Frases de VR por chave de texto. FUN_007865E0 troca cada &chave& pelo valor de FUN_007867C0 (stdcall
// (String* chave), EAX = contexto, ret 4; devolve wchar_t* ou 0), que só é chamada em 0x00786691. O resultado passa de
// novo pelo mesmo expansor (FUN_004441B0), então um &[B]& dentro da frase vira o glifo do botão no jogo. A chave é o
// texto entre os &, igual ao tutorials.scr (inclusive o $, como em "Hint_SOD_$"). Botões do VR em CENA (mod.cpp):
// A = corpo a corpo, B = Concentração, stick direito para cima = pular e para baixo = agachar, clique dele = objetivo
// (segurar: habilidades), Y = menu de armas. ---
// As frases ficam em cojvr\vr_texts.txt (UTF-8, "Chave = frase", \n quebra a linha, # comenta), para dar para editar
// sem recompilar; a mudança vale no próximo lançamento. ponytail: até 256 chaves de até 63 bytes, busca linear.
struct Text {
	char key[64];
	wchar_t* vr;
};
Text g_text[256];
int g_ntext;
void load_texts() {
	char path[MAX_PATH];
	snprintf(path, sizeof path, "%svr_texts.txt", g_moddir);
	FILE* f = fopen(path, "rb");
	if (!f) return logf("dicas: %s não existe; textos do jogo sem troca", path);
	char line[1024];
	while (fgets(line, sizeof line, f) && g_ntext < 256) {
		char *k = line + (memcmp(line, "\xEF\xBB\xBF", 3) ? 0 : 3), *e = strchr(k, '=');
		if (*k == '#' || !e) continue;
		*e = 0;
		char* v = e + 1;
		auto trim = [](char*& a) {
			while (*a == ' ' || *a == '\t') a++;
			for (char* z = a + strlen(a); z > a && strchr(" \t\r\n", z[-1]);) *--z = 0;
		};
		trim(k), trim(v);
		if (!*k || strlen(k) >= sizeof g_text[0].key) continue;
		char* w = v;
		for (char* r = v; *r; r++) *w++ = r[0] == '\\' && r[1] == 'n' ? (r++, '\n') : *r;
		*w = 0;
		int n = MultiByteToWideChar(CP_UTF8, 0, v, -1, NULL, 0);
		if (n <= 0) continue;
		Text& t = g_text[g_ntext];
		t.vr = (wchar_t*)malloc(n * sizeof(wchar_t));
		MultiByteToWideChar(CP_UTF8, 0, v, -1, t.vr, n);
		strcpy(t.key, k);
		g_ntext++;
	}
	fclose(f);
	logf("dicas: %d frases de VR lidas de vr_texts.txt", g_ntext);
}

// Hash (FNV-1a) das chaves de texto com botão já anotadas no log, para anotar cada uma só uma vez.
// ponytail: guarda até 512 hashes; quando enche, para de anotar.
unsigned g_seen[512];
int g_nseen;
bool first_time(const char* k, int n) {
	unsigned h = 2166136261u;
	for (int i = 0; i < n; i++) h = (h ^ (unsigned char)k[i]) * 16777619u;
	for (int i = 0; i < g_nseen; i++)
		if (g_seen[i] == h) return false;
	if (g_nseen < 512) g_seen[g_nseen++] = h;
	return g_nseen < 512;
}
bool has_button(const wchar_t* v) {
	for (; v && *v; v++)
		if ((v[0] == L'%' && v[1] == L'K' && v[2] == L'E' && v[3] == L'Y') || (v[0] == L'&' && v[1] == L'[')) return true;
	return false;
}
}  // namespace

// loc_orig chama a FUN_007867C0 original na convenção dela (contexto em EAX, chave na pilha). loc_thunk entra no lugar
// da chamada em 0x00786691: recebe os mesmos argumentos, repassa a loc_text em cdecl e sai com ret 4.
extern "C" const wchar_t* __cdecl loc_orig(void* key, void* ctx);
asm(".globl _loc_orig\n"
    "_loc_orig:\n"
    "  mov 8(%esp), %eax\n"
    "  push 4(%esp)\n"
    "  mov $0x007867C0, %ecx\n"
    "  call *%ecx\n"
    "  ret\n");
extern "C" const wchar_t* __cdecl loc_text(void* key, void* ctx) {
	const char* k = *(const char**)key;
	int n = *(int*)((char*)key + 4);
	if (k && n > 0 && n < 64 && (g_vr || setting("fake_hmd", 0)) && setting("vr_texts", 1))
		for (int i = 0; i < g_ntext; i++)
			if ((int)strlen(g_text[i].key) == n && !memcmp(g_text[i].key, k, n)) return g_text[i].vr;
	const wchar_t* v = loc_orig(key, ctx);
	if (k && n > 0 && n < 64 && has_button(v) && first_time(k, n)) {
		char a[200];
		int i = 0;
		for (; v[i] && i < 199; i++) a[i] = v[i] < 128 ? (v[i] == '\n' ? '|' : (char)v[i]) : '?';
		a[i] = 0;
		logf("dicas: texto com botão %.*s = \"%s\"", n, k, a);
	}
	return v;
}
extern "C" void loc_thunk();
asm(".globl _loc_thunk\n"
    "_loc_thunk:\n"
    "  push %eax\n"
    "  push 8(%esp)\n"
    "  call _loc_text\n"
    "  add $8, %esp\n"
    "  ret $4\n");

// Há uma dica com pausa na tela? Nesse caso player.cpp trata o jogo como na pausa em 3D, com a dica num painel no mundo.
bool tut_paused() {
	for (int i = 0; i < g_nlive; i++)
		if (g_live[i].c->pause) return true;
	return false;
}

// Alguma dica na tela espera a ação a.
bool tut_expects(int a) {
	for (int i = 0; i < g_nlive; i++)
		if (expects(g_live[i].self, a)) return true;
	return false;
}

// O mod fez a ação a por chamada direta, sem passar pelo InputsManagerDI. Marca como concluídas as dicas que a esperam
// escrevendo a flag, em vez de chamar o vf2: a dica com pausa repassaria a ação ao jogador e o jogo a faria de novo.
void tut_action(int a) {
	for (int i = 0; i < g_nlive; i++) {
		Live& l = g_live[i];
		if (!expects(l.self, a) || l.self[l.c->pause ? 0x84 : 0x58]) continue;
		l.self[l.c->pause ? 0x84 : 0x58] = 1, l.how = -2;
		logf("dicas: dica %s %p concluída pelo mod (%s)", l.c->name, (void*)l.self, act_name(a));
	}
}

void tutorial_init() {
	load_texts();
	int ok = 0;
	for (const Cls& c : k_cls) {
		bool match = true;
		for (int s = 0; s < 3; s++) match &= *(uintptr_t*)(c.vt + 4 * k_slot[s]) == c.fn[s];
		if (!match) {
			logf("dicas: vtable da %s não confere; sem hook nela", c.name);
			continue;
		}
		patch_ptr(c.vt + 4 * k_slot[0], (void*)hk_in);
		patch_ptr(c.vt + 4 * k_slot[1], (void*)hk_show);
		patch_ptr(c.vt + 4 * k_slot[2], (void*)hk_hide);
		ok++;
	}
	bool txt = patch_call(0x00786691, 0x007867C0, (void*)loc_thunk);
	logf("dicas: hooks em %d de %d classes, textos %s", ok, (int)(sizeof k_cls / sizeof *k_cls), txt ? "ok" : "não confere");
}
