// Opções de VR no menu do jogo. A lista de Opções → Jogo (MenuOptionsGameCJ) é montada em código: FUN_008D61E0
// (chamada em 0x008D5A97 pelo vf213, ao mostrar a página) cria cada linha a partir do modelo "OptionTextDummy", e
// FUN_008D5C90 (chamada em 0x008D5B64 pelo vf485, ao aceitar) lê os valores escolhidos. O mod acrescenta as linhas de VR
// logo depois da montagem, usando as mesmas funções do jogo, e lê as escolhas logo depois do aceitar: grava no
// cojvr.ini (seção [stereo]) e aplica na hora. Nenhum arquivo do jogo é alterado.
#include "mod.h"
#include <math.h>
#include <stdio.h>

namespace {
struct GStr {  // string do engine: o texto vem do malloc do jogo (FUN_00402110) e se solta com game_free
	char* p;
	unsigned n, cap;
};
}  // namespace

// As funções abaixo recebem argumentos em registradores, por isso os wrappers em asm.
// FUN_00402110 monta uma GStr a partir de um texto: EDI = destino, EAX = tamanho máximo (passamos −1),
// [esp+4] = texto, ret 4.
extern "C" void game_str(GStr* dst, const char* s);
asm(".globl _game_str\n"
    "_game_str:\n"
    "  push %edi\n"
    "  mov 8(%esp), %edi\n"
    "  push 12(%esp)\n"
    "  or $-1, %eax\n"
    "  mov $0x00402110, %ecx\n"
    "  call *%ecx\n"
    "  pop %edi\n"
    "  ret\n");
// S_UIListBox::CreateNewItem (0x010551E0): EDI = lista, (modelo, nome, 0), ret 0xC; devolve o item.
extern "C" char* game_new_item(char* list, GStr* model, GStr* name);
asm(".globl _game_new_item\n"
    "_game_new_item:\n"
    "  push %edi\n"
    "  push %esi\n"
    "  push %ebx\n"
    "  mov 16(%esp), %edi\n"
    "  push $0\n"
    "  push 28(%esp)\n"
    "  push 28(%esp)\n"
    "  mov $0x010551E0, %eax\n"
    "  call *%eax\n"
    "  pop %ebx\n"
    "  pop %esi\n"
    "  pop %edi\n"
    "  ret\n");
// push_back de vetor do engine (0x006BDF30): ESI = vetor, [esp+4] = &elemento, ret 4.
extern "C" void game_vec_push(void* vec, char** item);
asm(".globl _game_vec_push\n"
    "_game_vec_push:\n"
    "  push %esi\n"
    "  mov 8(%esp), %esi\n"
    "  push 12(%esp)\n"
    "  mov $0x006BDF30, %eax\n"
    "  call *%eax\n"
    "  pop %esi\n"
    "  ret\n");
// Seleciona o valor de uma opção (0x0105D880): EDI = opção, (valor, 1), ret 8.
extern "C" void game_opt_select(char* opt, int v);
asm(".globl _game_opt_select\n"
    "_game_opt_select:\n"
    "  push %edi\n"
    "  mov 8(%esp), %edi\n"
    "  push $1\n"
    "  push 16(%esp)\n"
    "  mov $0x0105D880, %eax\n"
    "  call *%eax\n"
    "  pop %edi\n"
    "  ret\n");

namespace {
void game_free(void* p) { ((void(__cdecl*)(void*)) * (void**)0x01258480)(p); }
void* game_malloc(size_t n) { return ((void*(__cdecl*)(size_t)) * (void**)0x01258424)(n); }

char g_update_label[64];  // "VR: update available (vX)"; essa linha só aparece quando há versão nova

struct VrOpt {
	const char *label, *key;
	float def;
	int n;
	const char* names[8];
	float vals[8];
};
VrOpt g_opts[] = {  // rótulos em inglês, como o resto do menu; key = nome da chave em [stereo] no cojvr.ini
	{"VR: play position", "seated", 1, 2, {"Standing", "Seated"}, {0, 1}},
	{"VR: dominant hand", "left_handed", 0, 2, {"Right", "Left"}, {0, 1}},
	{"VR: crouch with your body", "phys_crouch", 1, 2, {"Off", "On"}, {0, 1}},
	{"VR: turning", "turn", 0, 2, {"Snap", "Smooth"}, {0, 1}},
	{"VR: snap angle", "snap_deg", 45, 4, {"30°", "45°", "60°", "90°"}, {30, 45, 60, 90}},
	{"VR: locomotion", "move_type", 0, 2, {"Smooth", "Teleport"}, {0, 1}},
	{"VR: walk direction", "move_dir", 0, 3, {"Head", "Left hand", "Right hand"}, {0, 1, 2}},
	{"VR: comfort vignette", "vignette", 0.6f, 4, {"Off", "Low", "Medium", "High"}, {0, 0.35f, 0.6f, 0.85f}},
	{"VR: walk speed", "move_speed", 0.75f, 4, {"50%", "75%", "100%", "125%"}, {0.5f, 0.75f, 1, 1.25f}},
	{"VR: ladders", "ladder_auto", 1, 2, {"Climb with the stick", "Straight to the top/bottom"}, {0, 1}},
	{"VR: aim dot", "crosshair_vr", 1, 2, {"Off", "On"}, {0, 1}},
	{"VR: aim dot shape", "crosshair_shape", 0, 4, {"Ring and dot", "Dot", "Ring", "Cross"}, {0, 1, 2, 3}},
	{"VR: aim dot color (right)", "crosshair_color_r", 0, 8, {"Red", "Blue", "Green", "Yellow", "White", "Orange", "Purple", "Cyan"}, {0, 1, 2, 3, 4, 5, 6, 7}},
	{"VR: aim dot color (left)", "crosshair_color_l", 1, 8, {"Red", "Blue", "Green", "Yellow", "White", "Orange", "Purple", "Cyan"}, {0, 1, 2, 3, 4, 5, 6, 7}},
	{"VR: HUD distance", "hud_dist", 1.5f, 4, {"1.0 m", "1.5 m", "2.0 m", "2.5 m"}, {1, 1.5f, 2, 2.5f}},
	{"VR: HUD size", "hud_width", 1.3f, 3, {"Small", "Medium", "Large"}, {1, 1.3f, 1.6f}},
	{"VR: ammo on wrist", "ammo_wrist", 1, 2, {"Off", "On"}, {0, 1}},
	{"VR: cinema screen in menus", "cinema", 1, 2, {"Off", "On"}, {0, 1}},
	{"VR: 3D pause menu", "pause_3d", 1, 2, {"Off", "On"}, {0, 1}},
	{"VR: duels", "duel_vr", 1, 2, {"Flat screen", "VR"}, {0, 1}},
	{"VR: duel focus", "duel_signs", 1, 2, {"Game's reticle", "Signs on the enemy"}, {0, 1}},
	{"VR: duel aim dot", "duel_gaze", 1, 2, {"Off", "On"}, {0, 1}},
	{"VR: realistic guns (experimental)", "real_guns", 0, 2, {"Off", "On"}, {0, 1}},
	// Linhas de ação (não são gravadas no INI): escolher "Open…" e aceitar abre a página no navegador do computador
	{"VR: report a bug / send feedback", "report", 0, 2, {"-", "Open page on the PC"}, {0, 1}},
	{g_update_label, "update", 0, 2, {"-", "Open download page on the PC"}, {0, 1}},
};
const int N_OPTS = sizeof g_opts / sizeof *g_opts;
char* g_item[N_OPTS];  // linha de cada opção enquanto a página está aberta

// Cria uma linha de opção na lista com as mesmas chamadas que o jogo usa: item a partir do modelo, push no vetor de
// itens (+0x48), cast do item para a opção (0x012582FC, com a assinatura do __RTDynamicCast), chave e rótulo, um valor
// por nome (0x0105CFC0) e, por fim, seleciona o valor mais próximo do que está no cojvr.ini.
char* add_opt(char* list, VrOpt& o) {
	GStr model, name, label, key;
	game_str(&model, "OptionTextDummy"), game_str(&name, "");
	char* it = game_new_item(list, &model, &name);
	game_free(model.p), game_free(name.p);
	if (!it) return NULL;
	game_vec_push(list + 0x48, &it);
	((void(__thiscall*)(char*, int, int))(*(void***)list)[0x554 / 4])(list, *(int*)(list + 0x4C) - 1, 1);
	char* opt = (char*)((void*(__cdecl*)(void*, long, void*, void*, int)) * (void**)0x012582FC)(it, 0, (void*)0x0162EFDC, (void*)0x0162F4C0, 0);
	if (!opt) return NULL;
	game_str(&key, o.key), game_str(&label, o.label);
	char* sub = *(char**)(opt + 0x10);
	((void(__thiscall*)(char*, GStr*))(*(void***)sub)[0x324 / 4])(sub, &key);
	((void(__thiscall*)(char*, GStr*, int))(*(void***)opt)[0x3A8 / 4])(opt, &label, 0);
	game_free(key.p), game_free(label.p);
	int cur = 0;
	float v = setting(o.key, o.def), best = 1e9f;
	for (int i = 0; i < o.n; i++) {
		GStr s;
		game_str(&s, o.names[i]);
		int* d = (int*)game_malloc(12);  // UIDataInt: vtable, byte, valor
		d[0] = 0x014B3790, d[1] = 0, d[2] = i;
		((void(__stdcall*)(char*, GStr*, int*))0x0105CFC0)(opt, &s, d);
		game_free(s.p);
		if (fabsf(o.vals[i] - v) < best) best = fabsf(o.vals[i] - v), cur = i;
	}
	game_opt_select(opt, cur);
	return opt;
}

typedef void(__stdcall* page_t)(char*);
// No lugar de FUN_008D61E0: monta a página como o jogo e acrescenta as linhas de VR na lista (menu+400).
void __stdcall hk_fill(char* menu) {
	((page_t)0x008D61E0)(menu);
	char* list = *(char**)(menu + 400);
	if (!list || !setting("vr_options", 1)) return;
	snprintf(g_update_label, sizeof g_update_label, "VR: update available (%s)", g_update_tag);
	for (int k = 0; k < N_OPTS; k++) g_item[k] = g_opts[k].label != g_update_label || *g_update_tag ? add_opt(list, g_opts[k]) : NULL;
	logf("opções: %d opções de VR na lista de Opções → Jogo", N_OPTS);
}

// No lugar de FUN_008D5C90: o jogo lê as opções dele e depois o mod lê as linhas de VR. Só grava e aplica o que mudou;
// as linhas de ação abrem a página em vez de gravar.
void __stdcall hk_save(char* menu) {
	((page_t)0x008D5C90)(menu);
	char ini[MAX_PATH], buf[32];
	snprintf(ini, sizeof ini, "%scojvr.ini", g_moddir);
	for (int k = 0; k < N_OPTS; k++) {
		VrOpt& o = g_opts[k];
		char *opt = g_item[k], *cur = opt ? *(char**)(opt + 0xA8) : NULL;
		g_item[k] = NULL;
		if (!cur || !*(int*)(opt + 0xA0)) continue;
		void* d = *(void**)(cur - 4);
		int i = ((int(__thiscall*)(void*))(*(void***)d)[4])(d);
		if (i < 0 || i >= o.n || o.vals[i] == setting(o.key, o.def)) continue;
		if (!strcmp(o.key, "report") || !strcmp(o.key, "update")) {
			open_page(o.key);
			continue;
		}
		setting_set(o.key, o.vals[i]);
		snprintf(buf, sizeof buf, "%g", o.vals[i]);
		WritePrivateProfileStringA("stereo", o.key, buf, ini);
		if (!strcmp(o.key, "seated")) pose_state().calibrated = false;  // a referência de altura muda: força recentrar
		logf("opções: %s = %s (%s)", o.key, buf, o.names[i]);
	}
}
}  // namespace

void options_init() {
	patch_call(0x008D5A97, 0x008D61E0, (void*)hk_fill);
	patch_call(0x008D5B64, 0x008D5C90, (void*)hk_save);
}
