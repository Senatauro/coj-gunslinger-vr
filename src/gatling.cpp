// Gatling em VR (StationaryCannon; modos 14 e 15 do jogador, controller em PlayerDI+0x518). Mira manual: a mão de apoio
// agarra a gatling com o grip e ela gira em volta do suporte, para o lado oposto ao da mão, como uma gatling de verdade.
// Tiro: a mão dominante gira a manivela com o grip apertado; quanto mais voltas por segundo, mais tempo o gatilho do jogo
// fica apertado. Sair: soltar as duas mãos por gat_exit_ms. A âncora da cabeça vem da câmera do jogo na gatling, porque
// o corpo preso a ela oscila alguns centímetros por frame.
#include "mod.h"
#include <math.h>
#include <string.h>

extern "C" int raycast(char* p, const float* from, float* to, void* out);  // weapon.cpp

namespace {
bool g_have_dir, g_held, g_gripped;  // mira inicial lida; mão de apoio segurando; alguma mão já agarrou desde a montagem
float g_dir[3];                      // mira da gatling (mundo do jogo, unitária)
float g_pivot[3];                    // suporte, relativo à âncora (fixado na pega: mão + mira × gat_lever_cm)
double g_free;                       // desde quando as duas mãos estão soltas (0 = segurando)
float g_rps;                         // voltas por segundo da manivela, filtrado
float g_budget;                      // tiros liberados pela manivela e ainda não disparados (o gatilho fica apertado ≥ 1)
float g_cu, g_cv, g_ang, g_w;        // centro estimado da manivela, último ângulo da mão nele, voltas/s com sinal
bool g_cinit, g_hasang;
int g_frames;                        // frames montado (o 1º guarda os grips já apertados)

double now_s() {
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	return (double)t.QuadPart / f.QuadPart;
}
int dom() { return setting("left_handed", 0) ? 1 : 0; }
bool mounted(char* p) {
	int m = p ? *(int*)(p + 0x5A8) : -1;
	return (m == 14 || m == 15) && player_head_frame();
}
void reset() { g_have_dir = g_held = g_gripped = g_cinit = g_hasang = false, g_free = 0, g_rps = g_w = g_budget = 0, g_frames = 0; }

// Voltas por segundo da manivela. A mão da manivela vai para o plano vertical que contém a mira (o plano dela), relativa
// à âncora; o centro da manivela é a média móvel da mão (gat_crank_center_s: numa volta inteira a média é o centro) e a
// velocidade é a variação do ângulo da mão em volta dele, com sinal e filtrada: sacudir a mão não soma, só girar.
void crank(const float* G, bool grip, const float* A, float dt) {
	float f[2] = {g_dir[0], g_dir[2]}, n = sqrtf(f[0] * f[0] + f[1] * f[1]), w = 0;
	if (grip && G && n > 1e-3f && dt > 0) {
		float r[3] = {G[3] - A[0], G[7] - A[1], G[11] - A[2]}, u = (r[0] * f[0] + r[2] * f[1]) / n, v = r[1];
		if (!g_cinit) g_cu = u, g_cv = v, g_cinit = true;
		float k = fminf(1, dt / setting("gat_crank_center_s", 1)), rmin = setting("gat_crank_min_cm", 3);
		g_cu += (u - g_cu) * k, g_cv += (v - g_cv) * k;
		float du = u - g_cu, dv = v - g_cv;
		if (du * du + dv * dv >= rmin * rmin) {
			float ang = atan2f(dv, du), d = ang - g_ang;
			d = d > 3.14159f ? d - 6.28318f : d < -3.14159f ? d + 6.28318f : d;
			if (g_hasang) w = d / 6.28318f / dt;
			g_ang = ang, g_hasang = true;
		} else {
			g_hasang = false;
		}
	} else {
		g_cinit = g_hasang = false;
	}
	if (dt > 0) g_w += (w - g_w) * fminf(1, dt / setting("gat_crank_tau", 0.25f));
	g_rps = fabsf(g_w);
}
}  // namespace

bool gatling_on(char* p) { return setting("gat_vr", 1) && mounted(p); }

// Âncora da cabeça montado na gatling: a câmera do jogo (assento da gatling), com a altura dela sobre o corpo fixada no
// 1º frame montado. Chamado logo depois do CameraFPPDI::vf8 original (cam = mundo da câmera do jogo, 3x4).
void gatling_anchor(char* p, const float* cam, float a[3]) {
	static bool was;
	static float dy;
	bool on = setting("gat_vr", 1) && mounted(p);
	if (on && !was) dy = cam[7] - a[1];
	was = on;
	if (on) a[0] = cam[3], a[1] = cam[7] - dy, a[2] = cam[11];
}

// Direção de mira lida pelo controller da gatling (PlayerDI::vf120 chamado de FUN_00BDDA80). A primeira leitura depois
// de montar fica como a mira inicial; depois a mira é a da mão.
void gatling_lookdir(char* p, float out[3]) {
	if (!setting("gat_vr", 1) || !mounted(p)) return;
	if (!g_have_dir) {
		memcpy(g_dir, out, sizeof g_dir), g_have_dir = true;
		return logf("gatling: montado (modo %d), mira inicial %.2f %.2f %.2f", *(int*)(p + 0x5A8), out[0], out[1], out[2]);
	}
	memcpy(out, g_dir, sizeof g_dir);
}

// Boca e direção da gatling para o Shoot (o tiro dela passa pelo mesmo WeaponFireController das armas): do suporte
// (ou da cabeça antes da 1ª pega) gat_barrel_cm na direção da mira.
bool gatling_muzzle(char* p, float o[3], float d[3]) {
	if (!setting("gat_vr", 1) || !mounted(p) || !g_have_dir) return false;
	const float *A = player_anchor(), *C = player_head_cam();
	float L = setting("gat_barrel_cm", 80);
	for (int i = 0; i < 3; i++) o[i] = (g_gripped ? A[i] + g_pivot[i] : C[i * 4 + 3]) + g_dir[i] * L, d[i] = g_dir[i];
	g_budget = fmaxf(0, g_budget - 1);  // um tiro da manivela gasto
	return true;
}

// Gatilho do jogo (ação 161) montado na gatling: apertado enquanto a manivela tiver tiros liberados (gatling_frame soma
// voltas/s × gat_shots_per_rev, cada Shoot da gatling gasta um). O teto é a cadência da própria arma. −1 = fora dela.
float gatling_fire(char* p) {
	if (!setting("gat_vr", 1) || !mounted(p)) return -1;
	return g_budget >= 1 ? 1.f : 0.f;
}

// No fim do update do jogador: mira pela mão de apoio, manivela, saída e ponto de mira.
void gatling_frame(char* p) {
	static double last;
	double t = now_s();
	float dt = last ? fminf(0.1f, (float)(t - last)) : 0;
	last = t;
	if (!setting("gat_vr", 1) || !mounted(p)) {
		if (g_have_dir) logf("gatling: fora (modo %d)", p ? *(int*)(p + 0x5A8) : -1), vr_reticle(3, NULL, 0);
		return reset();
	}
	if (!g_have_dir) return;
	const float* A = player_anchor();
	int hd = dom(), ha = 1 - hd;
	const float *Ga = weapon_hand_game(ha), *Gd = weapon_hand_game(hd);
	bool ga = vr_grip(ha) && Ga, gd = vr_grip(hd) && Gd;
	// Mira: na pega o suporte fica a gat_lever_cm à frente da mão, na mira atual; depois a mira é do suporte para longe
	// da mão (a mão na traseira, a boca do outro lado).
	if (ga && !g_held) {
		float L = setting("gat_lever_cm", 60);
		for (int i = 0; i < 3; i++) g_pivot[i] = Ga[i * 4 + 3] + g_dir[i] * L - A[i];
		logf("gatling: mão %s segurando (suporte a %.0f cm da mão)", ha ? "esquerda" : "direita", L);
	}
	if (!ga && g_held) logf("gatling: mão de apoio soltou");
	g_held = ga;
	if (g_held) {
		float d[3] = {A[0] + g_pivot[0] - Ga[3], A[1] + g_pivot[1] - Ga[7], A[2] + g_pivot[2] - Ga[11]};
		float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (n > 5)
			for (int i = 0; i < 3; i++) g_dir[i] = d[i] / n;
	}
	static double next_log;  // gat_log 1: estado a cada 0,5 s
	if (setting("gat_log", 0) && t >= next_log) {
		char* c = *(char**)(p + 0x518);
		next_log = t + 0.5;
		logf("gatling: mira %.2f %.2f %.2f | apoio %d manivela %.1f voltas/s | controller estado %d | mãos %d %d", g_dir[0], g_dir[1],
		     g_dir[2], g_held, g_rps, c ? *(int*)(c + 0xD0) : -1, ga, gd);
	}
	float before = g_rps;
	crank(Gd, gd, A, dt);
	// ponytail: acima da cadência da arma (~5 tiros/s) a manivela não acelera mais; mexer na cadência da arma se precisar.
	g_budget = fminf(2, g_budget + g_rps * setting("gat_shots_per_rev", 3) * dt);
	if ((g_rps > 0.2f) != (before > 0.2f)) logf("gatling: manivela %s (%.1f voltas/s)", g_rps > 0.2f ? "girando" : "parada", g_rps);
	// Sair: as duas mãos soltas por gat_exit_ms, depois de um aperto novo de grip desde a montagem (o grip que montou
	// não conta: soltá-lo logo depois não pode tirar o jogador da gatling). O controller só aceita a ação 20 com valor
	// 1,0, como o "segurar X" do jogo.
	static bool prev[2];
	if (g_frames++ == 0) prev[0] = ga, prev[1] = gd;
	g_gripped |= (ga && !prev[0]) || (gd && !prev[1]);
	prev[0] = ga, prev[1] = gd;
	g_free = ga || gd || !g_gripped ? 0 : g_free ? g_free : t;
	if (g_free && t - g_free >= setting("gat_exit_ms", 1000) / 1000) {
		char* c = *(char**)(p + 0x518);
		logf("gatling: mãos soltas por %.0f ms, saindo", setting("gat_exit_ms", 1000));
		if (c) ((void(__thiscall*)(char*, int, float, char))(*(void***)c)[207])(c, 20, 1.f, 0);
		g_free = 0, g_gripped = false;
	}
	// Ponto de mira: do suporte (ou da cabeça antes da 1ª pega) na direção da mira até onde bate.
	// ponytail: a mira da mão, não a da gatling; no limite de giro da gatling o ponto passa do cano.
	if (!setting("crosshair_vr", 1)) return vr_reticle(3, NULL, 0);
	const float* C = player_head_cam();
	float o[3], to[3], out[0x30];
	for (int i = 0; i < 3; i++) o[i] = g_gripped ? A[i] + g_pivot[i] : C[i * 4 + 3], to[i] = o[i] + g_dir[i] * 10000;
	if (!raycast(p, o, to, out)) return vr_reticle(3, NULL, 0);
	float v[3] = {to[0] - C[3], to[1] - C[7], to[2] - C[11]}, len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), k[3];
	game_to_trk(to, k);
	vr_reticle(3, k, len / (100 * setting("world_scale", 1)) * tanf(setting("crosshair_deg", 1.5f) * 0.01745329f));
}
