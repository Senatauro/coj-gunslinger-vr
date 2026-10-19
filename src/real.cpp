// Armas realistas (experimento, opção real_guns): o cão, o tambor e a alavanca das armas são movidos pela mão. Os
// modelos das armas já têm essas peças como ossos (bone_hammer, bone_magazine, bone_trigger_reload…); o mod escreve o
// mundo deles depois de pôr a arma na mão (weapon_camera), como faz com a manivela da gatling, e decide quando o
// gatilho chega ao jogo: revólver e escopeta só atiram com o cão armado; o rifle só com a alavanca ciclada depois do
// tiro. Polegar da mão da arma (A na direita, X na esquerda) arma o cão ou cicla a alavanca; a outra mão passando
// rápido pelo cão do revólver arma o cão (fanning: com o gatilho apertado, cada passada é um tiro); com o rifle nas
// duas mãos, dobrar o pulso da mão da arma para baixo e voltar cicla a alavanca (levering).
#include "mod.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
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
struct Gun {
	char* wv;
	int kind;
	int b_ham[2], b_cyl, b_lever;
	float cock_deg;       // ângulo local do cão armado (x); 0 = abaixado
	float ham[2];         // desenho do cão: 0 abaixado .. 1 armado
	bool cocked[2];       // cão armado (escopeta: [0] direito, [1] esquerdo)
	float cyl;            // desenho do tambor, em câmaras (contínuo)
	int cyl_n;            // câmaras giradas desde a pega
	float lever;          // alavanca: 0 fechada .. 1 aberta
	int lever_phase;      // ciclo pelo polegar: 0 parado, 1 abrindo, 2 fechando
	bool lever_open;      // levering pelo pulso: passou do aberto (falta voltar)
	bool need_cycle;      // rifle: atirou, falta a alavanca
	bool trig, fire;      // gatilho físico apertado (com histerese); tiro pedido ao jogo
	bool pulse;           // gatilho do jogo apertado agora
	double t_edge;        // última troca do pulso (ms)
	bool thumb;           // polegar apertado (borda no real_buttons)
	double t_thumb;       // quando o polegar desceu (ms; segurar = recarga do jogo enquanto não há a manual)
	bool reload_btn;      // polegar segurado: o X vai para o jogo (recarga)
	bool fan_block;       // passada do fanning já contada (rearma quando a mão volta ou sai)
	volatile LONG thumb_req;  // polegar pedido pela thread do input; o update executa
} g[2];

bool on() { return setting("real_guns", 0) != 0; }
// A arma da mão lógica k está no modo realista agora: opção ligada, jogo normal (modo 3: o QTE, a Concentração, o
// duelo e a gatling seguem como no jogo) e arma conhecida na mão.
bool active(char* p, int k) {
	return on() && p && player_head_frame() && *(int*)(p + 0x5A8) == 3 && g[k].wv && g[k].kind != K_NONE &&
	       weapon_held(p, k) == g[k].wv;
}
void logr(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
void logr(const char* fmt, ...) {
	if (!setting("real_log", 1)) return;
	char b[256];
	va_list a;
	va_start(a, fmt), vsnprintf(b, sizeof b, fmt, a), va_end(a);
	logf("real: %s", b);
}

void setup(int k, char* wv) {
	Gun& G = g[k];
	memset(&G, 0, sizeof G);
	G.wv = wv;
	if (!wv) return;
	G.b_ham[0] = bone(wv, "bone_hammer"), G.b_ham[1] = -1, G.b_cyl = bone(wv, "bone_magazine"), G.b_lever = bone(wv, "bone_trigger_reload");
	if (G.b_lever >= 0) G.kind = K_LEVER;
	else if (bone(wv, "bone_hammer_r") >= 0) G.kind = K_SHOTGUN, G.b_ham[0] = bone(wv, "bone_hammer_r"), G.b_ham[1] = bone(wv, "bone_hammer_l");
	else if (bone(wv, "bone_reload_schofield") >= 0) G.kind = K_BREAK;
	else if (G.b_ham[0] >= 0 && G.b_cyl >= 0) G.kind = K_GATE;
	if (G.b_ham[0] < 0) G.kind = K_NONE;
	// Ângulo do cão armado: o da pose parada da animação (o jogo segura a arma com o cão armado); o abaixado é o local
	// sem giro. Medido no revólver: −50° (a pose parada) e 0° (logo depois do tiro).
	static const float def[] = {0, -50, -45, -35, -40};
	float a = G.b_ham[0] >= 0 ? rot_x(wv, G.b_ham[0]) : 0;
	G.cock_deg = fabsf(a) > 15 && fabsf(a) < 80 ? a : def[G.kind];
	char key[32];
	snprintf(key, sizeof key, "real_cock_deg_%d", G.kind);
	G.cock_deg = setting(key, G.cock_deg);
	logr("mão lógica %d: %s (cão %d/%d, tambor %d, alavanca %d); cão armado a %.0f° (animação %.0f°)", k, k_kind[G.kind], G.b_ham[0],
	     G.b_ham[1], G.b_cyl, G.b_lever, G.cock_deg, a);
}

void haptic(int k, float amp) { vr_haptic(weapon_phys(k), 0.02f, 160, amp); }

// Armar o cão (polegar ou passada do fanning). Revólver: gira o tambor uma câmara. Escopeta: o próximo cão abaixado.
bool cock(int k, const char* how) {
	Gun& G = g[k];
	int i = G.kind == K_SHOTGUN && G.cocked[0] ? 1 : 0;
	if (G.cocked[i] || (G.kind == K_SHOTGUN && G.b_ham[i] < 0)) return false;
	G.cocked[i] = true;
	if (G.kind == K_GATE || G.kind == K_BREAK) G.cyl_n++;
	bool right = weapon_phys(k) == 0;
	sound(G.kind == K_BREAK ? (right ? "qs_hammer_pull_right.wav" : "qs_hammer_pull_left.wav")
	      : G.kind == K_SHOTGUN ? "rmngtn_reload_rack_00.wav"
	                            : (right ? "ss_hammer_pull_right_00.wav" : "ss_hammer_pull_left_00.wav"));
	haptic(k, 0.5f);
	logr("mão lógica %d: cão %d armado (%s)", k, i, how);
	return true;
}

// Alavanca aberta: ejeta o estojo e arma o cão; fechada: o cartucho seguinte na câmara.
void lever_opened(int k, const char* how) {
	Gun& G = g[k];
	G.cocked[0] = true, G.need_cycle = false;
	static int n;
	char nm[24];
	snprintf(nm, sizeof nm, "wctr_rack_%02d.wav", n++ % 3);
	sound(nm), haptic(k, 0.6f);
	logr("mão lógica %d: alavanca aberta (%s): estojo fora, cão armado", k, how);
}

void thumb_action(int k) {
	Gun& G = g[k];
	if (G.kind == K_LEVER) {
		if (G.lever_phase == 0 && !G.lever_open) G.lever_phase = 1;
	} else {
		cock(k, "polegar");
	}
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
void wrist_lever(int k) {
	Gun& G = g[k];
	const float *A = weapon_aim(k), *M = weapon_hand_game(weapon_phys(k));
	if (!A || !M || !weapon_supported(k)) {
		if (G.lever_phase == 0) G.lever = 0, G.lever_open = false;
		return;
	}
	float zh[3] = {-M[2], -M[6], -M[10]}, zy = 0, zz = 0;  // frente da mão (frame do controle: −z = frente)
	for (int i = 0; i < 3; i++) zy += zh[i] * A[i * 4 + 1], zz += zh[i] * A[i * 4 + 2];
	float down = -atan2f(zy, zz) * 57.29578f, a0 = setting("real_lever_wrist0", 12), a1 = setting("real_lever_wrist1", 35);
	float l = fminf(1, fmaxf(0, (down - a0) / (a1 - a0)));
	G.lever = l;
	if (l > 0.9f && !G.lever_open) G.lever_open = true, lever_opened(k, "pulso");
	else if (l < 0.1f && G.lever_open) G.lever_open = false, logr("mão lógica %d: alavanca fechada (pulso)", k);
}
}  // namespace

// Fim do weapon_frame (thread do update): troca de arma, gestos e a animação das peças.
void real_frame(char* p, float dt) {
	for (int k = 0; k < 2; k++) {
		char* wv = on() ? weapon_held(p, k) : NULL;
		if (wv != g[k].wv) setup(k, wv);
		Gun& G = g[k];
		if (!active(p, k)) continue;
		if (InterlockedExchange(&G.thumb_req, 0)) thumb_action(k);
		const char* fk = weapon_phys(k) ? "real_thumb_l" : "real_thumb_r";  // teste sem headset: polegar por SET
		if (setting(fk, 0)) setting_set(fk, 0), thumb_action(k);
		if (setting("real_fan", 0)) setting_set("real_fan", 0), cock(k, "SET real_fan");
		if (G.kind == K_GATE || G.kind == K_BREAK) fan(p, k);
		for (int i = 0; i < 2; i++) {
			float t = G.cocked[i] ? 1.f : 0.f, sp = dt / (G.cocked[i] ? setting("real_cock_s", 0.06f) : 0.015f);
			G.ham[i] += fmaxf(-sp, fminf(sp, t - G.ham[i]));
		}
		G.cyl += fminf(dt / 0.06f, fmaxf(0, G.cyl_n - G.cyl));
		if (G.kind == K_LEVER) {
			float sp = dt / setting("real_lever_s", 0.12f);
			if (G.lever_phase == 1 && (G.lever += sp) >= 1) G.lever = 1, G.lever_phase = 2, lever_opened(k, "polegar");
			else if (G.lever_phase == 2 && (G.lever -= sp) <= 0) G.lever = 0, G.lever_phase = 0;
			else if (G.lever_phase == 0) wrist_lever(k);
		}
	}
}

// No weapon_pad (thread do input), depois do mapeamento dos gatilhos: o gatilho do jogo da mão lógica k (cur[6] para a
// 0, cur[5] para a 1) vira um pulso enquanto houver tiro pedido. O jogo atira na borda do gatilho e ignora a borda
// durante o tiro anterior, então o pulso se repete até o Shoot chegar (real_shot).
void real_pad(char* p, LONG* cur) {
	double t = now_ms();
	for (int k = 0; k < 2; k++) {
		Gun& G = g[k];
		if (!active(p, k)) {
			G.fire = G.pulse = false;
			continue;
		}
		float tr = weapon_trigger(k);
		G.trig = G.trig ? tr > 0.35f : tr > 0.6f;
		bool ready = G.kind == K_LEVER ? G.cocked[0] && G.lever < 0.05f : G.cocked[0] || G.cocked[1];
		if (!G.trig) G.fire = false;
		else if (ready && !G.fire) G.fire = true, logr("mão lógica %d: tiro pedido", k);
		if (G.fire && !G.pulse && t - G.t_edge >= 30) G.pulse = true, G.t_edge = t;
		else if (G.pulse && (!G.fire || t - G.t_edge >= 45)) G.pulse = false, G.t_edge = t;
		cur[k ? 5 : 6] = G.pulse ? 255 : 0;
		if (G.reload_btn) cur[0] |= XINPUT_GAMEPAD_X;  // polegar segurado: recarga do jogo (até haver a manual)
	}
}

// Shoot do jogador (hk_shoot): o cão cai; no rifle a câmara fica com o estojo até a alavanca.
void real_shot(int k, bool fired) {
	Gun& G = g[k & 1];
	char* p = player_local();
	if (!active(p, k)) return;
	G.fire = G.pulse = false, G.t_edge = now_ms();
	int i = G.kind == K_SHOTGUN && !G.cocked[0] ? 1 : 0;
	G.cocked[i] = false;
	if (G.kind == K_LEVER) G.need_cycle = true;
	logr("mão lógica %d: %s, cão %d abaixado", k, fired ? "tiro" : "tiro em seco", i);
}

// Botões crus do VR (antes do mapeamento): o polegar da mão de cada arma realista (A na direita, X na esquerda) é do
// mod. Apertar = armar o cão / ciclar a alavanca; segurar por real_reload_ms = recarga do jogo (X), enquanto a recarga
// manual não existe.
WORD real_buttons(WORD vr) {
	char* p = player_local();
	WORD eat = 0;
	double t = now_ms();
	for (int k = 0; k < 2; k++) {
		Gun& G = g[k];
		if (!active(p, k)) {
			G.thumb = G.reload_btn = false;
			continue;
		}
		WORD bit = weapon_phys(k) == 0 ? XINPUT_GAMEPAD_A : XINPUT_GAMEPAD_X;
		bool down = vr & bit;
		eat |= bit;
		if (down && !G.thumb) G.t_thumb = t, InterlockedExchange(&G.thumb_req, 1);
		G.reload_btn = down && t - G.t_thumb >= setting("real_reload_ms", 600);
		G.thumb = down;
	}
	return eat;
}

// Depois de a arma da mão lógica k estar posta na mão (weapon_camera): as peças no estado do mod.
void real_camera(char* p, int k, char* wv) {
	Gun& G = g[k & 1];
	if (wv && G.wv == wv && active(p, k)) {
		for (int i = 0; i < 2; i++)
			if (G.b_ham[i] >= 0) set_rot(wv, G.b_ham[i], 0, G.cock_deg * G.ham[i]);
		if ((G.kind == K_GATE || G.kind == K_BREAK) && G.b_cyl >= 0) set_rot(wv, G.b_cyl, 2, -60 * setting("real_cyl_dir", 1) * G.cyl);
		if (G.kind == K_LEVER && G.b_lever >= 0) set_rot(wv, G.b_lever, 0, -setting("real_lever_deg", 50) * G.lever);
		static double next;
		if (setting("real_debug", 0) && now_ms() >= next)
			next = now_ms() + 500, logf("real: desenho mão %d: cão %.2f (armado %d) -> %.1f°, tambor %.2f, alavanca %.2f", k, G.ham[0], G.cocked[0],
			                            rot_x(wv, G.b_ham[0]), G.cyl, G.lever);
	}
	// Diagnóstico: SET real_test_bone <índice> + real_test_deg + real_test_axis gira um osso da arma da mão 0.
	int tb = (int)setting("real_test_bone", -1);
	if (k == 0 && wv && tb >= 0) set_rot(wv, tb, (int)setting("real_test_axis", 0), setting("real_test_deg", 0));
	if (wv && setting("real_dump", 0)) {  // SET real_dump 1: ossos da arma com o pai e o local
		static const char* const names[] = {"holder", "barrel", "bone_root", "bone_hammer", "bone_hammer_l", "bone_hammer_r",
		                                    "bone_trigger", "bone_trigger_reload", "bone_magazine", "bone_magazine_element",
		                                    "bone_reload_hammer", "bone_reload_schofield", "bone_reload_shotgun", "bone_chamber_up",
		                                    "bone_reload", "bone_side_ellement", "holder_bullet", "holder_bullet1", "holder_bullet_b",
		                                    "full_bullet", "bullet_l", "bullet_r"};
		for (const char* nm : names) {
			int i = bone(wv, nm);
			if (i < 0) continue;
			float L[12];
			local_of(wv, i, L);
			logf("real: mão %d osso %-22s %3d pai %3d | local %6.2f %6.2f %6.2f | x %5.2f %5.2f %5.2f y %5.2f %5.2f %5.2f", k, nm, i,
			     parent_of(wv, i), L[3], L[7], L[11], L[0], L[4], L[8], L[1], L[5], L[9]);
		}
		if (k == 1 || !weapon_held(p, 1)) setting_set("real_dump", 0);
	}
}
