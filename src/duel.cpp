// Duelo em VR. O duelo fica em CENA (`duel_vr`, ver player.cpp), com a cabeça virada para o inimigo no começo. O saque
// é o gesto de tirar a arma do coldre, a esquiva é a inclinação da cabeça e o foco vem de sinais no corpo do inimigo
// que o jogador precisa olhar. A mira, o tiro do inimigo e o Speed % do placar são ajustados para valer em VR.
// O log anota cada troca de estado do DuelManager, de fase e de saque do DuelController e de classe do vis do jogador,
// com os valores do momento; duel_log 1 soma uma amostra a cada 0,5 s durante o duelo.
#include "mod.h"
#include <math.h>
#include <stdio.h>

namespace {
// DuelManager da sessão: *(*(*(*(PlayerDI+0x20)+0x24)+0x74)+0x614) (cadeia do asm em 0x00AC1430), nulo a cada passo.
char* duel_manager(char* p) {
	static const int chain[] = {0x20, 0x24, 0x74, 0x614};
	for (int off : chain) {
		if (!p) return NULL;
		p = *(char**)(p + off);
	}
	return p;
}

const char* name_of(const char* const* names, int n, int v) { return v >= 0 && v < n ? names[v] : "?"; }
const char* const k_state[] = {"NotStarted", "PreparingCharacters", "Dialog", "Warmup", "PreparingToSilence", "Silence",
                               "DishonorableStart", "Gong", "Fight", "PlayerWin", "PlayerDeath", "FinishingRoundMulti",
                               "StartingNewRoundMulti", "FadeoutWaiting", "Finished"};
const char* const k_phase[] = {"None", "Warmup", "Catch", "Draw", "Fight", "Win", "Lose"};
const char* const k_grab[] = {"ReadyToGrab", "WaitingOnReact", "Grabing", "Grabed"};

const char* vis_name(char* vis) {
	uintptr_t vt = vis ? *(uintptr_t*)vis : 0;
	return vt == 0x01342C2C ? "PlayerDuelVis" : vt == 0x013423AC ? "PlayerFppVis" : "outro";
}

void log_values(const char* why, char* p, char* d, char* dm) {
	float* f = (float*)d;  // índices em floats: +0x124 = f[73]
	char* cam = *(char**)(d + 0x17C);
	float* c = (float*)cam;
	logf("duelo: %s | estado %s fase %s saque %s | foco %.2f/%.2f mão %.2f %.2f (alvo %.2f %.2f) saque %.3f s reação %.3f s"
	     " esquiva %.2f | inimigo %d de %d | câmera %.0f %.0f %.0f FOV %.1f | alvo do tiro %.0f %.0f %.0f | jogador modo %d",
	     why, name_of(k_state, 15, dm ? *(int*)(dm + 4) : -1), name_of(k_phase, 7, *(int*)(d + 0x3C)),
	     name_of(k_grab, 4, *(int*)(d + 0x118)), f[0x124 / 4], f[0x128 / 4], f[0xF4 / 4], f[0xF8 / 4], f[0xFC / 4], f[0x100 / 4],
	     f[0x11C / 4], f[0x5C / 4], f[0x13C / 4], *(int*)(d + 0x194), dm && *(char**)(dm + 0xC) ? *(int*)(*(char**)(dm + 0xC) + 0x94) : 0,
	     cam ? c[0] : 0, cam ? c[1] : 0, cam ? c[2] : 0, cam ? c[0x54 / 4] : 0, f[0x88 / 4], f[0x8C / 4], f[0x90 / 4],
	     *(int*)(p + 0x5A8));
}

// Posição do inimigo atual: câmeras do duelo +0x80 (gravada pelo DuelController a cada frame até a fase Fight).
const float* enemy_pos(char* d) {
	char* cam = d ? *(char**)(d + 0x17C) : NULL;
	return cam ? (float*)(cam + 0x80) : NULL;
}

// Yaw (graus, convenção do jogo: atan2(z, x) da frente) da cabeça até o inimigo.
bool yaw_to_enemy(char* d, float& psi) {
	const float *e = enemy_pos(d), *C = player_head_cam();
	if (!e) return false;
	float dx = e[0] - C[3], dz = e[2] - C[11];
	if (dx * dx + dz * dz < 1) return false;
	psi = atan2f(dz, dx) * 57.29578f;
	return true;
}

// Log dos ossos do corpo do duelo (player_tpp) que interessam ao mod: índice pelo nome (0x00E93420, thiscall (corpo,
// nome)).
void dump_bones(char* vis) {
	static const char* names[] = {"R_Holster", "L_Holster", "R_HandHolder", "L_HandHolder", "R_Hand", "L_Hand", "Head",
	                              "Neck", "R_Clavicle", "L_Clavicle", "Pelvis", "Spine", "EyeCamera", "RefCamera"};
	char line[512];
	int n = 0;
	for (const char* x : names) {
		int i = ((int(__thiscall*)(char*, const char*))0x00E93420)(vis + 0x20, x);
		n += snprintf(line + n, sizeof line - n, " %s=%d", x, i);
	}
	logf("duelo: ossos do corpo do duelo:%s", line);
}

// --- DuelMinigameReticule/Hardin::vf20 (0x00ED6550, thiscall (float dt), ret 4), o update do minijogo. Depois
// dele o mod troca o raio do retículo (origem +0x58, direção +0x4C, ponto +0x40 na distância do inimigo) pelo da cabeça;
// a taxa do foco (FUN_00AC21E0, mais adiante no mesmo vf200) já mede esse raio. ---
typedef void(__thiscall* mg20_t)(char*, float);
mg20_t o_mg20;

// Mão dominante (física) e o centro do coldre do quadril dela no frame do corpo (cm), o mesmo da zona do coldre da
// arma (weapon.cpp).
int dom_hand() { return setting("left_handed", 0) ? 1 : 0; }
void holster_center(float c[3]) {
	c[0] = setting("hip_x", 25) * (dom_hand() ? -1 : 1), c[1] = setting("hip_y", -55), c[2] = setting("hip_z", 0);
}
float dist3(const float* a, const float* b) {
	return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void __fastcall hk_mg20(char* mg, void*, float dt) {
	o_mg20(mg, dt);
	char* d = *(char**)(mg + 4);
	const float *e = enemy_pos(d), *C = player_head_cam();
	if (!e || !setting("duel_vr", 1) || !player_head_frame()) return;
	float o[3] = {C[3], C[7], C[11]}, f[3] = {-C[2], -C[6], -C[10]};
	float dist = dist3(e, o);
	float* m = (float*)mg;
	for (int i = 0; i < 3; i++) m[0x58 / 4 + i] = o[i], m[0x4C / 4 + i] = f[i], m[0x40 / 4 + i] = o[i] + f[i] * dist;
	// Speed pelo saque real (ver real_speed). Com o skill DuelHand o jogo lê a mão 2D (+0x98 x, +0x9C y) para calcular
	// o tempo do saque (+0x11C); o mod a deixa no ponto ótimo do jogo (0,025; −0,85), que dá o melhor tempo interno, e
	// quem decide é o saque de verdade. Sem o skill o jogo fixa a mão em (0; −0,4) a cada frame (FUN_00ED7510) e o mod
	// não mexe.
	if (m[0x98 / 4] == 0 && m[0x9C / 4] == -0.4f) return;
	m[0x98 / 4] = 0.025f, m[0x9C / 4] = -0.85f;
}

// --- Foco por sinais. Do Warmup até o sinal do duelo (estados 3 a 5: Warmup, PreparingToSilence e Silence), a cada
// duel_sign_gap_min..max s um ponto do inimigo atual (cabeça, mão da arma, coldre)
// vira o sinal por duel_sign_ms, marcado no próprio ponto pelo anel do "usar" (overlay pequeno no mundo; branco
// esperando, amarelo segurando, verde ao pegar). O olho do HUD do jogo (G_Eye, bit 0x8 do FUN_00B6E9D0, ponto +0x40
// do minijogo do olho) não serve: no HUD preso à cabeça ele sai projetado ~14° fora do lugar, como o retículo e o nome
// do inimigo. Pegar = frente da cabeça a menos de duel_sign_deg do ponto, girando menos de duel_steady_dps, por
// duel_sign_hold_ms. Cada sinal pego soma duel_sign_gain ao foco, que nunca desce: o mod escreve +0x128 a cada frame
// do duelo (a câmera lenta +0x124 acompanha pela mola do jogo). Ossos do inimigo como no DuelMinigameEye::vf26:
// entrada DuelManager+0x40 + i·0xC (i = +0x194), ator = entrada − 0x2C.
// A tolerância e o tempo do sinal diminuem a cada duelo (g_diff, abaixo). ---
double now_s() {
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	return (double)t.QuadPart / f.QuadPart;
}
float rnd01() {
	static unsigned x = GetTickCount();
	x = x * 1103515245u + 12345u;
	return (x >> 8) / 16777216.f;
}
char* enemy_at(char* dm, unsigned i) {
	char* e = dm && i < 2 ? *(char**)(dm + 0x40 + i * 0xC) : NULL;
	return e ? e - 0x2C : NULL;
}
char* duel_enemy(char* d, char* dm) { return enemy_at(dm, *(unsigned*)(d + 0x194)); }
// Inimigos do duelo: m_BossCount da DuelArea (DuelManager+0xC → +0x94). Com 2 é o impasse mexicano (GhostTown na
// campanha e Sundance Kid no modo Duelo).
int boss_count(char* dm) {
	char* a = dm ? *(char**)(dm + 0xC) : NULL;
	return a ? *(int*)(a + 0x94) : 0;
}
float g_focus;   // foco dos sinais: 0..1, só sobe
int g_sign = -1;  // parte do sinal ativo (−1 = nenhum)
unsigned g_sign_enemy;  // inimigo do sinal ativo: o alvo do jogo quando o sinal acendeu
double g_sign_next;
void signs_reset() { g_focus = 0, g_sign = -1, g_sign_next = now_s() + 1; }

// --- A dificuldade dos sinais sobe a cada duelo, na campanha e no modo Duelo. O duelo é reconhecido comparando o
// preset da DuelArea (DuelManager+0xC) com as tabelas abaixo, tiradas do data/presets/duelarea_pre.def na ordem do
// arquivo, que é a ordem do jogo (no modo Duelo o silêncio mínimo cai em passo fixo; na campanha a desconcentração
// sobe). Os offsets que o editor registra (em 0x00ABBFA0) ficam +8 no objeto, como o m_Hardin (+0x74) e os tempos da
// mão (+0x120/+0x124): m_MinSilenceDuration +0x78, m_MaxSilenceDuration +0x7C, m_DeconcentrationSpeed +0x84.
// g_diff 0 = primeiro duelo (duel_sign_deg, duel_sign_ms), 1 = o último (duel_sign_deg_hard, duel_sign_ms_hard);
// preset desconhecido = 0. ---
struct DuelPreset {
	const char* name;
	float deconc, smin, smax;
};
const DuelPreset k_story[] = {{"Farm", .1f, 3, 4},           {"Prison", .122727f, 3, 4},    {"Ranch", .159091f, 4, 7},
                              {"Mine", .168182f, 3, 4},      {"Saloon", .190909f, 2, 3},    {"Steamboat", .213636f, 3, 4},
                              {"TrainRobbery", .236364f, 2, 4}, {"Bridge", .259091f, 2, 5}, {"Train", .2125f, 3, 5},
                              {"Woodcamp", .304545f, 4, 7},  {"GhostTown", .2625f, 5, 7},   {"Finale", .2625f, 5, 8}};
const DuelPreset k_dm[] = {
    {"PatGarret", .175f, 2.6f, 4.5f},          {"BobOlinger", .2875f, 2.538462f, 4.846154f},
    {"OldManClanton", .2875f, 2.476923f, 5.192308f}, {"CurlyBill", .2875f, 2.415385f, 5.538462f},
    {"JohnnyRingo", .2875f, 2.353846f, 5.884615f}, {"HenryPlummer", .225f, 1.5f, 3},
    {"JohnWesleyHardin", .2875f, 2.230769f, 6.576923f}, {"KidCurry", .2875f, 2.169231f, 6.923077f},
    {"EmmettDalton", .2875f, 2.107692f, 7.269231f}, {"GratDalton", .225f, 2.046154f, 7.615385f},
    {"GeorgeFlatNoseCurry", .2875f, 1.984615f, 7.961538f}, {"JimReed", .2875f, 1.923077f, 8.307692f},
    {"JesseJames", .2875f, 1.861538f, 8.653846f}, {"BillyTheKid", .2875f, 1.8f, 9},
    {"SundanceKid", .2875f, 9, 13}};
float g_diff;
void diff_detect(char* dm) {
	char* a = dm ? *(char**)(dm + 0xC) : NULL;
	g_diff = 0;
	if (!a) return;
	float dc = *(float*)(a + 0x84), lo = *(float*)(a + 0x78), hi = *(float*)(a + 0x7C);
	auto find = [&](const DuelPreset* t, int n) {
		for (int i = 0; i < n; i++)
			if (fabsf(t[i].deconc - dc) < .002f && fabsf(t[i].smin - lo) < .01f && fabsf(t[i].smax - hi) < .01f) return i;
		return -1;
	};
	const int ns = sizeof k_story / sizeof *k_story, nd = sizeof k_dm / sizeof *k_dm;
	int i = find(k_story, ns), j = i < 0 ? find(k_dm, nd) : -1;
	if (i >= 0) g_diff = i / float(ns - 1);
	if (j >= 0) g_diff = j / float(nd - 1);
	if (float f = setting("duel_diff_force", -1); f >= 0) g_diff = f;  // teste: dificuldade fixa
	logf("duelo: duelo %s (desconcentração %.4f, silêncio %.2f–%.2f s, %d inimigos) -> dificuldade %.2f: sinal %.1f° por %.0f ms",
	     i >= 0 ? k_story[i].name : j >= 0 ? k_dm[j].name : "desconhecido", dc, lo, hi, *(int*)(a + 0x94), g_diff,
	     setting("duel_sign_deg", 2.5f) + (setting("duel_sign_deg_hard", 1.5f) - setting("duel_sign_deg", 2.5f)) * g_diff,
	     setting("duel_sign_ms", 1400) + (setting("duel_sign_ms_hard", 900) - setting("duel_sign_ms", 1400)) * g_diff);
}
float by_diff(const char* easy, float e, const char* hard, float h) {
	float a = setting(easy, e);
	return a + (setting(hard, h) - a) * g_diff;
}
void signs(char* d, char* dm, int state) {
	static const char* const parts[3][3] = {{"Head"}, {"R_Hand"}, {"bone_holsterA", "R_Holster", "Pelvis"}};
	static const char* const names[3] = {"cabeça", "mão da arma", "coldre"};
	static double off, hold, last_t, flash;  // flash: anel verde por 0,3 s depois de pegar
	static float flash_k[3], flash_w;
	static float last_f[3], dps;  // giro da cabeça (°/s) filtrado em ~150 ms: medido frame a frame dava picos falsos
	double t = now_s();
	const float* C = player_head_cam();
	float f[3] = {-C[2], -C[6], -C[10]};
	if (t - last_t >= 0.005) {  // dois updates no mesmo frame não contam
		float a = acosf(fminf(1, f[0] * last_f[0] + f[1] * last_f[1] + f[2] * last_f[2])) * 57.29578f;
		if (last_t > 0) dps += (a / (float)(t - last_t) - dps) * fminf(1, (float)(t - last_t) / 0.15f);
		memcpy(last_f, f, sizeof f), last_t = t;
	}
	if (float ff = setting("duel_focus_force", -1); ff >= 0) g_focus = ff;  // teste: foco fixo
	*(float*)(d + 0x128) = g_focus;  // o foco é o dos sinais, do começo ao fim do duelo
	static const float white[3] = {1, 1, 1}, yellow[3] = {1, .9f, .2f}, green[3] = {.3f, 1, .3f};
	if (t < flash) vr_reticle(2, flash_k, flash_w, green);
	if (tut_paused()) {  // com uma dica do tutorial aberta os sinais somem e não dá para pegá-los
		g_sign = -1, g_sign_next = t + 1;
		return;
	}
	if (state < 3 || state > 5 || !duel_enemy(d, dm)) return void(g_sign = -1);
	auto gap = [] { float a = setting("duel_sign_gap_min", 1.5f); return a + rnd01() * (setting("duel_sign_gap_max", 3) - a); };
	bool fresh = g_sign < 0;
	if (fresh) {
		if (t < g_sign_next) return;
		g_sign = (int)(rnd01() * 3) % 3, off = t + by_diff("duel_sign_ms", 1400, "duel_sign_ms_hard", 900) / 1000, hold = 0;
		g_sign_enemy = *(unsigned*)(d + 0x194);
	} else if (g_sign_enemy != *(unsigned*)(d + 0x194))  // no impasse o alvo trocou (standoff()): o sinal vai com ele
		return void(g_sign = -1), void(g_sign_next = t + 0.3);
	char* en = enemy_at(dm, g_sign_enemy);
	float P[3];
	bool ok = false;
	for (const char* nm : parts[g_sign])
		if (nm && en && (ok = actor_bone_pos(en, nm, P))) break;
	if (!ok) return logf("duelo: sinal na %s sem osso no inimigo %u", names[g_sign], g_sign_enemy), void(g_sign = -1);
	if (fresh)
		logf("duelo: sinal na %s do inimigo %u (alvo do jogo %u), a %.1f m da cabeça", names[g_sign], g_sign_enemy,
		     *(unsigned*)(d + 0x194), sqrtf((P[0] - C[3]) * (P[0] - C[3]) + (P[2] - C[11]) * (P[2] - C[11])) / 100);
	float v[3] = {P[0] - C[3], P[1] - C[7], P[2] - C[11]}, n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	float ang = acosf(fminf(1, (v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / n)) * 57.29578f;
	float deg = by_diff("duel_sign_deg", 2.5f, "duel_sign_deg_hard", 1.5f);
	bool on = ang < deg && dps < setting("duel_steady_dps", 60);
	hold = on ? (hold > 0 ? hold : t) : 0;
	// anel: duel_sign_ring_deg de diâmetro angular (w em m); a área que conta (duel_sign_deg) é bem maior que ele
	float k[3], w = n / (100 * setting("world_scale", 1)) * 2 * tanf(setting("duel_sign_ring_deg", 0.8f) * 0.01745329f);
	game_to_trk(P, k);
	// Mira da cabeça: um ponto no centro da visão, na distância do sinal, mostra o que conta. Sem ele a pessoa mira com
	// os olhos, não com a cabeça, e o sinal na cabeça do inimigo podia ser pego olhando para o pé dele. O sinal é pego
	// com o ponto dentro do anel (duel_gaze 0 esconde o ponto).
	if (setting("duel_gaze", 1)) {
		float G[3] = {C[3] + f[0] * n, C[7] + f[1] * n, C[11] + f[2] * n}, kg[3];
		game_to_trk(G, kg);
		vr_reticle(3, kg, n / (100 * setting("world_scale", 1)) * 2 * tanf(setting("duel_gaze_deg", 1.2f) * 0.01745329f), white);
	}
	if (hold > 0 && t - hold >= setting("duel_sign_hold_ms", 500) / 1000) {
		g_focus = fminf(1, g_focus + setting("duel_sign_gain", 0.34f));
		logf("duelo: sinal na %s pego (foco %.2f)", names[g_sign], g_focus);
		memcpy(flash_k, k, sizeof k), flash_w = w, flash = t + 0.3;
		g_sign = -1, g_sign_next = t + gap();
	} else if (vr_reticle(2, k, w, hold > 0 ? yellow : white), t >= off) {
		logf("duelo: sinal na %s perdido (a %.1f°)", names[g_sign], ang);
		g_sign = -1, g_sign_next = t + gap();
	}
}
// --- As dicas "Look at your opponent" (Duel_Aim, Duel_TrackEnemy) esperam a mira do stick direito (ações 210–213),
// que em VR é a cabeça: olhar para a cabeça do inimigo (a menos de duel_hint_deg, por 300 ms) conclui a dica. Sem a
// cabeça aplicada (a dica pausa o jogo e a imagem fica parada) ela sai sozinha depois de duel_hint_s. ---
void aim_hint(char* d, char* dm) {
	static double since, shown;
	double t = now_s();
	if (!tut_expects(212) && !tut_expects(210)) return void(since = shown = 0);
	if (!shown) shown = t;
	char* en = duel_enemy(d, dm);
	float P[3];
	bool look = false;
	if (player_head_frame() && en && actor_bone_pos(en, "Head", P)) {
		const float* C = player_head_cam();
		float v[3] = {P[0] - C[3], P[1] - C[7], P[2] - C[11]}, n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-3f;
		look = acosf(fminf(1, -(v[0] * C[2] + v[1] * C[6] + v[2] * C[10]) / n)) * 57.29578f < setting("duel_hint_deg", 6);
	}
	since = look ? (since ? since : t) : 0;
	if ((since && t - since >= 0.3) || t - shown >= setting("duel_hint_s", 5)) {
		logf("dicas: dica da mira do duelo %s", since ? "concluída olhando o inimigo" : "concluída pelo tempo");
		tut_action(212), tut_action(210), since = shown = 0;
	}
}

// --- Impasse mexicano: olhar o outro inimigo troca o alvo do jogo (+0x194: o inimigo do foco, da câmera do duelo e do
// G_Eye). O jogo faz a troca pelas ações 216/217 no DuelController::vf207 (slot 207, 0x00ABFD50, thiscall (ação, valor,
// repetição), ret 0xC), que só aceita valor 1,0 sem repetição, com 2 inimigos e o DuelManager até o Fight (FUN_00AC3730);
// no pad isso é o LB, que o Touch não tem. Conta como olhar: a frente da cabeça a menos de duel_switch_deg do meio do
// corpo (entre a cabeça e a pélvis) e mais perto dele que do outro, por duel_switch_ms. ---
bool enemy_angle(char* en, const float* C, float& ang) {
	float H[3], B[3];
	if (!en || !actor_bone_pos(en, "Head", H)) return false;
	if (actor_bone_pos(en, "Pelvis", B))
		for (int i = 0; i < 3; i++) H[i] = (H[i] + B[i]) / 2;
	float v[3] = {H[0] - C[3], H[1] - C[7], H[2] - C[11]}, n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-3f;
	ang = acosf(fminf(1, -(v[0] * C[2] + v[1] * C[6] + v[2] * C[10]) / n)) * 57.29578f;
	return true;
}
// Yaw para o começo do duelo: o inimigo atual ou, no impasse, o meio entre os dois.
bool yaw_to_start(char* d, char* dm, float& psi) {
	float A[3], B[3];
	if (boss_count(dm) < 2 || !actor_bone_pos(enemy_at(dm, 0), "Head", A) || !actor_bone_pos(enemy_at(dm, 1), "Head", B))
		return yaw_to_enemy(d, psi);
	const float* C = player_head_cam();
	float dx = (A[0] + B[0]) / 2 - C[3], dz = (A[2] + B[2]) / 2 - C[11];
	if (dx * dx + dz * dz < 1) return false;
	return psi = atan2f(dz, dx) * 57.29578f, true;
}
void standoff(char* d, char* dm) {
	static double since;
	static int want = -1;
	if (boss_count(dm) < 2 || *(int*)(dm + 4) > 8 || tut_paused()) return void(want = -1);
	const float* C = player_head_cam();
	int best = -1;
	float best_a = setting("duel_switch_deg", 10), a;
	for (unsigned i = 0; i < 2; i++)
		if (enemy_angle(enemy_at(dm, i), C, a) && a < best_a) best = i, best_a = a;
	if (best < 0 || best == *(int*)(d + 0x194)) return void(want = -1);
	double t = now_s();
	if (best != want) want = best, since = t;
	if (t - since < setting("duel_switch_ms", 300) / 1000) return;
	((void(__thiscall*)(char*, int, float, char))(*(void***)d)[207])(d, 216 + best, 1.f, 0);
	logf("duelo: impasse: olhando o inimigo %d (a %.1f°) -> alvo do jogo %d", best, best_a, *(int*)(d + 0x194));
	want = -1;
}

// --- Saque = tirar a arma do coldre. Apertar o grip da mão dominante dentro da zona do quadril arma o saque; com o
// grip apertado, a mão andando duel_draw_cm no espaço do tracking (mexer a cabeça não conta) dá um pulso de GRAB
// (ação 199). Fora disso a 199 vale 0: descansar a mão no coldre com o grip apertado não trava o saque do jogo nem saca
// antes da hora. ---
int g_draw;      // frames restantes do pulso de GRAB
float g_dodge;   // −1..1 (esquerda..direita) pela inclinação da cabeça nas fases Draw e Fight
// Na fase Catch (2, depois do sinal para sacar), apertar o grip no coldre já saca, sem esperar a mão andar
// (duel_grip_draw): o foco só vale depois que o saque termina, e o inimigo do primeiro duelo do modo Duelo atira em 0,8 s.
void draw_gesture(char* d) {
	static bool was, armed;
	static float at[3];
	int h = dom_hand();
	float b[3], c[3], t[3];
	bool grip = vr_grip(h), ok = weapon_hand_body(h, b) && weapon_hand_trk(h, t);
	if (g_draw > 0) g_draw--;
	holster_center(c);
	// A mão sobre a arma no coldre vibra de leve até o saque (um pulso a cada 125 ms).
	static double buzz;
	if (ok && !grip && dist3(b, c) <= setting("hip_r", 16) && duel_gun_holstered(player_local()) && setting("duel_hover_buzz", 1) &&
	    now_s() >= buzz)
		vr_haptic(h, 0.04f, 120, setting("duel_hover_amp", 0.3f)), buzz = now_s() + 0.125;
	if (!grip || !ok) return was = grip, armed = false, void();
	if (!was && dist3(b, c) <= setting("hip_r", 16)) {
		armed = true, memcpy(at, t, sizeof at), logf("duelo: grip no coldre (%.0f %.0f %.0f cm)", b[0], b[1], b[2]);
		if (*(int*)(d + 0x3C) == 2 && setting("duel_grip_draw", 1)) armed = false, g_draw = 8, logf("duelo: saque pelo grip (GRAB)");
	}
	was = grip;
	if (armed && dist3(t, at) >= setting("duel_draw_cm", 10)) armed = false, g_draw = 8, logf("duelo: saque pela mão (GRAB)");
}

// --- Esquiva pela inclinação lateral da cabeça nas fases Draw e Fight: o deslocamento para o lado, em relação à linha
// até o inimigo, a partir de onde a cabeça estava no início da Draw. ---
void dodge(char* d) {
	static float ref[2], right[2];
	static bool in;
	const float* C = player_head_cam();
	int ph = *(int*)(d + 0x3C);
	float psi;
	if (ph != 3 && ph != 4) return in = false, g_dodge = 0, void();
	if (!in && yaw_to_enemy(d, psi))  // direita = frente girada 90° (convenção do jogo: frente = (cos ψ, sin ψ) em x, z)
		in = true, ref[0] = C[3], ref[1] = C[11], psi *= 0.01745329f, right[0] = -sinf(psi), right[1] = cosf(psi);
	if (!in) return;
	float lat = (C[3] - ref[0]) * right[0] + (C[11] - ref[1]) * right[1], g = lat / setting("duel_dodge_cm", 15);
	g = fmaxf(-1, fminf(1, g));
	if ((g > 0.5f) != (g_dodge > 0.5f) || (g < -0.5f) != (g_dodge < -0.5f)) logf("duelo: esquiva pela cabeça %.0f cm", lat);
	g_dodge = g;
}

// --- Speed % pelo saque real. Na vitória (FUN_00AC2640) o jogo manda ao placar a velocidade calculada em
// FUN_00AC3ED0: o "Total Draw Time" (+0x58), entre o pior e o melhor tempo da DuelArea (+0x124/+0x120, 0,35/0,15 s),
// vira 0..1. O jogo só copia o +0x11C (o saque interno, que o hk_mg20 deixa sempre no melhor) em +0x58 antes da fase
// Fight (FUN_00AC3650); na Fight o mod escreve ali o saque de verdade: do Grabing (o aperto no coldre) até o cano
// passar a duel_aim_cm do corpo do inimigo, mapeado de duel_speed_best_ms (100%) a duel_speed_worst_ms (0%), em tempo
// real. O tempo conta desde o Grabing porque a Fight só começa ~1 s depois dele. Sem mirar, o tempo corre até a
// vitória. ---
void real_speed(char* d, char* dm) {
	static double t0;
	static float e;  // tempo do saque (s), fixo depois de mirar
	static bool done;
	int ph = *(int*)(d + 0x3C);
	if (ph < 2) return t0 = 0, done = false, void();  // um duelo novo
	if (!t0 && *(int*)(d + 0x118) >= 2) t0 = now_s();
	char* a = dm ? *(char**)(dm + 0xC) : NULL;
	if (!t0 || ph > 4 || !a) return;
	float b = setting("duel_speed_best_ms", 300) / 1000, w = setting("duel_speed_worst_ms", 1000) / 1000;
	float s = fmaxf(0, fminf(1, (w - (done ? e : (float)(now_s() - t0))) / (w - b))), t1 = *(float*)(a + 0x120), t2 = *(float*)(a + 0x124);
	float best = fminf(t1, t2), worst = fmaxf(t1, t2);
	if (ph == 4) *(float*)(d + 0x58) = worst - s * (worst - best);
	if (done) return;
	float o[3], f[3], H[3], r = setting("duel_aim_cm", 30), best_d = 1e9f;
	bool ok = weapon_barrel(0, o, f), any = false;
	for (int e = 0; ok && e < 2 && e < boss_count(dm); e++) {  // no impasse, qualquer um dos dois inimigos
		if (!actor_bone_pos(enemy_at(dm, e), "Head", H)) continue;
		any = true;
		for (int i = 0; i <= 4; i++) {  // ponytail: o corpo do inimigo vira 5 pontos, da cabeça até 70 cm abaixo dela
			float v[3] = {H[0] - o[0], H[1] - 70 * i / 4.f - o[1], H[2] - o[2]}, k = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
			float x = v[0] - f[0] * k, y = v[1] - f[1] * k, z = v[2] - f[2] * k;
			if (k > 0) best_d = fminf(best_d, sqrtf(x * x + y * y + z * z));
		}
	}
	ok = ok && any;
	bool aim = best_d <= r;
	static double next_log;
	if (!aim && now_s() >= next_log)
		next_log = now_s() + 0.25, logf("duelo: saque em curso, cano a %.0f cm do corpo do inimigo%s", best_d, ok ? "" : " (sem cano ou inimigo)");
	if (aim)
		done = true, e = (float)(now_s() - t0), logf("duelo: saque real %.0f ms até o cano no inimigo -> Speed %.0f%% (melhor %.2f s, pior %.2f s)",
		                                             e * 1000, 100 * fmaxf(0, fminf(1, (w - e) / (w - b))), best, worst);
}

// --- PlayerDI::GetActionValue (vf186, slot 0x0134BF2C → 0x009DC100, thiscall (ação), ret 4, float em ST0). No duelo
// em VR o GRAB (199) vem do gesto de saque, a mão/esquiva (214/215) vem da cabeça e a mira do stick (210–213) fica em
// zero; duel_pad 1 devolve tudo ao pad (testes). Também atende a escada automática (ações 80/81). ---
typedef float(__thiscall* gav_t)(char*, int);
gav_t o_gav;
float __fastcall hk_gav(char* p, void*, int a) {
	float v = o_gav(p, a);
	if ((a == 80 || a == 81) && p == player_local()) {  // escada automática (player.cpp)
		float l = ladder_action(a);
		return l >= 0 ? l : v;
	}
	if ((a != 199 && (a < 210 || a > 215)) || setting("duel_pad", 0) || p != player_local() || !duel_active(p)) return v;
	if (a == 199) return g_draw > 0 ? 1.f : 0.f;
	if (a == 214) return fmaxf(0, -g_dodge);
	if (a == 215) return fmaxf(0, g_dodge);
	return 0;
}
// --- Mira do duelo. FUN_00AC0380 (stdcall (DuelController*), ret 4, chamado em 0x00ABFADE) leva o retículo
// 2D ao alvo do tiro +0x88 (o Shoot do duelo sai do cano para esse alvo, sem espalhamento). Com a arma fora do coldre,
// o alvo vira o raio do cano: origem +0x7C, direção +0xD8 e alvo a 100 m. ---
void __stdcall hk_aim(char* d) {
	((void(__stdcall*)(char*))0x00AC0380)(d);
	char* p = player_local();
	float o[3], f[3];
	if (!p || *(char**)(p + 0x580) != d || !duel_active(p) || duel_gun_holstered(p) || !player_head_frame() ||
	    !weapon_barrel(0, o, f))
		return;
	float* m = (float*)d;
	for (int i = 0; i < 3; i++) m[0x7C / 4 + i] = o[i], m[0xD8 / 4 + i] = f[i], m[0x88 / 4 + i] = o[i] + f[i] * 10000;
}
// --- Arma no coldre virtual. Até o saque, o PlayerDuelVis::vf110 prende o Holder da arma ao osso bone_holsterA do
// corpo de 3ª pessoa (FUN_009ACC30, stdcall (vis), ret 4, chamado em 0x009ABE62: Holder = holsterA · relação guardada
// uma vez em +0x2918). Com o HandHolder no coldre do mod, essa trava deixava a arma girada com o pitch da cabeça, fora
// da vista. Sem a trava o esqueleto da arma segue o HandHolder, como no FPP; duel_pin 1 devolve a trava do jogo. ---
int g_pins;  // diagnóstico do revólver piscando no coldre: frames em que a trava do jogo rodou (log por segundo)
void __stdcall hk_pin(char* vis) {
	if (player_head_frame() && setting("weapons", 1) && !setting("duel_pin", 0)) return;
	g_pins++;
	((void(__stdcall*)(char*))0x009ACC30)(vis);
}

// Com o foco pelos sinais, o círculo de foco do jogo (G_Reticule, bit 0x20 do HUD do duelo) fica escondido.
// FUN_00B6E9D0 é stdcall (bits), EDI = HUDDuel, ret 4, chamada com os bits do frame em 0x00B6EDAA (FUN_00B6EB80); o
// thunk limpa o bit na pilha e segue para ela.
extern "C" bool duel_hide_reticle() { return setting("duel_signs", 1) && !setting("duel_hud_reticle", 0); }
extern "C" void hud_bits_thunk();
asm(".globl _hud_bits_thunk\n"
    "_hud_bits_thunk:\n"
    "  call _duel_hide_reticle\n"
    "  test %al, %al\n"
    "  jz 1f\n"
    "  andl $0xFFFFFFDF, 4(%esp)\n"
    "1:\n"
    "  mov $0x00B6E9D0, %eax\n"
    "  jmp *%eax\n");

// Posições da cabeça nos últimos frames, para a bala do inimigo ir aonde o corpo estava duel_dodge_ms antes.
struct HeadAt {
	double t;
	float p[3];
} g_heads[128];
int g_nhead;
void heads_push() {
	const float* C = player_head_cam();
	HeadAt& h = g_heads[g_nhead++ % 128];
	h.t = now_s(), h.p[0] = C[3], h.p[1] = C[7], h.p[2] = C[11];
}
void head_ago(double dt, float o[3]) {
	const float* C = player_head_cam();
	o[0] = C[3], o[1] = C[7], o[2] = C[11];
	double t = now_s() - dt;
	for (int i = 1; i <= 128 && i <= g_nhead; i++) {
		const HeadAt& h = g_heads[(g_nhead - i) % 128];
		memcpy(o, h.p, sizeof h.p);
		if (h.t <= t) break;
	}
}

// --- Tiro do inimigo. A bala (passo em FUN_00F4CE40) sai mirando o corpo do jogador, que o mod esconde colapsando-o na
// cabeça com escala ε (weapon.cpp): as formas de acerto ficam com ~0,1 mm. Com a cabeça parada o raio passa a 0 cm e
// acerta (+0x118 = 2, +0x100 = PlayerDI, +0x104 = osso 40), mas com o headset a cabeça anda alguns milímetros durante o
// voo e as balas passavam direto. Por isso, antes do raio (FUN_00F4CCF0, stdcall (de = bala+0x5C, até = bala+0x74),
// ESI = bala, ret 8; chamado em 0x00F4D06C), um passo que vem na direção do jogador e cruza o corpo (eixo da cabeça
// até duel_hit_down cm abaixo, raio duel_hit_cm) passa a terminar exatamente no osso 40, com os mesmos floats; apontar
// para lá com conta ainda errava às vezes. duel_hit_cm 0 desliga. ---
extern "C" void __stdcall bullet_ray_before(char* b) {
	char* p = player_local();
	float k[3];
	if (!p || !duel_body(p) || !player_head_frame() || b[0x98] || !duel_bone_pos(40, k)) return;
	const float* C = player_head_cam();  // o corpo de agora decide o acerto; a mira usa a cabeça de duel_dodge_ms atrás
	float *a = (float*)(b + 0x5C), *e = (float*)(b + 0x74), h[3] = {C[3], C[7], C[11]};
	float d[3] = {e[0] - a[0], e[1] - a[1], e[2] - a[2]}, w[3] = {h[0] - a[0], h[1] - a[1], h[2] - a[2]};
	float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], t = dd > 0 ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd : 0;
	if (t <= 0) return;  // indo para longe (a bala do próprio jogador sai do cano)
	// No primeiro passo (+0x34 = caminho andado, zero na saída) a bala do inimigo é reapontada para onde a cabeça
	// estava duel_dodge_ms antes, com a mesma velocidade (+0x68). O jogo corrige a mira até ~100 ms antes do tiro: sem
	// isso, inclinar a cabeça não desviava. Com o jogador parado o ponto é o mesmo e o acerto também.
	if (*(float*)(b + 0x34) < 1 && dd > 0) {
		float L[3], g[3];
		head_ago(setting("duel_dodge_ms", 400) / 1000, L);
		for (int i = 0; i < 3; i++) g[i] = L[i] - a[i];
		float gn = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), *v = (float*)(b + 0x68);
		float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), sl = sqrtf(dd);
		if (gn > 300) {  // ainda longe do jogador (a bala sai do cano do inimigo)
			for (int i = 0; i < 3; i++) v[i] = g[i] / gn * sp, e[i] = a[i] + g[i] / gn * sl, d[i] = e[i] - a[i], w[i] = h[i] - a[i];
			t = (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd;
			logf("duelo: bala do inimigo mirada na cabeça de %.0f ms atrás (%.0f cm de onde ela está)", setting("duel_dodge_ms", 400),
			     sqrtf((L[0] - h[0]) * (L[0] - h[0]) + (L[1] - h[1]) * (L[1] - h[1]) + (L[2] - h[2]) * (L[2] - h[2])));
			if (t <= 0) return;
		}
	}
	t = fminf(1, t);
	float q[3] = {a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t};  // ponto do passo mais perto da cabeça
	float y = fmaxf(h[1] - setting("duel_hit_down", 70), fminf(h[1], q[1]));  // no eixo do corpo, na altura do ponto
	float r = sqrtf((q[0] - h[0]) * (q[0] - h[0]) + (q[1] - y) * (q[1] - y) + (q[2] - h[2]) * (q[2] - h[2]));
	if (r > setting("duel_hit_cm", 25)) return;
	e[0] = k[0], e[1] = k[1], e[2] = k[2];
	logf("duelo: bala do inimigo a %.0f cm do corpo levada ao corpo", r);
}
extern "C" void bullet_ray_thunk();
asm(".globl _bullet_ray_thunk\n"
    "_bullet_ray_thunk:\n"
    "  push %esi\n"
    "  call _bullet_ray_before@4\n"
    "  mov $0x00F4CCF0, %eax\n"
    "  jmp *%eax\n");

// --- FOV da câmera mono no duelo (ICameraTarget::vf7 do PlayerDuelVis, slot 0x0134321C → 0x009AA6A0, float em ST0).
// Os olhos do VR usam a projeção do HMD e o culling segue o olho, então os 20° de FOV do duelo não cortam a cena; o
// FOV mono só define onde o jogo projeta o que desenha no HUD. Igualado à altura angular do overlay do HUD (vr.cpp),
// o retículo e o olho do duelo caem sobre o inimigo. O CameraFPPDI divide pelo zoom (vf6 = câmeras +0x58), então o
// valor devolvido já sai multiplicado pelo zoom.
// ponytail: ignora o hud_y (−0,1 m deixa o retículo ~4° abaixo); se isso incomodar, somar um deslocamento. ---
typedef float(__thiscall* fov_t)(char*);
fov_t o_fov;
float __fastcall hk_fov(char* self, void*) {
	char* p = player_local();
	char* d = p ? *(char**)(p + 0x580) : NULL;
	char* cam = d ? *(char**)(d + 0x17C) : NULL;
	if (!cam || !duel_active(p) || !player_head_frame() || !setting("duel_hud_fov", 1)) return o_fov(self);
	float w = g_eye_w ? g_eye_w : setting("fake_eye_w", 2528), h = g_eye_h ? g_eye_h : setting("fake_eye_h", 2704);
	float half = setting("hud_width", 1.3f) * h / w / 2, fov = 2 * atanf(half / setting("hud_dist", 1.5f)) * 57.29578f;
	static float logged;
	if (fov != logged) logged = fov, logf("duelo: FOV mono do duelo %.1f° (o do jogo %.1f°)", fov, o_fov(self));
	return fov * *(float*)(cam + 0x58);
}
}  // namespace

unsigned hints_mask(char* p);
void duel_init() {
	static const uintptr_t slots[] = {0x01491ECC, 0x0149964C};  // vtables do Reticule e do Hardin, slot 20
	for (uintptr_t s : slots)
		if (*(uintptr_t*)s != 0x00ED6550) return logf("duelo: slot %p do minijogo não confere; duelo em VR desligado", (void*)s);
	for (uintptr_t s : slots) o_mg20 = (mg20_t)patch_ptr(s, (void*)hk_mg20);
	patch_call(0x00ABFADE, 0x00AC0380, (void*)hk_aim);
	if (*(uintptr_t*)0x0134BF2C == 0x009DC100) o_gav = (gav_t)patch_ptr(0x0134BF2C, (void*)hk_gav);
	if (*(uintptr_t*)0x0134321C == 0x009AA6A0) o_fov = (fov_t)patch_ptr(0x0134321C, (void*)hk_fov);
	bool pin = patch_call(0x009ABE62, 0x009ACC30, (void*)hk_pin);
	patch_call(0x00F4D06C, 0x00F4CCF0, (void*)bullet_ray_thunk);
	patch_call(0x00B6EDAA, 0x00B6E9D0, (void*)hud_bits_thunk);
	*(unsigned*)0x02E65A98 = hints_mask(NULL);  // antes do 1º duelo (as dicas nascem com o DuelController)
	logf("duelo: hooks do minijogo do retículo (vf20), mira (0x00ABFADE), GetActionValue %s, trava do coldre %s",
	     o_gav ? "ok" : "não confere", pin ? "ok" : "não confere");
}

// Corpo de 3ª pessoa do duelo no jogador (vis do duelo): do PreparingCharacters até a volta ao FPP, com a morte e a tela
// do fim (pausa em 3D) no meio.
bool duel_body(char* p) {
	if (!p || !setting("duel_vr", 1)) return false;
	char* vo = ((char*(__thiscall*)(char*))(*(void***)(p + 0x1C8))[1])(p + 0x1C8);
	return vo && *(uintptr_t*)vo == 0x01342C2C;
}

// Duelo em VR em curso: DuelController ativo e vis do duelo.
bool duel_active(char* p) {
	char* d = p ? *(char**)(p + 0x580) : NULL;
	return d && d[0x20] && duel_body(p);
}

// Fase do DuelController (+0x3C: 1 Warmup, 2 Catch, 3 Draw, 4 Fight, 5 Win, 6 Lose) no duelo em VR; 0 fora dele.
int duel_phase(char* p) { return duel_active(p) ? *(int*)(*(char**)(p + 0x580) + 0x3C) : 0; }

// A arma do duelo ainda no coldre: antes do Grabing (saque +0x118 < 2).
bool duel_gun_holstered(char* p) {
	return duel_active(p) && *(int*)(*(char**)(p + 0x580) + 0x118) < 2;
}

// Dicas do duelo (DuelTutorial). Cada uma é uma "PauseHint" (data/tutorials.scr): o jogo para, a imagem deixa de seguir
// a cabeça (olhar para baixo só move as mãos, não a cena) e o corpo de 3ª pessoa aparece. A máscara global 0x02E65A98,
// quando não é zero, é usada no lugar da do preset da DuelArea (FUN_00AC2C40: DuelManager+0xC → +0x68; bits 0x01 foco,
// 0x02 seguir, 0x04 mira, 0x08 esquiva, 0x10 desonra, 0x20 mão, 0x40 saque, 0x80 impasse). Com duel_hints 1 as dicas
// aparecem com os textos de VR (tutorial.cpp), menos as que não têm versão VR (duel_hints_off: 0x20 mão, que em VR
// é o saque real): a global vira a do preset sem esses bits, ou 0x100 (bit que nenhuma dica usa) se não sobrar nada;
// duel_hints 0 desliga todas. duel_tut_mask (teste) força uma máscara. FUN_00AC4060 (stdcall (DuelController*), ret 4,
// al) devolve 0 com uma dica na tela. Como as mãos já deram problema com a dica aberta, o log da troca diz por onde
// elas foram posicionadas (path: 0 weapon_camera, 1 weapon_paused, 2 nenhum).
unsigned hints_mask(char* p) {
	float m = setting("duel_tut_mask", -1);
	if (m >= 0) return (unsigned)m;
	if (!setting("duel_vr", 1)) return 0;
	char* dm = p && setting("duel_hints", 1) ? duel_manager(p) : NULL;
	char* a = dm ? *(char**)(dm + 0xC) : NULL;
	unsigned k = a ? *(unsigned*)(a + 0x68) & ~(unsigned)setting("duel_hints_off", 0x20) : 0;
	return k ? k : 0x100;
}
void duel_tutorial_log(char* p, int path) {
	unsigned mk = hints_mask(p);
	static unsigned last_mk;
	if (mk != last_mk) logf("dicas: máscara das dicas do duelo %#x", mk), last_mk = mk;
	*(unsigned*)0x02E65A98 = mk;
	// Diagnóstico do revólver piscando no coldre: por segundo de duelo, em quantos frames a câmera de update não
	// posicionou mãos e arma (weapon_camera) e quantas vezes a trava do jogo prendeu a arma ao coldre do corpo
	// escondido.
	static int frames, miss;
	static DWORD t_log;
	if (duel_body(p)) frames++, miss += path != 0;
	if (GetTickCount() - t_log >= 1000) {
		if (frames && (miss || g_pins)) logf("duelo: coldre: %d de %d frames de câmera sem weapon_camera, trava do jogo %d vezes", miss, frames, g_pins);
		frames = miss = g_pins = 0, t_log = GetTickCount();
	}
	char* d = p ? *(char**)(p + 0x580) : NULL;
	char* g = *(char**)0x02E65DB4;
	static int last = -1, last_path = -1;
	if (!d || !d[0x20] || !*(char**)(d + 0x28) || !g || !*(char**)(g + 0xCB0)) return void(last = -1);
	int on = !((bool(__stdcall*)(char*))0x00AC4060)(d);
	if (on != last || (on && path != last_path))
		logf("duelo: dica do duelo %s | mãos por %s | pausa %d | fase %d", on ? "na tela" : "fora",
		     path == 0 ? "weapon_camera" : path == 1 ? "weapon_paused" : "nenhum", player_paused(), *(int*)(d + 0x3C));
	last = on, last_path = path;
}

// Chamado no fim do update do LevelDI (player_update_end), com o jogador local.
void duel_frame(char* p) {
	static int state = -1, phase = -1, grab = -1, active = -1;
	static const char* vis = "";
	char* d = p ? *(char**)(p + 0x580) : NULL;
	char* dm = p ? duel_manager(p) : NULL;
	int s = dm ? *(int*)(dm + 4) : -1;
	char* vo = p ? ((char*(__thiscall*)(char*))(*(void***)(p + 0x1C8))[1])(p + 0x1C8) : NULL;  // vis atual: PlayerDI+0x1C8 → vf1
	const char* v = p ? vis_name(vo) : "";
	if (v != vis) {
		logf("duelo: vis do jogador %s -> %s", vis, v), vis = v;
		if (vo && *(uintptr_t*)vo == 0x01342C2C) dump_bones(vo);
	}
	if (s != state) {
		logf("duelo: DuelManager %p estado %s -> %s", (void*)dm, name_of(k_state, 15, state), name_of(k_state, 15, s));
		state = s;
		// No começo do duelo a cabeça vira para o inimigo (no impasse, para o meio dos dois): sem TELA antes, o
		// alinhamento da volta da TELA (player.cpp) não roda.
		float psi;
		if ((s == 1 || s == 3) && setting("duel_vr", 1) && yaw_to_start(*(char**)(p + 0x580), dm, psi)) player_face(psi);
		if (s == 3) signs_reset(), diff_detect(dm);
	}
	if (!d) return;
	int a = d[0x20];
	if (a != active) logf("duelo: DuelController %p %s", (void*)d, a ? "ativo" : "inativo"), active = a;
	if (!a) return;
	if (duel_active(p) && player_head_frame()) {
		draw_gesture(d), dodge(d), heads_push(), real_speed(d, dm), standoff(d, dm);
		if (setting("duel_signs", 1)) signs(d, dm, s);
	}
	if (duel_active(p)) aim_hint(d, dm);
	int ph = *(int*)(d + 0x3C), g = *(int*)(d + 0x118);
	if (ph != phase || g != grab) {
		char why[64];
		snprintf(why, sizeof why, "%s/%s -> %s/%s", name_of(k_phase, 7, phase), name_of(k_grab, 4, grab), name_of(k_phase, 7, ph),
		         name_of(k_grab, 4, g));
		phase = ph, grab = g;
		log_values(why, p, d, dm);
	}
	static LONGLONG last;
	LARGE_INTEGER t, fq;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&fq);
	if (setting("duel_log", 0) && (t.QuadPart - last) * 2 >= fq.QuadPart) last = t.QuadPart, log_values("amostra", p, d, dm);
}
