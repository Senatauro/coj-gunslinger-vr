// Armas realistas (experimento, opção real_guns): o cão, o tambor, a alavanca e a abertura das armas são movidos pela
// mão, e a recarga é bala a bala. Os modelos das armas já têm essas peças como ossos (bone_hammer, bone_magazine,
// bone_trigger_reload, bone_reload_schofield…); o mod escreve o mundo deles depois de pôr a arma na mão
// (weapon_camera), como faz com a manivela da gatling, e decide quando o gatilho chega ao jogo.
// - Polegar da mão da arma (A na direita, X na esquerda): arma o cão, cicla a alavanca ou, com a portinhola aberta, gira
//   o tambor. Segurar o polegar abre ou fecha a arma (portinhola do Colt e do Walker; quebrar a Schofield e a escopeta).
// - A outra mão passando rápido para trás pelo cão do revólver arma o cão (fanning: com o gatilho apertado, cada
//   passada é um tiro). Com o rifle nas duas mãos, dobrar o pulso da mão da arma para baixo e voltar cicla a alavanca.
// - Recarga: com a arma aberta (ou o rifle, pela portinhola lateral), grip da outra mão na cintura pega uma bala, que
//   vai à câmara encostando no ponto de carga; grip no ponto de carga com a mão vazia tira a bala ou o estojo. Cano
//   para cima derruba os estojos. Cada câmara tem bala, estojo ou nada: o cão numa câmara sem bala só dá o clique.
#include "mod.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" float* bone_world(char* obj, int idx);  // weapon.cpp
extern "C" char* bone_node(char* obj, int idx);
extern "C" float* bone_slot(char* node);
struct GStr {
	char* p;
	unsigned n, cap;
};
extern "C" void game_str(GStr* dst, const char* s);  // options.cpp

namespace {
int bone(char* wv, const char* name) {
	char* m = wv ? *(char**)(wv + 4) : NULL;
	return m ? ((int(__thiscall*)(char*, const char*))(*(void***)m)[0x3EC / 4])(m, name) : -1;
}
void inv(const float* M, float* o) {  // inversa de afim rígida 3x4
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) o[r * 4 + c] = M[c * 4 + r];
		o[r * 4 + 3] = -(M[r] * M[3] + M[4 + r] * M[7] + M[8 + r] * M[11]);
	}
}
void mul(const float* A, const float* B, float* out) {
	float o[12];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			o[r * 4 + c] = A[r * 4] * B[c] + A[r * 4 + 1] * B[4 + c] + A[r * 4 + 2] * B[8 + c] + (c == 3 ? A[r * 4 + 3] : 0);
	memcpy(out, o, sizeof o);
}
int parent_of(char* wv, int i) {  // índice do pai de um osso (−1 se o pai está fora do vetor de nós do modelo)
	char *base = bone_node(wv, 0), *pa = *(char**)bone_node(wv, i);
	int pi = pa ? (int)((pa - base) / 0x18) : -1;
	return pa >= base && pi >= 0 && pi < 256 ? pi : -1;
}
void local_of(char* wv, int i, float* L) {  // local de um osso no pai (mundos atuais)
	int pi = parent_of(wv, i);
	float iv[12];
	if (pi < 0) return memcpy(L, bone_world(wv, i), 48), void();
	inv(bone_world(wv, pi), iv), mul(iv, bone_world(wv, i), L);
}
// Gira o osso b para o local R (giro em torno do eixo ax local, graus) mantendo a posição local: mundo = pai · [R | t].
// Esqueleto (S = nó+0x10): +0x0C locais, +0x14 mundos, +0x1C flags (bit 1 = mundo velho, recalculado do pai pelo
// 0x004B61D0, que o bone_world chama; bit 0 = local velho, refeito do mundo). Depois de o mod escrever o HandHolder, os
// ossos da arma ficam com o mundo velho (a arma ainda presa ao braço da animação). O bone_slot (0x0065C3B0) não
// recalcula: só dá o ponteiro e marca o mundo como válido. Por isso o bone_world vem antes, no pai e no osso.
void set_rot(char* wv, int b, int ax, float deg) {
	int pi = b >= 0 ? parent_of(wv, b) : -1;
	if (pi < 0) return;
	const float* P = bone_world(wv, pi);
	bone_world(wv, b);
	float* M = bone_slot(bone_node(wv, b));
	float a = deg * 0.01745329f, c = cosf(a), s = sinf(a), R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	int u = (ax + 1) % 3, v = (ax + 2) % 3;  // colunas u e v giram: u' = u·c + v·s, v' = −u·s + v·c
	R[u * 3 + u] = c, R[v * 3 + u] = s, R[u * 3 + v] = -s, R[v * 3 + v] = c;  // R[linha*3+coluna]
	for (int r = 0; r < 3; r++)
		for (int col = 0; col < 3; col++) M[r * 4 + col] = P[r * 4] * R[col] + P[r * 4 + 1] * R[3 + col] + P[r * 4 + 2] * R[6 + col];
}
float rot_x(char* wv, int b) {  // ângulo do giro local em x (graus), da animação
	float L[12];
	local_of(wv, b, L);
	return atan2f(L[9], L[5]) * 57.29578f;
}
double now_ms() {
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	return t.QuadPart * 1000.0 / f.QuadPart;
}

// Som do jogo pelo vis do jogador: PlayerVis vtable +0x490 (thiscall: &nome, 0, volume, 0.0, 0.0, 0, −1.0, 0), como o
// PlayerVis::vf201 toca o "object_throw.ogg" (0x00983DBA). O nome vai numa string do engine, solta depois pelo free.
void sound(const char* name, float vol = 1) {
	char* vis = weapon_player_vis();
	if (!vis || !setting("real_sound", 1)) return;
	GStr s;
	game_str(&s, name);
	((void(__thiscall*)(char*, GStr*, int, float, float, float, int, float, int))(*(void***)vis)[0x490 / 4])(vis, &s, 0, vol, 0, 0, 0, -1, 0);
	((void(__cdecl*)(void*)) * (void**)0x01258480)(s.p);
}

enum Kind { K_NONE, K_GATE, K_BREAK, K_SHOTGUN, K_LEVER };
const char* const k_kind[] = {"nenhuma", "revólver de portinhola", "revólver de quebrar", "escopeta", "rifle de alavanca"};
enum { EMPTY, LIVE, SPENT };
const char* const k_st[] = {"vazia", "bala", "estojo"};

// --- Munição de cada arma do inventário, câmara por câmara. Fica com o item: trocar de arma e voltar mantém o tambor.
// O jogo só conhece a contagem de balas (item+0x20); o mod mantém a contagem igual ao número de câmaras com bala (mais
// o tubo, no rifle). Bala posta = AddBullet do jogo (tira da reserva); bala tirada = −1 no item e +1 na reserva. ---
struct Ammo {
	char* item;
	int kind;
	int n;              // câmaras: 6 nos revólveres, 2 na escopeta, 1 no rifle (o resto fica no tubo)
	signed char st[6];  // EMPTY, LIVE ou SPENT
	int idx;            // revólver: câmara alinhada com o cano (e com a portinhola, aberta)
	int tube;           // rifle: balas no tubo
};
Ammo g_ammo[8];
int g_ammo_next;

char* item_of(char* p, int k) {
	char* s = *(char**)(p + 0xA84 + 0xC * k);
	return s ? s - 0x14 : NULL;
}
int& mag(char* it) { return *(int*)(it + 0x20); }
int cap_of(char* it) {  // AmmoCount do ItemDescFirearm (vf 0x88 do desc em item+0x2C)
	char* d = it + 0x2C;
	return ((int(__thiscall*)(char*))(*(void***)d)[0x88 / 4])(d);
}
// Reserva do tipo de munição da arma: *(*(PlayerDI+0x4B4)+0x28) + 0x24 + tipo·4; o máximo fica em +0x44 + tipo·4.
int* reserve(char* p, char* it, int* mx = NULL) {
	char* stock = *(char**)(*(char**)(p + 0x4B4) + 0x28);
	char* d = it + 0x2C;
	int t = ((int(__thiscall*)(char*))(*(void***)d)[0x84 / 4])(d);
	if (!stock || t < 0 || t >= 8) return NULL;
	if (mx) *mx = *(int*)(stock + 0x44 + t * 4);
	return (int*)(stock + 0x24 + t * 4);
}
int live_of(const Ammo& a) {
	int n = a.tube;
	for (int i = 0; i < a.n; i++) n += a.st[i] == LIVE;
	return n;
}
// AddBullet (0x00BF3C70, EAX = WFC): +1 bala tirada da reserva, ou o tambor cheio se WFC+0x19D (ClipReload, a Schofield);
// o mod zera o +0x19D durante a chamada para pôr uma de cada vez.
extern "C" void add_bullet_raw(char* wfc);
asm(".globl _add_bullet_raw\n"
    "_add_bullet_raw:\n"
    "  push %ebx\n"
    "  push %esi\n"
    "  push %edi\n"
    "  push %ebp\n"
    "  mov 20(%esp), %eax\n"
    "  mov $0x00BF3C70, %ecx\n"
    "  call *%ecx\n"
    "  pop %ebp\n"
    "  pop %edi\n"
    "  pop %esi\n"
    "  pop %ebx\n"
    "  ret\n");
bool add_bullet(char* p, int k, char* it) {
	char* wfc = *(char**)(p + 0x538 + 4 * k);
	if (!wfc || !it) return false;
	int before = mag(it);
	char clip = wfc[0x19D];
	wfc[0x19D] = 0, add_bullet_raw(wfc), wfc[0x19D] = clip;
	return mag(it) > before;
}
void remove_live(char* p, char* it) {  // bala tirada da arma volta para a reserva
	int mx = 0, *r = reserve(p, it, &mx);
	if (mag(it) > 0) mag(it)--;
	if (r && (*r < mx || mx <= 0)) (*r)++;
}

struct Gun {
	char* wv;
	char* model;          // modelo do WeaponVis (na troca de arma o vis pode ficar e o modelo mudar)
	char* item;
	Ammo* am;
	int kind;
	int b_ham[2], b_cyl, b_lever, b_act, b_load, b_cart, b_ch[6];
	float cock_deg;       // ângulo local do cão armado (x); 0 = abaixado
	float ham[2];         // desenho do cão: 0 abaixado .. 1 armado
	bool cocked[2];       // cão armado (escopeta: [0] direito, [1] esquerdo)
	float cyl;            // desenho do tambor, em câmaras (contínuo)
	int cyl_n;            // câmaras giradas desde a pega (o desenho segue)
	float lever;          // alavanca: 0 fechada .. 1 aberta
	int lever_phase;      // ciclo pelo polegar: 0 parado, 1 abrindo, 2 fechando
	bool lever_open;      // levering pelo pulso: passou do aberto (falta voltar)
	bool open;            // arma aberta para recarregar (portinhola ou quebrada)
	float act;            // desenho da portinhola / dobradiça: 0 fechada .. 1 aberta
	bool tipped;          // cano para cima já contado (rearma ao baixar)
	bool ejected;         // Schofield: o extrator já jogou os estojos ao abrir
	int held;             // na outra mão: EMPTY (nada), LIVE (bala) ou SPENT (estojo)
	bool gprev;           // grip da outra mão no frame anterior
	bool away;            // a mão que tirou uma bala já saiu do ponto de carga (só então pode pôr de volta)
	bool trig, fire;      // gatilho físico apertado (com histerese); tiro pedido ao jogo
	bool shot_hold;       // já atirou (ou clicou) neste aperto: só atira de novo soltando o gatilho ou armando outro cão
	bool pulse;           // gatilho do jogo apertado agora
	double t_edge;        // última troca do pulso (ms)
	bool thumb, long_done;  // polegar apertado; o segurar já virou abrir/fechar
	double t_thumb;       // quando o polegar desceu (ms)
	bool fan_block;       // passada do fanning já contada (rearma quando a mão volta ou sai)
	volatile LONG thumb_req, click_req, open_req;  // pedidos da thread do input; o update executa
	bool reload_btn;      // botão de recarga do jogo (X) apertado, com a arma na outra mão
	int b_port;           // rifle: elevador do cartucho (de onde o estojo sai, por cima)
	struct Fly {          // estojo voando: osso, início (ms), posição inicial (mundo, cm) e velocidade (cm/s)
		int bone;
		double t0;
		float p0[3], v[3];
	} fly[6];
} g[2];
bool g_hidden[2];  // akimbo: a arma desta mão lógica foi para o coldre enquanto a outra recarrega

void eject(Gun& G, int k, int bone, const float* from, float vx, float vy, float vz) {
	const float* A = weapon_aim(k);
	if (bone < 0 || !A || !G.wv) return;
	int s = 0;
	for (int i = 0; i < 6; i++)
		if (G.fly[i].bone == bone || G.fly[i].t0 < G.fly[s].t0) s = i;  // o mesmo osso recomeça; senão o mais antigo
	Gun::Fly& f = G.fly[s];
	const float* W = bone_world(G.wv, bone);
	float j = (float)(rand() % 41 - 20) / 100;  // um pouco de variação
	f.bone = bone, f.t0 = now_ms();
	for (int r = 0; r < 3; r++)
		f.p0[r] = from ? from[r] : W[r * 4 + 3], f.v[r] = A[r * 4] * vx * (1 + j) + A[r * 4 + 1] * vy + A[r * 4 + 2] * vz * (1 - j);
}

bool on() { return setting("real_guns", 0) != 0; }
// A arma da mão lógica k está no modo realista agora: opção ligada, jogo normal (modo 3: o QTE, a Concentração, o
// duelo e a gatling seguem como no jogo) e arma conhecida na mão.
bool active(char* p, int k) {
	return on() && p && player_head_frame() && *(int*)(p + 0x5A8) == 3 && g[k].wv && g[k].kind != K_NONE &&
	       weapon_held(p, k) == g[k].wv && g[k].am;
}
void logr(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void logr(const char* fmt, ...) {
	if (!setting("real_log", 1)) return;
	char b[256];
	va_list a;
	va_start(a, fmt), vsnprintf(b, sizeof b, fmt, a), va_end(a);
	logf("real: %s", b);
}
void haptic(int h, float amp, float s = 0.02f) { vr_haptic(h, s, 160, amp); }

// Estojo (ou bala) saindo da arma: o osso dele voa com a gravidade por real_fly_s, partindo de onde está (ou de from) com
// a velocidade v no frame da arma (cm/s: x esquerda, y cima, z frente). Usa o próprio cartucho do modelo.
struct Gun;
void eject(Gun& G, int k, int bone, const float* from, float vx, float vy, float vz);

// Câmaras de um item, criadas na primeira vez a partir da contagem do jogo: revólver e escopeta com as balas nas
// primeiras câmaras e estojos no resto (a arma já atirou); rifle com uma bala na câmara e o resto no tubo.
Ammo* ammo_for(char* it, int kind) {
	for (Ammo& a : g_ammo)
		if (a.item == it && a.kind == kind) return &a;
	Ammo* f = NULL;
	for (Ammo& a : g_ammo)
		if (a.item == it) f = &a;  // o mesmo item com outro tipo (não deveria acontecer): refaz
	Ammo& a = f ? *f : g_ammo[g_ammo_next++ % 8];
	memset(&a, 0, sizeof a);
	a.item = it, a.kind = kind;
	int c = mag(it);
	a.n = kind == K_SHOTGUN ? 2 : kind == K_LEVER ? 1 : 6;
	if (kind == K_LEVER) a.st[0] = c > 0 ? LIVE : EMPTY, a.tube = c > 0 ? c - 1 : 0;
	else
		for (int i = 0; i < a.n; i++) a.st[i] = i < c ? LIVE : SPENT;
	a.idx = a.n - 1;  // o 1º cão arma a câmara 0
	return &a;
}
// A contagem do jogo mudou por fora (recarga fora do modo 3, munição achada no Arcade…): ajusta as câmaras.
void sync(Gun& G, int k) {
	Ammo& a = *G.am;
	int want = mag(G.item), have = live_of(a);
	if (want == have) return;
	logr("mão lógica %d: o jogo tem %d balas e as câmaras %d; ajustando", k, want, have);
	for (int pass = 0; pass < 2 && have < want; pass++)  // primeiro as vazias, depois os estojos
		for (int i = 0; i < a.n && have < want; i++)
			if (a.st[i] == (pass ? SPENT : EMPTY)) a.st[i] = LIVE, have++;
	if (have < want && a.n == 1) a.tube += want - have, have = want;
	if (have > want && a.n == 1 && a.tube) {
		int d = have - want < a.tube ? have - want : a.tube;
		a.tube -= d, have -= d;
	}
	for (int i = 0; i < a.n && have > want; i++)
		if (a.st[i] == LIVE) a.st[i] = SPENT, have--;
}

const char* snd(const Gun& G, const char* what) {  // som do jogo por arma e evento
	bool walker = G.wv && G.item && strstr(*(const char**)(*(char**)(G.item + 0x2C + 4) + 8), "Walker");
	static const struct { int kind; const char *open, *close, *insert, *turn, *empty; } t[] = {
		{K_GATE, "ss_reload_open.wav", "ss_reload_close.wav", "ss_reload_bullet_insert.wav", "ss_reload_drum_turn_short.wav", "ss_empty_00.wav"},
		{K_BREAK, "qs_reload_open.wav", "qs_reload_close.wav", "qs_reload_bullet_insert.wav", "ss_reload_drum_turn_short.wav", "qs_empty.wav"},
		{K_SHOTGUN, "rmngtn_reload_open_00.wav", "rmngtn_reload_close_00.wav", "rmngtn_reload_put_bullets_00.wav", "rmngtn_reload_draw_bullets_00.wav", "rmngtn_empty_00.wav"},
		{K_LEVER, "wctr_reload_prepare.wav", "wctr_reload_put_down.wav", "wctr_reload_bullet_insert_00.wav", "wctr_rack_00.wav", "wctr_empty_00.wav"}};
	for (auto& e : t)
		if (e.kind == G.kind) {
			const char* s = !strcmp(what, "open") ? e.open : !strcmp(what, "close") ? e.close : !strcmp(what, "insert") ? e.insert
			                : !strcmp(what, "turn") ? e.turn : e.empty;
			if (walker && !strncmp(s, "ss_", 3)) {  // o Walker tem os próprios (wr_*), sem as variações _00
				static char b[48];
				snprintf(b, sizeof b, "wr_%s", s + 3);
				if (char* x = strstr(b, "_00")) memmove(x, x + 3, strlen(x + 3) + 1);
				return b;
			}
			return s;
		}
	return "";
}

void setup(char* p, int k, char* wv) {
	Gun& G = g[k];
	memset(&G, 0, sizeof G);
	G.wv = wv;
	if (!wv) return;
	G.b_ham[0] = bone(wv, "bone_hammer"), G.b_ham[1] = -1, G.b_cyl = bone(wv, "bone_magazine"), G.b_lever = bone(wv, "bone_trigger_reload");
	G.b_act = G.b_load = -1, G.b_cart = bone(wv, "holder_bullet"), G.b_port = bone(wv, "bone_chamber_up");
	for (auto& f : G.fly) f.bone = -1, f.t0 = 0;
	for (int i = 0; i < 6; i++) G.b_ch[i] = -1;
	if (G.b_lever >= 0) {
		G.kind = K_LEVER, G.b_load = bone(wv, "bone_side_ellement");
		if (G.b_load < 0) G.b_load = bone(wv, "bone_reload");  // a serrada não tem a portinhola lateral
	} else if (bone(wv, "bone_hammer_r") >= 0) {
		G.kind = K_SHOTGUN, G.b_ham[0] = bone(wv, "bone_hammer_r"), G.b_ham[1] = bone(wv, "bone_hammer_l");
		G.b_act = bone(wv, "bone_reload_shotgun"), G.b_ch[0] = bone(wv, "bullet_r"), G.b_ch[1] = bone(wv, "bullet_l");
	} else if (bone(wv, "bone_reload_schofield") >= 0) {
		G.kind = K_BREAK, G.b_act = bone(wv, "bone_reload_schofield"), G.b_load = G.b_cyl;
		char nm[32];
		for (int i = 0; i < 6; i++) snprintf(nm, sizeof nm, "holder_schofield_bullet%02d", i), G.b_ch[i] = bone(wv, nm);
	} else if (G.b_ham[0] >= 0 && G.b_cyl >= 0) {
		G.kind = K_GATE, G.b_act = G.b_load = bone(wv, "bone_reload_hammer");
	}
	if (G.b_ham[0] < 0) G.kind = K_NONE;
	// O tipo do item tem de bater com o modelo (na troca de arma o item muda antes do modelo): 0xF revólver, 0x11 rifle,
	// 0x14 escopeta.
	G.model = *(char**)(wv + 4), G.item = item_of(p, k);
	int ty = G.item ? ((int(__thiscall*)(char*))(*(void***)(G.item + 0x2C))[0x44 / 4])(G.item + 0x2C) : -1;
	if (G.kind != (ty == 0x11 ? K_LEVER : ty == 0x14 ? K_SHOTGUN : ty == 0xF ? G.kind : -1) || (ty == 0xF && G.kind != K_GATE && G.kind != K_BREAK))
		G.kind = K_NONE;
	// Ângulo do cão armado: o da pose parada da animação (o jogo segura a arma com o cão armado); o abaixado é o local
	// sem giro. Medido no revólver: −50° (a pose parada) e 0° (logo depois do tiro).
	static const float def[] = {0, -50, -45, -35, -40};
	float a = G.b_ham[0] >= 0 ? rot_x(wv, G.b_ham[0]) : 0;
	G.cock_deg = fabsf(a) > 15 && fabsf(a) < 80 ? a : def[G.kind];
	char key[32];
	snprintf(key, sizeof key, "real_cock_deg_%d", G.kind);
	G.cock_deg = setting(key, G.cock_deg);
	G.am = G.kind != K_NONE && G.item ? ammo_for(G.item, G.kind) : NULL;
	if (G.kind == K_LEVER && G.am && G.am->st[0] == LIVE) G.cocked[0] = true;  // rifle pronto: bala na câmara e cão armado
	char sts[16] = "";
	for (int i = 0; G.am && i < G.am->n; i++) sts[i] = "-BE"[G.am->st[i]];
	logr("mão lógica %d: %s (cão %d/%d, tambor %d, alavanca %d, abre %d, carga %d); cão armado a %.0f° (animação %.0f°); câmaras %s"
	     " tubo %d, jogo %d/%d", k, k_kind[G.kind], G.b_ham[0], G.b_ham[1], G.b_cyl, G.b_lever, G.b_act, G.b_load, G.cock_deg, a, sts,
	     G.am ? G.am->tube : 0, G.item ? mag(G.item) : -1, G.item && G.kind ? cap_of(G.item) : -1);
}

// Câmara que vai atirar: a alinhada (revólver), a do primeiro cão armado (escopeta) ou a do rifle.
int fire_chamber(const Gun& G) { return G.kind == K_GATE || G.kind == K_BREAK ? G.am->idx : G.kind == K_SHOTGUN ? (G.cocked[0] ? 0 : 1) : 0; }

// Armar o cão (polegar ou passada do fanning). Revólver: gira o tambor uma câmara. Escopeta: o próximo cão abaixado.
bool cock(int k, const char* how) {
	Gun& G = g[k];
	if (G.open) return false;
	int i = G.kind == K_SHOTGUN && G.cocked[0] ? 1 : 0;
	if (G.cocked[i] || (G.kind == K_SHOTGUN && G.b_ham[i] < 0)) return false;
	G.cocked[i] = true, G.shot_hold = false;
	if (G.kind == K_GATE || G.kind == K_BREAK) G.cyl_n++, G.am->idx = (G.am->idx + 1) % 6;
	bool right = weapon_phys(k) == 0;
	sound(G.kind == K_BREAK ? (right ? "qs_hammer_pull_right.wav" : "qs_hammer_pull_left.wav")
	      : G.kind == K_SHOTGUN ? "rmngtn_reload_rack_00.wav"
	                            : (right ? "ss_hammer_pull_right_00.wav" : "ss_hammer_pull_left_00.wav"));
	haptic(weapon_phys(k), 0.5f);
	logr("mão lógica %d: cão %d armado (%s); câmara %d: %s", k, i, how, fire_chamber(G), k_st[G.am->st[fire_chamber(G)]]);
	return true;
}

// Alavanca aberta: tira o que estiver na câmara (bala volta para a reserva) e arma o cão. Fechada: a bala seguinte do
// tubo entra na câmara.
void lever_opened(char* p, int k, const char* how) {
	Gun& G = g[k];
	Ammo& a = *G.am;
	if (a.st[0] == LIVE) remove_live(p, G.item);
	const char* was = k_st[a.st[0]];
	if (a.st[0] != EMPTY && G.b_port >= 0) {  // o estojo sai por cima e para a direita
		const float *P = bone_world(G.wv, G.b_port), *A = weapon_aim(k);
		float from[3];
		for (int r = 0; r < 3; r++) from[r] = P[r * 4 + 3] + (A ? A[r * 4 + 1] * 2 : 0);
		eject(G, k, G.b_cart, from, -setting("real_eject_side", 90), setting("real_eject_up", 160), -40);
	}
	a.st[0] = EMPTY, G.cocked[0] = true;
	static int n;
	char nm[24];
	snprintf(nm, sizeof nm, "wctr_rack_%02d.wav", n++ % 3);
	sound(nm), haptic(weapon_phys(k), 0.6f);
	logr("mão lógica %d: alavanca aberta (%s): %s fora, cão armado", k, how, was);
}
void lever_closed(int k, const char* how) {
	Ammo& a = *g[k].am;
	if (a.tube > 0 && a.st[0] == EMPTY) a.tube--, a.st[0] = LIVE;
	g[k].shot_hold = false;  // levering: com o gatilho apertado, cada alavanca fechada é um tiro
	logr("mão lógica %d: alavanca fechada (%s): câmara %s, tubo %d", k, how, k_st[a.st[0]], a.tube);
}

// Abrir / fechar para recarregar (polegar segurado). Revólver de portinhola: a portinhola abre e o cão vai a meio;
// Schofield e escopeta quebram. No akimbo a outra arma vai para o coldre e a mão dela fica livre para as balas.
void toggle_open(char* p, int k) {
	Gun& G = g[k];
	if (G.kind == K_LEVER || G.b_act < 0) return;
	G.open = !G.open, G.cocked[0] = G.cocked[1] = false, G.tipped = G.ejected = false;
	sound(snd(G, G.open ? "open" : "close")), haptic(weapon_phys(k), 0.5f);
	int o = 1 - k;
	if (G.open && weapon_held(p, o) && weapon_phys(o) == 1 - weapon_phys(k)) g_hidden[o] = true;
	if (!G.open) g_hidden[o] = false;
	char sts[16] = "";
	for (int i = 0; i < G.am->n; i++) sts[i] = "-BE"[G.am->st[i]];
	logr("mão lógica %d: arma %s (câmaras %s, alinhada %d)%s", k, G.open ? "aberta" : "fechada", sts, G.am->idx,
	     g_hidden[o] ? "; a outra foi para o coldre" : "");
}

void thumb_action(char* p, int k) {
	Gun& G = g[k];
	if (G.kind == K_LEVER) {
		if (G.lever_phase == 0 && !G.lever_open) G.lever_phase = 1;
	} else if (G.open && G.kind == K_GATE) {  // portinhola aberta: o polegar gira o tambor uma câmara
		G.cyl_n++, G.am->idx = (G.am->idx + 1) % 6, G.tipped = false;
		sound(snd(G, "turn")), haptic(weapon_phys(k), 0.3f);
		logr("mão lógica %d: tambor girado; na portinhola: %s", k, k_st[G.am->st[G.am->idx]]);
	} else {
		cock(k, "polegar");
	}
	(void)p;
}

// Fanning: a palma da outra mão passando para trás perto do cão (velocidade relativa à arma).
void fan(char* p, int k) {
	Gun& G = g[k];
	int h = weapon_phys(k), o = 1 - h;
	if (weapon_held(p, 1 - k) && weapon_phys(1 - k) == o) return;  // a outra mão segura outra arma (akimbo)
	const float *A = weapon_aim(k), *P = weapon_hand_game(o), *vo = weapon_hand_vel(o), *vh = weapon_hand_vel(h);
	if (!A || !P) return;
	const float* H = bone_world(G.wv, G.b_ham[0]);
	float up = setting("real_fan_up", 2), back = setting("real_fan_back", 2), S[3], d2 = 0, vb = 0;
	for (int i = 0; i < 3; i++) {
		S[i] = H[i * 4 + 3] + A[i * 4 + 1] * up - A[i * 4 + 2] * back;
		d2 += (P[i * 4 + 3] - S[i]) * (P[i * 4 + 3] - S[i]);
		vb -= (vo[i] - vh[i]) * A[i * 4 + 2];  // para trás da arma
	}
	float d = sqrtf(d2), r = setting("real_fan_cm", 10);
	static double next;
	if (setting("real_debug", 0) && now_ms() >= next) {  // cão e mão de apoio na cabeça (cm: x direita, y cima, z trás)
		const float* C = player_head_cam();
		float s[3], q[3];
		for (int c = 0; c < 3; c++)
			s[c] = C[c] * (S[0] - C[3]) + C[4 + c] * (S[1] - C[7]) + C[8 + c] * (S[2] - C[11]),
			q[c] = C[c] * (P[3] - C[3]) + C[4 + c] * (P[7] - C[7]) + C[8 + c] * (P[11] - C[11]);
		next = now_ms() + 300, logf("real: fanning: cão %.1f %.1f %.1f, mão %.1f %.1f %.1f, %.1f cm, %.0f cm/s para trás", s[0], s[1], s[2], q[0], q[1],
		                            q[2], d, vb);
	}
	if (!G.fan_block && d < r && vb > setting("real_fan_speed", 80)) {
		G.fan_block = true;
		if (cock(k, "fanning")) logr("fanning: mão a %.1f cm do cão, %.0f cm/s para trás", d, vb);
	} else if (G.fan_block && (d > r + 4 || vb < 0)) {
		G.fan_block = false;
	}
}

// Levering: rifle nas duas mãos, a mão da arma dobra o pulso para baixo em relação ao rifle (como a mão que empurra a
// alavanca) e volta. A alavanca do modelo segue o pulso; passar do aberto arma o cão e volta a fechar.
void wrist_lever(char* p, int k) {
	Gun& G = g[k];
	const float *A = weapon_aim(k), *M = weapon_hand_game(weapon_phys(k));
	if (!A || !M || !weapon_supported(k)) {
		if (G.lever_phase == 0) {
			if (G.lever_open) lever_closed(k, "pulso");
			G.lever = 0, G.lever_open = false;
		}
		return;
	}
	float zh[3] = {-M[2], -M[6], -M[10]}, zy = 0, zz = 0;  // frente da mão (frame do controle: −z = frente)
	for (int i = 0; i < 3; i++) zy += zh[i] * A[i * 4 + 1], zz += zh[i] * A[i * 4 + 2];
	float down = -atan2f(zy, zz) * 57.29578f, a0 = setting("real_lever_wrist0", 12), a1 = setting("real_lever_wrist1", 35);
	float l = fminf(1, fmaxf(0, (down - a0) / (a1 - a0)));
	G.lever = l;
	if (l > 0.9f && !G.lever_open) G.lever_open = true, lever_opened(p, k, "pulso");
	else if (l < 0.1f && G.lever_open) G.lever_open = false, lever_closed(k, "pulso");
}

// Ponto de carga no mundo (portinhola, boca do tambor aberto, cano da escopeta mais perto da mão, portinhola do rifle).
// ch = câmara desse ponto para tirar (want = −1) ou pôr (want = EMPTY); −1 se não há.
bool load_point(const Gun& G, const float* hand, int want, float pos[3], int* ch) {
	*ch = -1;
	const Ammo& a = *G.am;
	if (G.kind == K_SHOTGUN) {
		float best = 1e9f;
		for (int i = 0; i < 2; i++) {
			int b = G.b_ch[i] >= 0 ? G.b_ch[i] : G.b_act;
			if (b < 0 || (want == EMPTY ? a.st[i] != EMPTY : a.st[i] == EMPTY)) continue;
			const float* W = bone_world(G.wv, b);
			float d = 0;
			for (int c = 0; c < 3; c++) d += (W[c * 4 + 3] - hand[c * 4 + 3]) * (W[c * 4 + 3] - hand[c * 4 + 3]);
			if (d < best) best = d, *ch = i, pos[0] = W[3], pos[1] = W[7], pos[2] = W[11];
		}
		return *ch >= 0;
	}
	int b = G.b_load >= 0 ? G.b_load : G.b_act;
	if (b < 0) return false;
	const float* W = bone_world(G.wv, b);
	pos[0] = W[3], pos[1] = W[7], pos[2] = W[11];
	if (G.kind == K_LEVER) return *ch = 0, true;
	if (G.kind == K_GATE) return (want == EMPTY ? a.st[a.idx] == EMPTY : a.st[a.idx] != EMPTY) && (*ch = a.idx, true);
	for (int pass = 0; pass < 2 && *ch < 0; pass++)  // Schofield: qualquer câmara (para tirar, os estojos primeiro)
		for (int i = 0; i < 6 && *ch < 0; i++)
			if (want == EMPTY ? a.st[i] == EMPTY : a.st[i] == (pass ? LIVE : SPENT)) *ch = i;
	return *ch >= 0;
}

// Recarga pela outra mão: grip na cintura pega uma bala da reserva (a bala aparece na mão); com a bala, encostar no
// ponto de carga põe a bala; com a mão vazia, grip no ponto de carga tira o que estiver lá (bala volta à reserva,
// estojo some). Soltar o grip larga o que estiver na mão.
void loader(char* p, int k) {
	Gun& G = g[k];
	int o = 1 - weapon_phys(k);
	bool other = weapon_held(p, 1 - k) && weapon_phys(1 - k) == o;
	const float* H = weapon_hand_game(o);
	if (other || !H) return void(G.held = EMPTY);
	bool grip = vr_grip(o), edge = grip && !G.gprev;
	G.gprev = grip;
	if (!grip) {
		if (G.held) logr("mão lógica %d: %s", k, G.held == LIVE ? "bala largada" : "estojo largado");
		return void(G.held = EMPTY);
	}
	if (!G.open && G.kind != K_LEVER) return;
	float pos[3], r = setting("real_load_cm", 5), b[3];
	int ch;
	static double next;
	if (setting("real_debug", 0) && now_ms() >= next && load_point(G, H, G.held == LIVE ? EMPTY : -1, pos, &ch)) {
		const float* C = player_head_cam();  // ponto de carga e mão na cabeça (cm: x direita, y cima, z trás)
		float s[3], q[3];
		for (int c = 0; c < 3; c++)
			s[c] = C[c] * (pos[0] - C[3]) + C[4 + c] * (pos[1] - C[7]) + C[8 + c] * (pos[2] - C[11]),
			q[c] = C[c] * (H[3] - C[3]) + C[4 + c] * (H[7] - C[7]) + C[8 + c] * (H[11] - C[11]);
		next = now_ms() + 300, logf("real: carga: ponto %.1f %.1f %.1f (câmara %d), mão %.1f %.1f %.1f", s[0], s[1], s[2], ch, q[0], q[1], q[2]);
	}
	auto dist = [&](const float* q) {
		float d = 0;
		for (int c = 0; c < 3; c++) d += (q[c] - H[c * 4 + 3]) * (q[c] - H[c * 4 + 3]);
		return sqrtf(d);
	};
	if (edge && !G.held) {
		int mx = 0, *res = reserve(p, G.item, &mx);
		if (G.kind != K_LEVER && load_point(G, H, -1, pos, &ch) && dist(pos) < r * 1.5f) {
			G.held = G.am->st[ch], G.away = false;
			if (G.held == LIVE) remove_live(p, G.item);
			G.am->st[ch] = EMPTY;
			sound(snd(G, G.kind == K_SHOTGUN ? "turn" : "insert"), 0.6f), haptic(o, 0.4f);
			logr("mão lógica %d: %s tirad%s da câmara %d", k, k_st[G.held], G.held == LIVE ? "a" : "o", ch);
		} else if (res && *res > 0 && (G.kind == K_LEVER ? mag(G.item) < cap_of(G.item) : live_of(*G.am) < G.am->n)) {
			// Onde pegar a bala: abaixo do peito (cinto e bolsos; sentado, o Carlos pegou a 25–33 cm abaixo dos olhos) ou,
			// com a arma aberta, qualquer grip longe da arma. No rifle (que não abre) a mão de apoio no guarda-mão não conta.
			const float* Hg = weapon_hand_game(weapon_phys(k));
			bool low = weapon_hand_body(o, b) && b[1] < setting("real_pouch_y", -20);
			bool off = G.kind != K_LEVER && Hg && sqrtf((Hg[3] - H[3]) * (Hg[3] - H[3]) + (Hg[7] - H[7]) * (Hg[7] - H[7]) +
			                                            (Hg[11] - H[11]) * (Hg[11] - H[11])) > setting("real_take_cm", 20);
			if ((low || off) && !weapon_supported(k)) {
				G.held = LIVE, G.away = true, haptic(o, 0.3f);
				logr("mão lógica %d: bala pega %s (reserva %d)", k, low ? "na cintura" : "longe da arma", *res);
			}
		}
	}
	if (G.held != LIVE) return;
	if (G.kind == K_LEVER ? mag(G.item) < cap_of(G.item) && load_point(G, H, EMPTY, pos, &ch) : load_point(G, H, EMPTY, pos, &ch)) {
		if (!G.away) return void(G.away = dist(pos) > 2 * r);
		if (dist(pos) >= r) return;
		if (!add_bullet(p, k, G.item)) return logr("mão lógica %d: AddBullet não pôs a bala (reserva vazia?)", k), void(G.held = EMPTY);
		if (G.kind == K_LEVER) G.am->tube++;
		else G.am->st[ch] = LIVE;
		G.held = EMPTY;
		sound(snd(G, "insert")), haptic(o, 0.5f), haptic(weapon_phys(k), 0.3f);
		logr("mão lógica %d: bala posta (%s %d); jogo %d/%d", k, G.kind == K_LEVER ? "tubo" : "câmara", G.kind == K_LEVER ? G.am->tube : ch,
		     mag(G.item), cap_of(G.item));
	}
}

// Arma aberta com o cano para cima: os estojos caem (o da portinhola, no revólver de portinhola; todos na Schofield e na
// escopeta). Bala não cai.
void tip(int k) {
	Gun& G = g[k];
	const float* A = weapon_aim(k);
	if (!G.open || !A || G.act < 0.9f) return;
	bool up = A[6] > setting("real_tip", 0.6f);  // y do eixo do cano (z do Holder)
	if (!up) return void(G.tipped = false);
	if (G.tipped) return;
	G.tipped = true;
	int n = 0;
	for (int i = 0; i < G.am->n; i++)
		if (G.am->st[i] == SPENT && (G.kind != K_GATE || i == G.am->idx)) {
			G.am->st[i] = EMPTY, n++;
			if (G.b_ch[i] >= 0) eject(G, k, G.b_ch[i], NULL, 0, 0, -30);  // escopeta: o cartucho escorrega para fora da culatra
			else if (G.kind == K_GATE && G.b_load >= 0) {  // revólver: o estojo cai pela portinhola
				const float* P = bone_world(G.wv, G.b_load);
				float from[3] = {P[3], P[7], P[11]};
				eject(G, k, G.b_cart, from, -20, 0, -20);
			}
		}
	if (n) logr("mão lógica %d: %d estojo(s) caíram", k, n), haptic(weapon_phys(k), 0.3f);
}

// Cadência: o intervalo mínimo entre tiros fica no WFC (+0x50, e +0x180 na mira; FUN_00BE87E0 lê), copiado do ShotTime
// do item ao equipar (Sixshooter 0,275 s, Winchester 0,9 s). Nas armas realistas quem dita o ritmo é a mão (cão,
// alavanca), então o mod baixa esse intervalo para real_shot_time enquanto a arma está no modo realista e devolve o do
// jogo depois. Só no WFC do jogador: a descrição do item é a mesma dos inimigos.
struct Rate {
	char *wfc, *item;
	float t50, t180;
} g_rate[2];
void cadence(char* p, int k, bool on_) {
	char *w = *(char**)(p + 0x538 + 4 * k), *it = item_of(p, k);
	Rate& r = g_rate[k];
	if (r.wfc && (r.wfc != w || r.item != it || !on_)) {  // devolve o do jogo (só se ainda for a mesma arma no mesmo WFC)
		if (r.wfc == w && r.item == it) *(float*)(w + 0x50) = r.t50, *(float*)(w + 0x180) = r.t180;
		r.wfc = NULL;
	}
	if (!on_ || !w) return;
	if (!r.wfc) {  // o normal vem do item (ShotTime, vf38 do desc): na troca de arma o WFC pode ainda ter o valor do mod
		r.wfc = w, r.item = it, r.t180 = *(float*)(w + 0x180);
		r.t50 = it ? ((float(__thiscall*)(char*))(*(void***)(it + 0x2C))[38])(it + 0x2C) : *(float*)(w + 0x50);
		logr("mão lógica %d: cadência do jogo %.3f s (mira %.3f s) -> %.3f s", k, r.t50, r.t180, setting("real_shot_time", 0.12f));
	}
	float m = setting("real_shot_time", 0.12f);
	*(float*)(w + 0x50) = fminf(r.t50, m), *(float*)(w + 0x180) = fminf(r.t180, m);
}
}  // namespace

// Fim do weapon_frame (thread do update): troca de arma, gestos e a animação das peças.
void real_frame(char* p, float dt) {
	for (int k = 0; k < 2; k++) {
		char* wv = on() ? weapon_held(p, k) : NULL;
		if (wv != g[k].wv || (wv && (*(char**)(wv + 4) != g[k].model || item_of(p, k) != g[k].item))) setup(p, k, wv);
		Gun& G = g[k];
		cadence(p, k, active(p, k));
		if (!active(p, k)) continue;
		sync(G, k);
		int h = weapon_phys(k);
		if (InterlockedExchange(&G.thumb_req, 0)) thumb_action(p, k);
		if (InterlockedExchange(&G.open_req, 0)) toggle_open(p, k);  // X (o botão de recarga do jogo) abre e fecha
		if (G.thumb && !G.long_done && now_ms() - G.t_thumb >= setting("real_long_ms", 450)) G.long_done = true, toggle_open(p, k);
		const char* fk = h ? "real_thumb_l" : "real_thumb_r";  // teste sem headset: polegar e polegar segurado por SET
		if (setting(fk, 0)) setting_set(fk, 0), thumb_action(p, k);
		fk = h ? "real_long_l" : "real_long_r";
		if (setting(fk, 0)) setting_set(fk, 0), toggle_open(p, k);
		if (setting("real_fan", 0)) setting_set("real_fan", 0), cock(k, "SET real_fan");
		if (InterlockedExchange(&G.click_req, 0)) {  // cão caiu numa câmara sem bala
			int i = G.kind == K_SHOTGUN && !G.cocked[0] ? 1 : 0;
			G.cocked[i] = false;
			sound(snd(G, "empty")), haptic(h, 0.2f);
			logr("mão lógica %d: clique (câmara %d: %s)", k, fire_chamber(G), k_st[G.am->st[fire_chamber(G)]]);
		}
		if ((G.kind == K_GATE || G.kind == K_BREAK) && !G.open && !G.held) fan(p, k);
		loader(p, k), tip(k);
		for (int i = 0; i < 2; i++) {
			float t = G.open ? (G.kind == K_GATE ? setting("real_half_cock", 0.35f) : 0) : G.cocked[i] ? 1.f : 0.f;
			float sp = dt / (t > G.ham[i] ? setting("real_cock_s", 0.06f) : 0.015f);
			G.ham[i] += fmaxf(-sp, fminf(sp, t - G.ham[i]));
		}
		float sa = dt / setting("real_open_s", 0.15f);
		G.act += fmaxf(-sa, fminf(sa, (G.open ? 1.f : 0.f) - G.act));
		if (G.kind == K_BREAK && G.open && G.act >= 1 && !G.ejected) {  // a Schofield aberta joga os estojos (extrator)
			int n = 0;
			for (int i = 0; i < 6; i++)
				if (G.am->st[i] == SPENT) G.am->st[i] = EMPTY, n++, eject(G, k, G.b_ch[i], NULL, 0, 60, -120);
			G.ejected = true;
			if (n) logr("mão lógica %d: o extrator jogou %d estojo(s)", k, n), haptic(h, 0.3f);
		}
		G.cyl += fminf(dt / 0.06f, fmaxf(0, G.cyl_n - G.cyl));
		if (G.kind == K_LEVER) {
			float sp = dt / setting("real_lever_s", 0.12f);
			if (G.lever_phase == 1 && (G.lever += sp) >= 1) G.lever = 1, G.lever_phase = 2, lever_opened(p, k, "polegar");
			else if (G.lever_phase == 2 && (G.lever -= sp) <= 0) G.lever = 0, G.lever_phase = 0, lever_closed(k, "polegar");
			else if (G.lever_phase == 0) wrist_lever(p, k);
		}
	}
	for (int k = 0; k < 2; k++)  // a arma do coldre volta quando a outra fecha ou sai da mão
		if (g_hidden[k] && !(g[1 - k].open && active(p, 1 - k))) g_hidden[k] = false;
}

// No weapon_pad (thread do input), depois do mapeamento dos gatilhos: o gatilho do jogo da mão lógica k (cur[6] para a
// 0, cur[5] para a 1) vira um pulso enquanto houver tiro pedido. O jogo atira na borda do gatilho e ignora a borda
// durante o tiro anterior, então o pulso se repete até o Shoot chegar (real_shot). Cão numa câmara sem bala = clique
// do mod, sem tiro. O X (recarga do jogo) não passa: a recarga é a manual.
void real_pad(char* p, LONG* cur) {
	double t = now_ms();
	for (int k = 0; k < 2; k++) {
		Gun& G = g[k];
		if (!active(p, k)) {
			G.fire = G.pulse = false;
			continue;
		}
		cur[0] &= ~XINPUT_GAMEPAD_X;
		float tr = weapon_trigger(k);
		G.trig = G.trig ? tr > 0.35f : tr > 0.6f;
		bool ready = !G.open && (G.kind == K_LEVER ? G.cocked[0] && G.lever < 0.05f : G.cocked[0] || G.cocked[1]);
		if (!G.trig) G.fire = G.shot_hold = false;
		else if (ready && !G.fire && !G.shot_hold) {
			if (G.am->st[fire_chamber(G)] == LIVE) G.fire = true, logr("mão lógica %d: tiro pedido", k);
			else if (!G.click_req) G.shot_hold = true, InterlockedExchange(&G.click_req, 1);
		}
		if (G.fire && !G.pulse && t - G.t_edge >= 30) G.pulse = true, G.t_edge = t;
		else if (G.pulse && (!G.fire || t - G.t_edge >= 45)) G.pulse = false, G.t_edge = t;
		cur[k ? 5 : 6] = G.pulse ? 255 : 0;
	}
}

// Shoot do jogador (hk_shoot): o cão cai e a câmara fica com o estojo.
void real_shot(int k, bool fired) {
	Gun& G = g[k & 1];
	char* p = player_local();
	if (!active(p, k)) return;
	G.fire = G.pulse = false, G.shot_hold = true, G.t_edge = now_ms();
	int ch = fire_chamber(G), i = G.kind == K_SHOTGUN ? ch : 0;
	G.cocked[i] = false;
	if (fired) G.am->st[ch] = SPENT;
	logr("mão lógica %d: %s, cão %d abaixado; câmara %d com estojo; jogo %d", k, fired ? "tiro" : "tiro em seco", i, ch, mag(G.item));
}

// Botões crus do VR (antes do mapeamento): o polegar da mão de cada arma realista (A na direita, X na esquerda) é do
// mod. Apertar = armar o cão / ciclar a alavanca / girar o tambor aberto; segurar por real_long_ms = abrir ou fechar a
// arma para recarregar.
WORD real_buttons(WORD vr) {
	char* p = player_local();
	WORD eat = 0;
	double t = now_ms();
	for (int k = 0; k < 2; k++) {
		Gun& G = g[k];
		if (!active(p, k)) {
			G.thumb = false;
			continue;
		}
		WORD bit = weapon_phys(k) == 0 ? XINPUT_GAMEPAD_A : XINPUT_GAMEPAD_X;
		bool down = vr & bit;
		eat |= bit;
		if (down && !G.thumb) G.t_thumb = t, G.long_done = false, InterlockedExchange(&G.thumb_req, 1);
		G.thumb = down;
		// Arma só na direita: o X (recarga do jogo, na mão esquerda livre) abre e fecha a arma, como atalho do segurar o A.
		bool other = active(p, 1 - k) && weapon_phys(1 - k) == 1;
		if (weapon_phys(k) == 0 && !other) {
			bool x = vr & XINPUT_GAMEPAD_X;
			if (x && !G.reload_btn) InterlockedExchange(&G.open_req, 1);
			G.reload_btn = x, eat |= XINPUT_GAMEPAD_X;
		}
	}
	return eat;
}

// A arma da mão lógica k (akimbo) está no coldre enquanto a outra recarrega.
bool real_hidden(int k) { return g_hidden[k & 1]; }
// A recarga do jogo não vale para a arma realista da mão lógica k.
bool real_owns(int k) { return active(player_local(), k & 1); }
// A mão física h está recarregando a arma da outra (não vira apoio de duas mãos).
bool real_hand_busy(int h) {
	char* p = player_local();
	for (int k = 0; k < 2; k++)
		if (active(p, k) && weapon_phys(k) == 1 - h && (g[k].open || g[k].held)) return true;
	return false;
}

// Depois de a arma da mão lógica k estar posta na mão (weapon_camera): as peças no estado do mod.
void real_camera(char* p, int k, char* wv) {
	Gun& G = g[k & 1];
	if (wv && G.wv == wv && active(p, k)) {
		for (int i = 0; i < 2; i++)
			if (G.b_ham[i] >= 0) set_rot(wv, G.b_ham[i], 0, G.cock_deg * G.ham[i]);
		if ((G.kind == K_GATE || G.kind == K_BREAK) && G.b_cyl >= 0) set_rot(wv, G.b_cyl, 2, -60 * setting("real_cyl_dir", 1) * G.cyl);
		if (G.kind == K_LEVER && G.b_lever >= 0) set_rot(wv, G.b_lever, 0, -setting("real_lever_deg", 50) * G.lever);
		if (G.b_act >= 0 && G.kind != K_LEVER) {  // portinhola (giro e eixo a conferir) ou dobradiça
			static const char* const ax[] = {"", "real_gate_ax", "real_break_ax", "real_shotgun_ax"};
			static const char* const dg[] = {"", "real_gate_deg", "real_break_deg", "real_shotgun_deg"};
			static const float dax[] = {0, 2, 0, 0}, ddg[] = {0, 80, 50, 40};
			set_rot(wv, G.b_act, (int)setting(ax[G.kind], dax[G.kind]), setting(dg[G.kind], ddg[G.kind]) * G.act);
		}
		// Estojos voando (gravidade no y do mundo), girando em torno do x da arma.
		double tn = now_ms();
		float life = setting("real_fly_s", 0.6f) * 1000;
		const float* A = weapon_aim(k);
		bool flying[64] = {};
		for (auto& f : G.fly) {
			if (f.bone < 0 || tn - f.t0 > life || !A) continue;
			float t = (float)(tn - f.t0) / 1000, sp = t * 12, c = cosf(sp), sn = sinf(sp);
			bone_world(wv, f.bone);
			float* M = bone_slot(bone_node(wv, f.bone));
			for (int r = 0; r < 3; r++) {
				M[r * 4] = A[r * 4], M[r * 4 + 1] = A[r * 4 + 1] * c + A[r * 4 + 2] * sn, M[r * 4 + 2] = -A[r * 4 + 1] * sn + A[r * 4 + 2] * c;
				M[r * 4 + 3] = f.p0[r] + f.v[r] * t + (r == 1 ? -490 * t * t : 0);
			}
			if (f.bone < 64) flying[f.bone] = true;
		}
		// Câmaras vazias da Schofield e da escopeta: a bala do modelo some (escala ε, como os braços).
		for (int i = 0; i < 6; i++) {
			int b = G.b_ch[i];
			if (b < 0 || G.am->st[i] != EMPTY || (b < 64 && flying[b])) continue;
			bone_world(wv, b);
			float* M = bone_slot(bone_node(wv, b));
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++) M[r * 4 + c] *= 0.001f;
		}
		// Bala (ou estojo) na outra mão: o cartucho do próprio modelo (holder_bullet, o da animação de recarga) vai para a mão.
		const float* Hh = weapon_hand_game(1 - weapon_phys(k));
		if (G.held && G.b_cart >= 0 && Hh && !(G.b_cart < 64 && flying[G.b_cart])) {
			bone_world(wv, G.b_cart);
			float* M = bone_slot(bone_node(wv, G.b_cart));
			memcpy(M, Hh, 48);
			for (int r = 0; r < 3; r++) M[r * 4 + 3] += Hh[r * 4 + 1] * setting("real_cart_up", 4) - Hh[r * 4 + 2] * setting("real_cart_fwd", 1);
		}
		static double next;
		if (setting("real_debug", 0) && now_ms() >= next)
			next = now_ms() + 500, logf("real: desenho mão %d: cão %.2f (armado %d) -> %.1f°, tambor %.2f, alavanca %.2f, abre %.2f", k, G.ham[0],
			                            G.cocked[0], rot_x(wv, G.b_ham[0]), G.cyl, G.lever, G.act);
	}
	// Diagnóstico: SET real_test_bone <índice> + real_test_deg + real_test_axis gira um osso da arma da mão 0.
	int tb = (int)setting("real_test_bone", -1);
	if (k == 0 && wv && tb >= 0) set_rot(wv, tb, (int)setting("real_test_axis", 0), setting("real_test_deg", 0));
	if (wv && setting("real_dump", 0)) {  // SET real_dump 1: ossos da arma com o pai e o local
		static const char* const names[] = {"holder", "barrel", "bone_root", "bone_hammer", "bone_hammer_l", "bone_hammer_r",
		                                    "bone_trigger", "bone_trigger_reload", "bone_magazine", "bone_magazine_element",
		                                    "bone_reload_hammer", "bone_reload_schofield", "bone_reload_shotgun", "bone_chamber_up",
		                                    "bone_reload", "bone_side_ellement", "holder_bullet", "holder_bullet1", "holder_bullet_b",
		                                    "full_bullet", "bullet_l", "bullet_r", "holder_schofield_bullet00", "schofield_full_bullet"};
		for (const char* nm : names) {
			int i = bone(wv, nm);
			if (i < 0) continue;
			float L[12];
			local_of(wv, i, L);
			logf("real: mão %d osso %-26s %3d pai %3d | local %6.2f %6.2f %6.2f | x %5.2f %5.2f %5.2f y %5.2f %5.2f %5.2f", k, nm, i,
			     parent_of(wv, i), L[3], L[7], L[11], L[0], L[4], L[8], L[1], L[5], L[9]);
		}
		if (k == 1 || !weapon_held(p, 1)) setting_set("real_dump", 0);
	}
}
