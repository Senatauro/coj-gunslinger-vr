// Gatling em VR (StationaryCannon; modos 14 e 15 do jogador, controller em PlayerDI+0x518). O jogo "monta" a gatling só
// por dentro: o jogador continua onde estava, com as próprias mãos. Mira manual: a mão de apoio agarra a gatling com o
// grip e ela gira em volta do eixo, para o lado oposto ao da mão, como uma gatling de verdade. Tiro: a mão dominante
// agarra a manivela e gira; o osso da manivela acompanha a mão e, quanto mais voltas por segundo, mais tempo o gatilho
// do jogo fica apertado. O tiro sai do cano do modelo. Sair: soltar as duas mãos por gat_exit_ms.
// Ossos do weapon_gatling.msh usados: barrel (boca), drotbarrel (eixo dos canos, x = frente), drothoriz (eixo de giro),
// bone_handgrip (manivela, gira em torno do x dele) e handgrip (o cabo da manivela, filho dela).
#include "mod.h"
#include <math.h>
#include <string.h>

extern "C" int raycast(char* p, const float* from, float* to, void* out);  // weapon.cpp
extern "C" float* bone_world(char* obj, int idx);                          // weapon.cpp
extern "C" char* bone_node(char* obj, int idx);
extern "C" float* bone_slot(char* node);

namespace {
bool g_have_dir, g_held, g_gripped;  // mira inicial lida; mão de apoio segurando; aperto novo de grip desde a montagem
float g_dir[3];                      // mira pedida ao jogo (mundo do jogo, unitária)
float g_base[2], g_v0[2];            // na pega: yaw/pitch da mira e da linha mão → eixo
float g_pivot[3];                    // eixo virtual relativo à âncora (sem os ossos): mão + mira × gat_lever_cm
double g_free;                       // desde quando as duas mãos estão soltas (0 = segurando)
float g_rps, g_w;                    // voltas por segundo da manivela (filtrado) e com sinal
float g_budget;                      // tiros liberados pela manivela e ainda não disparados (o gatilho fica apertado ≥ 1)
float g_cu, g_cv, g_ang;             // sem os ossos: centro estimado da manivela; último ângulo da mão em volta do centro
bool g_cinit, g_hasang, g_cranking;  // g_cranking: a mão da manivela está nela (o osso segue a mão)
bool g_shot_log;                     // 1º tiro da montagem já registrado
int g_frames;                        // frames montado (o 1º guarda os grips já apertados)
char* g_obj;                         // StationaryCannon montado
enum { B_BARREL, B_ROT, B_YAW, B_CRANK, B_HANDLE, B_N };
int g_b[B_N];

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
// Classe de um objeto pelo RTTI do MSVC (vtable[-1] = COL, +0xC = TypeDescriptor, nome em +8); "?" se não der para ler.
const char* rtti(char* o) {
	if (!o || IsBadReadPtr(o, 4) || IsBadReadPtr(*(char**)o - 4, 4)) return "?";
	char* col = (*(char***)o)[-1];
	return IsBadReadPtr(col + 0xC, 4) || IsBadReadPtr(*(char**)(col + 0xC) + 8, 4) ? "?" : *(char**)(col + 0xC) + 8;
}
int bone_of(char* g, const char* nm) {
	char* m = *(char**)(g + 4);
	return ((int(__thiscall*)(char*, const char*))(*(void***)m)[0x3EC / 4])(m, nm);
}
bool has(int b) { return g_obj && g_b[b] >= 0; }
const float* W(int b) { return bone_world(g_obj, g_b[b]); }
void yawpitch(const float* d, float yp[2]) { yp[0] = atan2f(d[2], d[0]), yp[1] = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2])); }
float wrap(float a) { return a > 3.14159265f ? a - 6.2831853f : a < -3.14159265f ? a + 6.2831853f : a; }
void reset() {
	g_have_dir = g_held = g_gripped = g_cinit = g_hasang = g_cranking = false, g_free = 0, g_rps = g_w = g_budget = 0, g_frames = 0, g_shot_log = false;
	g_obj = NULL;
}

// Ângulo de um ponto do mundo em volta do eixo da manivela (x do osso bone_handgrip), medido nos eixos y e z dele a
// partir do vertical do mundo: o y e o z do osso giram com a manivela (o próprio mod a gira para a mão), o x não. d =
// distância do ponto ao centro dela.
float crank_angle(const float* M, const float* P, float* d) {
	float r[3] = {P[0] - M[3], P[1] - M[7], P[2] - M[11]};
	if (d) *d = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	return wrap(atan2f(r[0] * M[2] + r[1] * M[6] + r[2] * M[10], r[0] * M[1] + r[1] * M[5] + r[2] * M[9]) - atan2f(M[6], M[5]));
}

// Voltas por segundo da manivela, pela variação do ângulo da mão com sinal e filtrada: sacudir a mão não soma, só girar.
// Com os ossos o centro é o da manivela do modelo e a mão precisa estar a gat_crank_reach_cm dele; sem eles, o centro é
// a média móvel da mão no plano vertical da mira (gat_crank_center_s: numa volta inteira a média é o centro).
void crank(const float* G, bool grip, const float* A, float dt) {
	float w = 0, ang = 0;
	bool ok = false;
	g_cranking = false;
	if (grip && G && dt > 0 && has(B_CRANK)) {
		float gp[3] = {G[3], G[7], G[11]}, d;
		ang = crank_angle(W(B_CRANK), gp, &d);
		ok = g_cranking = d <= setting("gat_crank_reach_cm", 45);
	} else if (grip && G && dt > 0) {
		float f[2] = {g_dir[0], g_dir[2]}, n = sqrtf(f[0] * f[0] + f[1] * f[1]);
		float r[3] = {G[3] - A[0], G[7] - A[1], G[11] - A[2]}, u = (r[0] * f[0] + r[2] * f[1]) / (n + 1e-6f), v = r[1];
		if (!g_cinit) g_cu = u, g_cv = v, g_cinit = true;
		float k = fminf(1, dt / setting("gat_crank_center_s", 1)), rmin = setting("gat_crank_min_cm", 3);
		g_cu += (u - g_cu) * k, g_cv += (v - g_cv) * k;
		ok = (u - g_cu) * (u - g_cu) + (v - g_cv) * (v - g_cv) >= rmin * rmin, ang = atan2f(v - g_cv, u - g_cu);
	} else {
		g_cinit = false;
	}
	if (ok) {
		if (g_hasang) w = wrap(ang - g_ang) / 6.2831853f / dt;
		g_ang = ang;
	}
	g_hasang = ok;
	if (dt > 0) g_w += (w - g_w) * fminf(1, dt / setting("gat_crank_tau", 0.25f));
	g_rps = fabsf(g_w);
}

// Ossos da gatling montada, achados uma vez por montagem.
void find_bones(char* p) {
	char *c = *(char**)(p + 0x518), *o = c ? *(char**)(c + 0x3C) : NULL, *g = o ? o - 0x2C : NULL;
	static const char* const names[B_N] = {"barrel", "drotbarrel", "drothoriz", "bone_handgrip", "handgrip"};
	g_obj = g && !strcmp(rtti(*(char**)(g + 4)), ".?AVCModelObject@@") ? g : NULL;
	for (int i = 0; i < B_N; i++) g_b[i] = g_obj ? bone_of(g_obj, names[i]) : -1;
	logf("gatling: objeto %p %s; ossos boca %d canos %d eixo %d manivela %d cabo %d", (void*)g, rtti(g), g_b[0], g_b[1], g_b[2], g_b[3],
	     g_b[4]);
}
}  // namespace

bool gatling_on(char* p) { return setting("gat_vr", 1) && mounted(p); }

// Âncora da cabeça na gatling (modos 13 a 15, montando, montado e desmontando). Padrão: a âncora de antes de montar,
// parada; o jogador continua onde estava, com o próprio corpo, e o jogo "monta" só por dentro (o corpo dele vai para
// o assento e oscila alguns cm por frame). gat_seat 1: a câmera do jogo no assento, com a altura sobre o corpo fixada
// no 1º frame. Chamado logo depois do CameraFPPDI::vf8 original (cam = mundo da câmera do jogo, 3x4).
static float g_pre[3];  // âncora do último frame fora da gatling
void gatling_anchor(char* p, const float* cam, float a[3]) {
	static bool was;
	static float dy;
	int m = p ? *(int*)(p + 0x5A8) : -1;
	bool on = setting("gat_vr", 1) && (m == 13 || m == 14 || m == 15) && player_head_frame();
	if (!on) memcpy(g_pre, a, sizeof g_pre);
	else if (!setting("gat_seat", 0)) memcpy(a, g_pre, sizeof g_pre);
	else {
		if (!was) dy = cam[7] - a[1];
		a[0] = cam[3], a[1] = cam[7] - dy, a[2] = cam[11];
	}
	was = on;
}

// Direção de mira lida pelo controller da gatling (PlayerDI::vf120 chamado de FUN_00BDDA80). A primeira leitura depois
// de montar fica como a mira inicial; depois a mira é a da mão.
void gatling_lookdir(char* p, float out[3]) {
	if (!setting("gat_vr", 1) || !mounted(p)) return;
	if (!g_have_dir) {
		memcpy(g_dir, out, sizeof g_dir), g_have_dir = true, find_bones(p);
		return logf("gatling: montado (modo %d), mira inicial %.2f %.2f %.2f", *(int*)(p + 0x5A8), out[0], out[1], out[2]);
	}
	memcpy(out, g_dir, sizeof g_dir);
}

// Boca e direção da gatling para o Shoot (o tiro dela passa pelo mesmo WeaponFireController das armas): o osso barrel na
// direção dos canos (x do drotbarrel), como a gatling está de fato (com os limites de giro dela). Sem os ossos, do eixo
// virtual gat_barrel_cm na direção da mira.
bool gatling_muzzle(char* p, float o[3], float d[3]) {
	if (!setting("gat_vr", 1) || !mounted(p) || !g_have_dir) return false;
	if (has(B_BARREL) && has(B_ROT)) {
		const float *B = W(B_BARREL), *R = W(B_ROT);
		float f[3] = {R[0], R[4], R[8]}, n = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]) + 1e-6f;
		for (int i = 0; i < 3; i++) d[i] = f[i] / n, o[i] = B[i * 4 + 3] + d[i] * 5;
	} else {
		const float *A = player_anchor(), *C = player_head_cam();
		float L = setting("gat_barrel_cm", 80);
		for (int i = 0; i < 3; i++) o[i] = (g_gripped ? A[i] + g_pivot[i] : C[i * 4 + 3]) + g_dir[i] * L, d[i] = g_dir[i];
	}
	g_budget = fmaxf(0, g_budget - 1);  // um tiro da manivela gasto
	if (!g_shot_log) g_shot_log = true, logf("gatling: 1º tiro da boca %.0f %.0f %.0f na direção %.2f %.2f %.2f", o[0], o[1], o[2], d[0], d[1], d[2]);
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
		int m = p ? *(int*)(p + 0x5A8) : -1;
		if (g_have_dir) {
			logf("gatling: fora (modo %d)", m), vr_reticle(3, NULL, 0);
			// Desmontou: o corpo, que o jogo deixou no assento, volta para onde o jogador estava antes de montar.
			if (p && m != 8 && !setting("gat_seat", 0))
				((void(__thiscall*)(char*, float*))(*(void***)p)[0xB8 / 4])(p, g_pre), logf("gatling: de volta a %.0f %.0f %.0f", g_pre[0], g_pre[1], g_pre[2]);
		}
		return reset();
	}
	if (!g_have_dir) return;
	const float* A = player_anchor();
	int hd = dom(), ha = 1 - hd;
	const float *Ga = weapon_hand_game(ha), *Gd = weapon_hand_game(hd);
	bool ga = vr_grip(ha) && Ga, gd = vr_grip(hd) && Gd;
	// Mira: a linha da mão de apoio até o eixo de giro da gatling (drothoriz) gira a mira a partir da pega: yaw e pitch
	// da linha somados à mira que havia na pega (a mão vai para a direita, a boca para a esquerda). Sem os ossos, o eixo
	// fica gat_lever_cm à frente da mão na pega.
	float piv[3];
	if (ga) {
		if (has(B_YAW)) {
			const float* Y = W(B_YAW);
			piv[0] = Y[3], piv[1] = Y[7], piv[2] = Y[11];
		} else {
			if (!g_held)
				for (int i = 0; i < 3; i++) g_pivot[i] = Ga[i * 4 + 3] + g_dir[i] * setting("gat_lever_cm", 60) - A[i];
			for (int i = 0; i < 3; i++) piv[i] = A[i] + g_pivot[i];
		}
		float v[3] = {piv[0] - Ga[3], piv[1] - Ga[7], piv[2] - Ga[11]}, vyp[2];
		yawpitch(v, vyp);
		if (!g_held) {
			yawpitch(g_dir, g_base), memcpy(g_v0, vyp, sizeof g_v0);
			logf("gatling: mão %s segurando (eixo a %.0f cm da mão)", ha ? "esquerda" : "direita", sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
		}
		float y = g_base[0] + wrap(vyp[0] - g_v0[0]), pt = fmaxf(-1.4f, fminf(1.4f, g_base[1] + vyp[1] - g_v0[1]));
		g_dir[0] = cosf(pt) * cosf(y), g_dir[1] = sinf(pt), g_dir[2] = cosf(pt) * sinf(y);
	}
	if (!ga && g_held) logf("gatling: mão de apoio soltou");
	g_held = ga;
	static double next_log;  // gat_log 1: estado a cada 0,5 s
	if (setting("gat_log", 0) && t >= next_log) {
		char* c = *(char**)(p + 0x518);
		next_log = t + 0.5;
		logf("gatling: mira %.2f %.2f %.2f | apoio %d manivela %.1f voltas/s%s | controller estado %d | mãos %d %d", g_dir[0], g_dir[1],
		     g_dir[2], g_held, g_rps, g_cranking ? " (na manivela)" : "", c ? *(int*)(c + 0xD0) : -1, ga, gd);
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
	if (g_free && t - g_free >= setting("gat_exit_ms", 200) / 1000) {
		char* c = *(char**)(p + 0x518);
		logf("gatling: mãos soltas por %.0f ms, saindo", setting("gat_exit_ms", 200));
		if (c) ((void(__thiscall*)(char*, int, float, char))(*(void***)c)[207])(c, 20, 1.f, 0);
		g_free = 0, g_gripped = false;
	}
	// Ponto de mira: do cano, na direção dos canos, até onde bate (sem os ossos, do eixo virtual na direção da mira).
	if (!setting("crosshair_vr", 1)) return vr_reticle(3, NULL, 0);
	const float* C = player_head_cam();
	float o[3], d[3], to[3], out[0x30];
	if (has(B_BARREL) && has(B_ROT)) {
		const float *B = W(B_BARREL), *R = W(B_ROT);
		float n = sqrtf(R[0] * R[0] + R[4] * R[4] + R[8] * R[8]) + 1e-6f;
		for (int i = 0; i < 3; i++) d[i] = R[i * 4] / n, o[i] = B[i * 4 + 3];
	} else {
		for (int i = 0; i < 3; i++) o[i] = g_gripped ? A[i] + g_pivot[i] : C[i * 4 + 3], d[i] = g_dir[i];
	}
	for (int i = 0; i < 3; i++) to[i] = o[i] + d[i] * 10000;
	if (!raycast(p, o, to, out)) return vr_reticle(3, NULL, 0);
	float v[3] = {to[0] - C[3], to[1] - C[7], to[2] - C[11]}, len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), k[3];
	game_to_trk(to, k);
	vr_reticle(3, k, len / (100 * setting("world_scale", 1)) * tanf(setting("crosshair_deg", 1.5f) * 0.01745329f));
}

// Holder da mão que gira a manivela (T = o do controle): no cabo, no ângulo da mão, com o cabo atravessando o punho (y
// do Holder no eixo da manivela) e o resto da orientação da mão real. false = essa mão não está na manivela.
bool gatling_grip(int side, const float* T, float out[12]) {
	const float* G = weapon_hand_game(side);
	if (!g_cranking || side != dom() || !G || !has(B_CRANK) || !has(B_HANDLE)) return false;
	const float *M = W(B_CRANK), *H = W(B_HANDLE);
	float a[3] = {M[0], M[4], M[8]}, n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) + 1e-6f;
	for (int i = 0; i < 3; i++) a[i] /= n;
	float h[3] = {H[3] - M[3], H[7] - M[7], H[11] - M[11]}, g[3] = {G[3] - M[3], G[7] - M[7], G[11] - M[11]};
	float ha = h[0] * a[0] + h[1] * a[1] + h[2] * a[2], ga = g[0] * a[0] + g[1] * a[1] + g[2] * a[2], r = 0, gl = 0;
	for (int i = 0; i < 3; i++) h[i] -= a[i] * ha, g[i] -= a[i] * ga, r += h[i] * h[i], gl += g[i] * g[i];
	if (gl < 1) return false;  // mão em cima do eixo: sem ângulo
	r = sqrtf(r), gl = sqrtf(gl);
	float y[3], z[3] = {T[2], T[6], T[10]}, x[3], s = a[0] * T[1] + a[1] * T[5] + a[2] * T[9] < 0 ? -1.f : 1.f;
	for (int i = 0; i < 3; i++) y[i] = a[i] * s;
	float zy = z[0] * y[0] + z[1] * y[1] + z[2] * y[2], zl = 0;
	for (int i = 0; i < 3; i++) z[i] -= y[i] * zy, zl += z[i] * z[i];
	if (zl < 1e-4f) return false;
	zl = sqrtf(zl);
	for (int i = 0; i < 3; i++) z[i] /= zl;
	// a mão do jogo pode ser espelhada: x segue a orientação do frame do controle
	float det = (T[4] * T[9] - T[8] * T[5]) * T[2] + (T[8] * T[1] - T[0] * T[9]) * T[6] + (T[0] * T[5] - T[4] * T[1]) * T[10];
	x[0] = y[1] * z[2] - y[2] * z[1], x[1] = y[2] * z[0] - y[0] * z[2], x[2] = y[0] * z[1] - y[1] * z[0];
	for (int i = 0; i < 3; i++) {
		if (det < 0) x[i] = -x[i];
		out[i * 4] = x[i], out[i * 4 + 1] = y[i], out[i * 4 + 2] = z[i];
		out[i * 4 + 3] = M[i * 4 + 3] + a[i] * ha + g[i] / gl * r;
	}
	return true;
}

// Depois da câmera FPP (com a âncora e as mãos finais do frame). Ao montar, o jogo esconde o corpo de 1ª pessoa
// (PlayerDI+0x1C8 vf6(0), que chama o SetVisible do modelo): com as mãos do VR, ele volta a aparecer (o braço já fica
// encolhido pelo mod, só as mãos aparecem). E a manivela do modelo vira para a mão que a segura: o osso bone_handgrip
// gira em torno do próprio x até o cabo (handgrip) ficar no ângulo da mão; bone_slot marca os filhos para recálculo.
void gatling_camera(char* p) {
	int m = p ? *(int*)(p + 0x5A8) : -1;
	if (char* model = p ? *(char**)(p + 4) : NULL; model && setting("gat_vr", 1) && setting("hands", 1) && m >= 13 && m <= 15)
		((void(__thiscall*)(char*, char))(*(void***)model)[0x254 / 4])(model, 1);
	if (!setting("gat_vr", 1) || !mounted(p) || !g_cranking || !has(B_CRANK) || !has(B_HANDLE)) return;
	const float* G = weapon_hand_game(dom());
	if (!G) return;
	const float* H = W(B_HANDLE);
	float hp[3] = {H[3], H[7], H[11]}, gp[3] = {G[3], G[7], G[11]};
	float* M = bone_slot(bone_node(g_obj, g_b[B_CRANK]));
	float a = wrap(crank_angle(M, gp, NULL) - crank_angle(M, hp, NULL)), c = cosf(a), s = sinf(a);
	for (int r = 0; r < 3; r++) {  // y' = y·cos + z·sin, z' = −y·sin + z·cos (giro em torno do x local)
		float y = M[r * 4 + 1], z = M[r * 4 + 2];
		M[r * 4 + 1] = y * c + z * s, M[r * 4 + 2] = -y * s + z * c;
	}
}
