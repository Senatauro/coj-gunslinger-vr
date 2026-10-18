// Itens ligados à distribuição do mod: bloqueia o envio de recordes ao placar online em VR, avisa quando há versão nova
// (pela última release no GitHub) e abre páginas no navegador do computador (relato de erro/feedback e download).
#include "mod.h"
#include <winhttp.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

char g_update_tag[32];  // tag da release mais nova que a instalada ("" = nenhuma ou sem resposta)

namespace {
const char* DEF_REPO = "Senatauro/coj-gunslinger-vr";

// Substitui Mode*::vf260 ("CanSaveLeaderboards", thiscall sem argumentos): devolver falso impede que o recorde vá ao
// placar online.
extern "C" int no_leaderboard() { return 0; }

long long ver_num(const char* s) {  // "v0.12.300" -> número comparável, 16 bits por parte
	int a = 0, b = 0, c = 0;
	sscanf(s + (*s == 'v'), "%d.%d.%d", &a, &b, &c);
	return (long long)a << 32 | (long long)b << 16 | c;
}

// Roda em segundo plano: pergunta à API do GitHub qual é a última release (timeouts de 5 s) e, se a tag for mais nova
// que a versão instalada, guarda em g_update_tag para os menus mostrarem o aviso.
DWORD WINAPI update_thread(void*) {
	char repo[128], body[16384];
	cfg_str("links", "repo", DEF_REPO, repo, sizeof repo);
	wchar_t path[200];
	swprintf(path, 200, L"/repos/%hs/releases/latest", repo);
	DWORD n = 0, got;
	HINTERNET s = WinHttpOpen(L"cojvr/" COJVR_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
	HINTERNET c = s ? WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
	HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
	if (r) WinHttpSetTimeouts(r, 5000, 5000, 5000, 5000);
	if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json", (DWORD)-1, NULL, 0, 0, 0) &&
	    WinHttpReceiveResponse(r, NULL))
		while (n < sizeof body - 1 && WinHttpReadData(r, body + n, sizeof body - 1 - n, &got) && got) n += got;
	body[n] = 0;
	HINTERNET hs[] = {r, c, s};
	for (HINTERNET h : hs)
		if (h) WinHttpCloseHandle(h);
	const char* t = strstr(body, "\"tag_name\"");
	char tag[32] = "";
	if (t && (t = strchr(t + 10, '"'))) sscanf(t + 1, "%31[^\"]", tag);
	if (!*tag) return logf("versão: sem resposta da última release de %s (%lu bytes)", repo, n), 0;
	bool newer = ver_num(tag) > ver_num(COJVR_VERSION);
	if (newer) strcpy(g_update_tag, tag);
	logf("versão: instalada %s, última release %s%s", COJVR_VERSION, tag, newer ? " (nova)" : "");
	return 0;
}
}  // namespace

// Abre no navegador padrão a página de relato (which = "report") ou a de download (qualquer outro valor). A seção [links]
// do cojvr.ini pode trocar o repositório (repo) ou a URL de cada página.
void open_page(const char* which) {
	char repo[128], def[256], url[512];
	cfg_str("links", "repo", DEF_REPO, repo, sizeof repo);
	if (!strcmp(which, "report")) snprintf(def, sizeof def, "https://github.com/%s/issues/new/choose", repo);
	else snprintf(def, sizeof def, "https://github.com/%s/releases/latest", repo);
	cfg_str("links", which, def, url, sizeof url);
	HINSTANCE h = ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
	logf("página %s: %s (ShellExecute %d)", which, url, (int)(intptr_t)h);
}

namespace {
// Aviso no menu principal (MenuMainDI): o botão escondido B_QuitArcade (+0x190) é reaproveitado como "Update available"
// e abre a página da release. release_init() confere os bytes originais antes de cada patch.
struct Str {  // string do engine (mesmo layout da GStr de options.cpp)
	char* p;
	unsigned n, cap;
};
extern "C" void game_str(Str* dst, const char* s);  // wrapper em asm definido em options.cpp
typedef void(__thiscall* press_t)(char*, char*, int);
press_t o_menu_press;

void show_button(char* b, const char* txt) {
	Str s;
	game_str(&s, txt);
	((void(__thiscall*)(char*, Str*, int))(*(void***)b)[0x3A8 / 4])(b, &s, 0);  // texto (mesmo slot das linhas "VR:")
	((void(__cdecl*)(void*)) * (void**)0x01258480)(s.p);
	((void(__thiscall*)(char*, int))(*(void***)b)[0x320 / 4])(b, 1);  // SetVisible
}

// MenuMainDI::vf213 (OnShow) chama MenuMain::vf213 (0x01105BA0, thiscall, 1 arg) em 0x0089D840; o hook entra no lugar
// dessa chamada e, depois do original, mostra o aviso se houver versão nova.
void __fastcall hk_menu_show(char* self, void*, int a) {
	((void(__thiscall*)(char*, int))0x01105BA0)(self, a);
	char* b = *(char**)(self + 0x190);
	if (!b || !*g_update_tag) return;
	char txt[80];
	snprintf(txt, sizeof txt, "Update available: %s (opens on the PC)", g_update_tag);
	show_button(b, txt);
	logf("menu principal: aviso de versão %s no botão %p", g_update_tag, (void*)b);
}

// MenuMainDI::vf365 (escolha de botão, 0x0089D9C0, thiscall, ret 8): o original não faz nada para o +0x190.
void __fastcall hk_menu_press(char* self, void*, char* btn, int p) {
	if (btn && *g_update_tag && btn == *(char**)(self + 0x190)) return open_page("update");
	o_menu_press(self, btn, p);
}


// Menu de pausa: o botão "Unlock Full Game" (que só aparece na versão de demonstração) vira o relato de erro/feedback
// sem o mod mexer na UI. O jogo deixa de escondê-lo (jne → jmp em 0x0111E240, no fim do MenuInGame::vf476), o texto vem
// da chave Menu_UnlockFullGame_$ do vr_texts.txt, e o clique (MenuInGameDI::vf493, slot 0x013272A0, thiscall sem
// argumentos, que no jogo abriria a compra) passa a abrir a página de relato.
void __fastcall hk_pause_report(char*) { open_page("report"); }
}  // namespace

void release_init() {
	// Placar: ModeArcade, ModeDuel e ModeDuelMulti (vtables 0x013611D4, 0x01360654, 0x01360BA4; slot 260). expect é a
	// função original do slot; só troca quando ela confere.
	struct Slot {
		uintptr_t slot, expect;
	} const lb[] = {{0x013611D4 + 260 * 4, 0x00A62550}, {0x01360654 + 260 * 4, 0x00A61360}, {0x01360BA4 + 260 * 4, 0x00A61360}};
	int ok = 0;
	for (auto& s : lb)
		if (*(uintptr_t*)s.slot == s.expect) patch_ptr(s.slot, (void*)no_leaderboard), ok++;
	logf("placar online: envio de recordes desligado em VR (%d de 3 modos)", ok);
	uint8_t* jne = (uint8_t*)0x0111E240;
	bool pause_ok = jne[0] == 0x75 && jne[1] == 0x10 && *(uintptr_t*)0x013272A0 == 0x009335E0;
	if (pause_ok) {
		DWORD old;
		VirtualProtect(jne, 1, PAGE_EXECUTE_READWRITE, &old);
		*jne = 0xEB;
		VirtualProtect(jne, 1, old, &old);
		FlushInstructionCache(GetCurrentProcess(), jne, 1);
		patch_ptr(0x013272A0, (void*)hk_pause_report);
	}
	logf("menu de pausa: relato de erro/feedback %s", pause_ok ? "ligado" : "desligado (bytes não conferem)");
	if (!cfg_int("links", "update_check", 1)) return;
	CloseHandle(CreateThread(NULL, 0, update_thread, NULL, 0, NULL));
	bool shown = patch_call(0x0089D840, 0x01105BA0, (void*)hk_menu_show);
	if (*(uintptr_t*)0x012F46C8 == 0x0089D9C0) o_menu_press = (press_t)patch_ptr(0x012F46C8, (void*)hk_menu_press);
	logf("menu principal: aviso de versão %s", shown && o_menu_press ? "ligado" : "desligado (endereços não conferem)");
}
