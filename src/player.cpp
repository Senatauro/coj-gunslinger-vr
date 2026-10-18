// Jogador local em 6DoF: a cabeça do HMD vira a câmera, o olhar e a mira do jogo.
// Hooks principais: PlayerDI::OnFrame (aplica a cabeça antes do update do jogador), CameraFPPDI::vf8 (refaz a câmera
// de update sobre a âncora interpolada), GetEyePos e GetLookDir (olho e direção do "usar"), a conversão local -> mundo
// do movimento e MoveController::vf200 (velocidade). Aqui também ficam o foco da janela, o listener do som, a escada
// automática e a troca entre CENA (o mundo em 3D no headset) e TELA (o jogo numa tela plana virtual).
#include "mod.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
inline unsigned present_count() { return *(volatile unsigned*)game::present_counter; }
inline char* game_di() { return *(char**)0x02E65DB4; }

// PlayerDI do jogador local: [[[[GameDI]+0xA80]+0x68]+0x28] - 0x2C, a mesma cadeia do GetLocalPlayer (0x00A19780),
// com teste de nulo a cada passo.
char* local_player() {
	static const int chain[] = {0xA80, 0x68, 0x28};
	char* p = game_di();
	for (int off : chain) {
		if (!p) return NULL;
		p = *(char**)(p + off);
	}
	return p ? p - 0x2C : NULL;
}

// Yaw ψ em graus a partir da coluna 2 (trás) de uma matriz de câmera 3x4 row-major; a frente é -col2.
inline float yaw_of_cam(const float* m) { return atan2f(-m[10], -m[2]) * 57.29578f; }

// mark() anota uma letra por hook, na ordem em que rodam no frame (O OnFrame, C câmera, M movimento, S fim do update).
volatile LONG g_seq;                  // sem uso
unsigned g_seq_pc = ~0u;
char g_order[16], g_last_order[17];  // ordem do frame atual e do último frame completo (player_order)
int g_norder;
void mark(char c) {
	unsigned pc = present_count();
	if (pc != g_seq_pc) {
		if (g_norder && g_seq_pc != ~0u) {
			static int logged;
			if (logged++ < 5) logf("jogador: ordem no frame %u: %.*s", g_seq_pc, g_norder, g_order);
			memcpy(g_last_order, g_order, g_norder), g_last_order[g_norder] = 0;
		}
		g_seq_pc = pc, g_norder = 0;
	}
	if (g_norder < (int)sizeof g_order) g_order[g_norder++] = c;
}

// Log amostrado a cada 100 ms: o OnFrame decide se o frame entra na amostra e os outros hooks do mesmo frame seguem.
unsigned g_log_pc = ~0u;
bool want_log() { return present_count() == g_log_pc; }
bool sample_now() {
	static LARGE_INTEGER f, last;
	LARGE_INTEGER t;
	if (!f.QuadPart) QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	if ((t.QuadPart - last.QuadPart) * 10 < f.QuadPart) return false;
	last = t;
	g_log_pc = present_count();
	return true;
}

// --- PlayerDI::OnFrame (thiscall, ECX = PlayerDI+0x18) ---
typedef void(__thiscall* onframe_t)(char*);
onframe_t o_onframe;

// Pose de update do frame (do HMD ou sintética) convertida para o jogo; o primeiro frame válido recentra no yaw do
// jogador.
bool g_have_head;
float g_head[12];  // câmera do jogo derivada do HMD e da âncora
float g_hmd[12];   // pose de update do frame (tracking)

// Âncora (o pé, PlayerDI+0x390) lida logo depois do CameraFPPDI::vf8. Nesse ponto o motor já interpolou a posição
// entre os dois últimos passos do ODE pela fração do passo; depois, o fechamento do ODE grava o passo cru. Lida no
// OnFrame, a âncora anda aos saltos (0 ou 9 cm por frame) em vez de deslizar.
float g_anchor[3];
unsigned g_anchor_pc = ~0u;
const float* anchor_of(char* p) { return present_count() - g_anchor_pc <= 2 ? g_anchor : (float*)(p + 0x390); }

// Segundos desde o OnFrame anterior do jogador local (giro suave, descida do agachar e pose sintética que gira).
float g_dt;
void frame_dt() {
	static LONGLONG last;
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	g_dt = last && t.QuadPart > last ? (float)(t.QuadPart - last) / f.QuadPart : 0;
	if (g_dt > 0.1f) g_dt = 0.1f;
	last = t.QuadPart;
}

// Câmera = cabeça sobre a âncora, menos a descida do agachar (g_drop).
float g_drop;      // cm
float g_tela_psi;  // yaw do jogador ao entrar na TELA (ou ao recentrar)
void head_cam(const float hmd[12], const float anchor[3], float cam[12]) {
	pose_to_game(hmd, anchor, cam);
	cam[7] -= g_drop;
}

void update_head(char* p) {
	float* hmd = g_hmd;
	if (!vr_hmd_pose(true, hmd)) {
		if (!setting("fake_hmd", 0)) return;
		if (float dps = setting("fake_yaw_dps", 0)) setting_set("fake_yaw", fmodf(setting("fake_yaw", 0) + dps * g_dt, 360));
		fake_hmd_pose(hmd);
	}
	PoseState& ps = pose_state();
	if (!ps.calibrated) pose_recenter(hmd, *(float*)(p + 0x834)), g_tela_psi = *(float*)(p + 0x834);
	head_cam(hmd, anchor_of(p), g_head);
	g_have_head = true;
	static LONGLONG last;
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	if (t.QuadPart - last > f.QuadPart) {
		last = t.QuadPart;
		float psi, theta;
		game_angles(g_head, psi, theta);
		logf("jogador: HMD %.3f %.3f %.3f m -> câmera %.1f %.1f %.1f, ψ %.1f θ %.1f (jogador ψ %.1f) ψ0 %.1f", hmd[3], hmd[7], hmd[11],
		     g_head[3], g_head[7], g_head[11], psi, theta, *(float*)(p + 0x834), ps.psi0);
	}
}

// --- Troca automática entre CENA e TELA. Motivos para ir à TELA: jogo fora do estado 6 (pausa, loading), jogador num
// modo de sequência (tela_modes: morto 8, cinemática 11, diálogo 21, duelo 29) ou câmera FPP que não rodou no update
// (outra câmera ativa). Concentração (23/25) e Sense of Death (30) ficam em CENA, com a marcação pela cabeça e a
// esquiva pelo corpo. Entra na TELA na hora e só volta para a CENA depois de 10 frames seguidos sem motivo. ---
char g_why[48];        // motivo atual ("" = nenhum)
int g_scene_frames;    // frames seguidos sem motivo
unsigned g_head_pc = ~0u;  // contador de Present do frame em que a cabeça foi aplicada

bool tela_mode(int m) {
	if (m == 29 && setting("duel_vr", 1)) return false;  // duelo em VR: fica em CENA (duel.cpp)
	static char list[64];
	static int ids[16], n = -1;
	if (n < 0) {  // tela_modes do INI ([stereo]), padrão 8,11,21,29
		cfg_str("stereo", "tela_modes", "8,11,21,29", list, sizeof list);
		n = 0;
		for (char* t = strtok(list, ", "); t && n < 16; t = strtok(NULL, ", ")) ids[n++] = atoi(t);
	}
	for (int i = 0; i < n; i++)
		if (ids[i] == m) return true;
	return false;
}

const char* tela_reason(char* p, bool camera_ran) {
	static char buf[48];
	char* gd = game_di();
	if (!gd || !p) return "sem jogador";
	// Pausa (estados 7/8) e volta dela (9) ficam em CENA com o menu num painel (pause_3d 0 manda para a TELA). Na pausa
	// a câmera FPP não roda, por isso ali a falta dela não conta como motivo.
	int st = *(int*)(gd + 0xC3C);
	bool pause = st >= 7 && st <= 9 && setting("pause_3d", 1);
	if (st != 6 && !pause) return snprintf(buf, sizeof buf, "estado %d", st), buf;
	if (tela_mode(*(int*)(p + 0x5A8))) return snprintf(buf, sizeof buf, "modo %d", *(int*)(p + 0x5A8)), buf;
	if (!camera_ran && !pause) return "sem câmera FPP";
	if (!setting("head", 1)) return "head=0";
	return NULL;
}

bool scene_allowed() {
	int m = (int)setting("mode", 0);  // 1 = CENA forçada, 2 = TELA forçada
	return m == 1 || (m != 2 && g_scene_frames >= 10);
}

// A cabeça só controla a câmera em gameplay livre; no resto quem manda é o jogo (TELA).
bool head_allowed(char* p) {
	char* gd = game_di();
	return scene_allowed() && gd && *(int*)(gd + 0xC3C) == 6 && !tela_mode(*(int*)(p + 0x5A8)) && setting("head", 1);
}

// Stick direito, entregue pelo hook do XInput. Com a cabeça aplicada (CENA livre) o giro é do mod e o jogo não recebe
// o stick direito; nos outros casos (TELA, escada, Gatling, sequências) ele vai para o jogo.
volatile float g_rstick;

// Giro do mod: em snap, gira snap_deg quando o stick passa da borda (|x| > 0,7; rearma abaixo de 0,3); no suave
// (turn=1), gira a smooth_dps. O pivô é a cabeça.
// g_face: yaw pedido para a frente da cabeça (NAN = nenhum), aplicado no próximo OnFrame com a cabeça. O duelo usa
// para virar o jogador para o inimigo.
float g_face = NAN;

bool turn(char* p) {
	if (g_face == g_face) {
		float psi, theta;
		game_angles(g_head, psi, theta);
		float d = fmodf(g_face - psi + 540, 360) - 180;
		g_face = NAN;
		pose_turn(g_hmd, d);
		logf("duelo: cabeça virada %+.0f° para o inimigo, ψ0 %.1f", d, pose_state().psi0);
		return true;
	}
	if (*(int*)(p + 0x5A8) == 29) return false;  // sem giro no duelo (modo 29)
	float x = g_rstick, d = 0;
	static bool armed = true;
	bool snap = !setting("turn", 0);
	if (!snap) {
		float m = (fabsf(x) - 0.2f) / 0.8f;
		if (m > 0) d = (x > 0 ? m : -m) * setting("smooth_dps", 180) * g_dt;
	} else if (armed && fabsf(x) > 0.7f) {
		d = x > 0 ? setting("snap_deg", 45) : -setting("snap_deg", 45), armed = false;
	} else if (fabsf(x) < 0.3f) {
		armed = true;
	}
	if (!d) return false;
	pose_turn(g_hmd, d);
	if (snap) p[0x18D4] = 1, logf("jogador: giro snap %+.0f°, ψ0 %.1f", d, pose_state().psi0);  // +0x18D4: o corpo acompanha o giro sem a mola neste frame
	return true;
}

// --- Agachar. Físico: com a cabeça abaixo de 85% da altura em pé, o mod aperta DUCK (ação 15) pelo OnInput do jogador
// (0x009D60D0, ECX = PlayerDI+0x488); acima de 88%, solta. Só age nas bordas: se o jogo recusar, não insiste.
// Agachado (pelo corpo ou pelo botão B = DUCK_TOGGLE), a câmera fica no máximo a crouch_cap_cm do pé (o olho agachado
// do jogo fica a 99 cm, mais uma margem) e desce a 200 cm/s, para não atravessar o teto de passagens baixas.
// Agachado = PlayerDI+0x1144 > 0 (o instante em que agachou; −1 em pé). ---
bool g_phys_duck;
void crouch(char* p) {
	bool down = *(float*)(p + 0x1144) > 0;
	if (!setting("seated", 0) && setting("phys_crouch", 1)) {
		float r = g_hmd[7] * 100 / pose_state().height;
		if (g_phys_duck != (g_phys_duck ? r < 0.88f : r <= 0.85f)) {
			g_phys_duck = !g_phys_duck;
			((void(__thiscall*)(char*, int, float, bool))0x009D60D0)(p + 0x488, 15, g_phys_duck ? 1.f : 0.f, false);
			if (g_phys_duck) tut_action(15);  // o OnInput direto não passa pelo tutorial: avisa a dica "Use [B] to Duck"
			logf("jogador: cabeça a %.0f%% da altura: DUCK %s (agachado antes: %d)", r * 100, g_phys_duck ? "apertado" : "solto", down);
		}
	}
	float h = g_head[7] + g_drop - anchor_of(p)[1];  // altura da cabeça sobre o pé, sem a descida
	float want = down ? fmaxf(0, h - setting("crouch_cap_cm", 115)) : 0, step = 200 * g_dt;
	g_drop = g_drop < want ? fminf(want, g_drop + step) : fmaxf(want, g_drop - step);
}

// Escreve a cabeça na mira (+0x834/+0x838), no olhar (+0x83C/+0x840) e, com target, no alvo do look controller
// (+0x4F8, pitch limitado a −80°); zera as velocidades da mola (+0x844..+0x850).
void write_angles(char* p, float psi, float theta, bool target) {
	float* a = (float*)(p + 0x834);
	a[0] = a[2] = psi, a[1] = a[3] = theta;
	for (int i = 4; i < 8; i++) a[i] = 0;
	char* lc = target ? *(char**)(p + 0x4F8) : NULL;
	if (lc) *(float*)(lc + 0x30) = psi, *(float*)(lc + 0x34) = theta < -80 ? -80 : theta;
}

// --- Escada com um botão. O jogador monta pelo prompt do próprio jogo ("Climb": segurar o usar perto da escada, ou
// estender a mão livre até ela); com ladder_auto o mod leva até o topo, ou até o pé se montou por cima, com a tela
// escura. LadderController em PlayerDI+0x514 (vtable 0x013E9D84, Update vf200 0x00C20750): +0x20 ativo, +0xC0 estado
// (0 monta por baixo, 6 desliza até a escada, 1 pendurado, 2/4 e 3/5 monta por cima, 8/7 sobe/desce um degrau, 9 sai
// pelo topo, 10/11 solta), +0x7C degrau (0 = topo, cresce para baixo), +0x78 último degrau em que dá para ficar,
// timers por estado em +0x3C + 4·s. Pendurado, o jogo põe o jogador no degrau +0x7C no mesmo frame e lê as ações
// 80/81 (subir/descer, > 0,4) pelo GetActionValue (hk_gav, em duel.cpp). Subir: zera os timers 0 e 6, escreve o
// degrau 0 e segura 80; no degrau 0 o jogo toca a saída pelo topo (estado 9, a animação leva o jogador por cima da
// borda) e solta a escada. Descer: zera os timers 2–5 e 11, escreve o último degrau e segura 81; o jogo solta a escada
// a ~30 cm do chão. ---
static int g_lad;  // 0 parado, 80 subindo, 81 descendo
static LONGLONG g_lad_t0, g_lad_tend;
static float g_lad_top[3], g_lad_z[3];  // topo e eixo da escada, para medir onde a saída pelo topo deixa o jogador
static double lad_ms(LONGLONG t) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return qpc_ms(n.QuadPart - t); }
}  // namespace
bool ladder_busy() { return g_lad != 0; }
float ladder_fade() { return g_lad ? 1 : 0; }
float ladder_action(int a) { return g_lad ? (a == g_lad ? 1.f : 0.f) : -1.f; }
namespace {

void ladder_frame(char* p) {
	char* L = *(char**)(p + 0x514);
	bool on = L && *(int*)(p + 0x5A8) == 7 && L[0x20];
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!g_lad) {
		if (!on || !setting("ladder_auto", 1) || !g_have_head) return;
		int st = *(int*)(L + 0xC0), last = *(int*)(L + 0x78);
		float* tm = (float*)(L + 0x3C);
		if (st == 0 || st == 6) g_lad = 80, tm[0] = tm[6] = 0;
		else if (st >= 2 && st <= 5 && last > 0) g_lad = 81, tm[2] = tm[3] = tm[4] = tm[5] = tm[11] = 0;
		else return;  // escada curta, ou já pendurado: fica com o jogo (stick)
		g_lad_t0 = now.QuadPart, g_lad_tend = 0;
		memcpy(g_lad_top, L + 0x6C, 12), memcpy(g_lad_z, L + 0x98, 12);
		logf("escada: %s (estado %d, degraus 0..%d, saída pelo topo %.2f s)", g_lad == 80 ? "subindo" : "descendo", st,
		     last, tm[9]);
		return;
	}
	if (on) {
		// Com a ação segurada, o estado 1 (pendurado) dura zero frames: o Update termina o passo, põe o degrau e já
		// começa o próximo. Por isso o degrau é escrito no passo em curso. Subindo: degrau 1 e passo instantâneo
		// (timer 8 = 0); no fim dele o jogo vai ao degrau 0 (SetPosition no topo) e toca a saída. Descendo: penúltimo
		// degrau e timer 7 = 0. Sem isso o jogador subia degrau por degrau, uns 4,5 s no escuro.
		int st = *(int*)(L + 0xC0), last = *(int*)(L + 0x78);
		float* tm = (float*)(L + 0x3C);
		g_lad_tend = 0;
		int* rung = (int*)(L + 0x7C);
		if (g_lad == 80 && (st == 1 || st == 6)) tm[8] = 0, *rung = 0;
		if (g_lad == 80 && st == 8 && *rung > 1) tm[8] = 0, *rung = 1;  // no fim do passo: degrau 0 e a saída (sem laço)
		if (g_lad == 80 && st == 9 && setting("ladder_skip_getout", 1)) tm[9] = 0;  // a saída vira teleporte (abaixo)
		if (g_lad == 81 && (st == 1 || st == 3 || st == 5 || st == 7)) tm[7] = 0, *rung = st == 7 ? last - 1 : last;
	} else if (!g_lad_tend) {
		g_lad_tend = now.QuadPart;
		const float* a = (float*)(p + 0x390);  // onde o jogo deixou o pé, no referencial da escada
		float d[3] = {a[0] - g_lad_top[0], a[1] - g_lad_top[1], a[2] - g_lad_top[2]};
		logf("escada: solta; pé - topo: ao longo do eixo %.0f, lado %.0f, altura %.0f cm", d[0] * g_lad_z[0] + d[2] * g_lad_z[2],
		     d[0] * g_lad_z[2] - d[2] * g_lad_z[0], d[1]);
		// Subindo, a animação de saída (1,2 s) é pulada: o pé vai direto para onde ela o deixaria, medido numa escada
		// do jogo: 35 cm além do topo ao longo do eixo da escada e 3 cm acima.
		if (g_lad == 80 && setting("ladder_skip_getout", 1)) {
			float k = setting("ladder_out_cm", 35), to[3] = {g_lad_top[0] + k * g_lad_z[0], g_lad_top[1] + 3, g_lad_top[2] + k * g_lad_z[2]};
			((void(__thiscall*)(char*, float*))(*(void***)p)[0xB8 / 4])(p, to);
			logf("escada: teleporte para o topo (%.0f %.0f %.0f)", to[0], to[1], to[2]);
		}
	}
	double t = lad_ms(g_lad_t0);
	if ((g_lad_tend && lad_ms(g_lad_tend) > 150) || t > 6000) {  // fora da escada há 0,15 s (assentar) ou 6 s de teto
		logf("escada: terminou em %.0f ms%s", t, t > 6000 ? " (teto; o jogo segue com a escada)" : "");
		g_lad = 0;
	}
}

void __fastcall hk_onframe(char* self, void*) {
	char* p = self - 0x18;
	mark('O');
	bool head = false;
	float psi = 0, theta = 0;
	if (p == local_player()) {
		frame_dt();
		update_head(p);
		ladder_frame(p);
		if (g_have_head) teleport_frame(p);
		head = g_have_head && head_allowed(p);
		if (head) {
			crouch(p);
			bool faced = g_face == g_face;  // o pedido de olhar para o inimigo (aplicado em turn) vence a regra abaixo
			bool moved = turn(p);
			if (g_scene_frames == 10) {  // primeiro frame da CENA depois da TELA
				// A frente do mundo virtual vai para onde o jogador está olhando: a cabeça assume o yaw que o jogo deu
				// ao personagem durante a TELA (cutscene, início de duelo), mesmo que a pessoa tenha se virado no
				// quarto. Com tela_align 0, aplica só o giro que o jogo fez durante a TELA.
				float hp, ht;
				game_angles(g_head, hp, ht);
				float ref = setting("tela_align", 1) ? hp : g_tela_psi, d = fmodf(*(float*)(p + 0x834) - ref + 540, 360) - 180;
				if (!faced && fabsf(d) > 10)
					pose_turn(g_hmd, d), moved = true, logf("modo: volta da TELA: frente do mundo virtual girada %+.0f° para o olhar", d);
				p[0x18D4] = 1;
			}
			if (moved) head_cam(g_hmd, anchor_of(p), g_head);
			game_angles(g_head, psi, theta);
			static bool lc_logged;
			char* lc = *(char**)(p + 0x4F8);
			if (!lc_logged) lc_logged = true, logf("jogador: look controller %p alvo %.1f %.1f (olhar %.1f %.1f)", (void*)lc,
			                                       lc ? *(float*)(lc + 0x30) : 0.f, lc ? *(float*)(lc + 0x34) : 0.f,
			                                       *(float*)(p + 0x83C), *(float*)(p + 0x840));
			write_angles(p, psi, theta, true);
			g_head_pc = present_count();
			weapon_frame();
		}
	}
	o_onframe(self);
	if (head) {  // "mola": quanto o jogo afastou a mira e o olhar da cabeça dentro do OnFrame
		auto dpsi = [](float a, float b) { float d = fmodf(a - b + 540, 360) - 180; return fabsf(d); };
		stats_add("mola.mira", dpsi(*(float*)(p + 0x834), psi) + fabsf(*(float*)(p + 0x838) - theta));
		stats_add("mola.corpo", dpsi(*(float*)(p + 0x8B0), psi));
		write_angles(p, psi, theta, false);
	}
	static bool checked;
	if (!checked) checked = true, logf("jogador: OnFrame PlayerDI %p, jogador local %p", (void*)p, (void*)local_player());
	if (p != local_player()) return;
	float* a = (float*)(p + 0x834);  // mira ψ θ, olhar ψ θ, velocidades 0x844..0x850
	float* body = (float*)(p + 0x8B0);
	float* anc = (float*)(p + 0x390);
	float eye[3] = {0, 0, 0};
	((void(__thiscall*)(char*, float*))0x009DFF50)(p, eye);  // GetEyePos (vf119)
	char* gd = game_di();
	char* in = *(char**)0x0165D7B0;
	if (sample_now()) {
		logf("jogador: mira %.1f %.1f olhar %.1f %.1f vel %.1f %.1f %.1f %.1f | corpo %.1f %.1f %.1f | snap %d | âncora %.1f %.1f %.1f"
		     " olho %.1f %.1f %.1f (olho-âncora y %.1f) | estado %d modo %d | input %#x | agachado %.2f queda %.0f",
		     a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], body[0], body[1], body[2], p[0x18D4], anc[0], anc[1], anc[2],
		     eye[0], eye[1], eye[2], eye[1] - anc[1], gd ? *(int*)(gd + 0xC3C) : -1, *(int*)(p + 0x5A8),
		     in ? *(unsigned char*)(in + 0x14) : 0xFF, *(float*)(p + 0x1144), g_drop);
	}
}

// --- Fase do passo fixo do ODE. ODE = *(CLevel+0x58C); cursor +0x190 = tempo já simulado, passo +0x1A4 (0,02 s)
// vezes a escala de tempo (0x00547B50). O laço (0x00547BB0) simula enquanto relógio + passo − cursor ≥ passo, então a
// física fica até 1 passo à frente do relógio e α = (relógio + passo − cursor)/passo ∈ [0, 1) é a fração do passo. ---
char* g_level;  // LevelDI do último fim de update (player_update_end)

bool ode_phase(float& alpha, float& cursor) {
	char* lvl = g_level;
	char* ode = lvl ? *(char**)(lvl + 0x58C) : NULL;
	if (!ode || *(char**)(ode + 0x180) != lvl) return false;
	float now = ((float(__thiscall*)(char*))(**(void***)(lvl + 0xB58)))(lvl + 0xB58);
	float step = ((float(__thiscall*)(char*))0x00547B50)(ode) * *(float*)(ode + 0x1A4);
	if (!(step > 0)) return false;
	cursor = *(float*)(ode + 0x190);
	alpha = (now + step - cursor) / step;
	return true;
}

int g_trace;  // SET trace <frames>: log da âncora e da fase do ODE frame a frame (na câmera e no fim do update)
void trace(const char* where, char* p) {
	float a = -1, c = 0, *anc = (float*)(p + 0x390);
	ode_phase(a, c);
	logf("jogador: %s pc %u cursor %.4f α %.3f âncora %.2f %.2f %.2f", where, present_count(), c, a, anc[0], anc[1], anc[2]);
}

// Coleira: a cabeça anda livre até leash_cm da âncora no plano horizontal; nos 15 cm seguintes a visão escurece até
// o preto.
void leash(float dx, float dz) {
	float d = sqrtf(dx * dx + dz * dz), a = (d - setting("leash_cm", 80)) / 15;
	a = a < 0 ? 0 : a > 1 ? 1 : a;
	vr_fade(fmaxf(a, fmaxf(teleport_fade(), ladder_fade())));  // teleporte e escada também escurecem a visão
	static float shown;
	if ((a > 0) != (shown > 0)) logf("jogador: coleira %s (cabeça a %.0f cm do pé)", a > 0 ? "escurecendo" : "livre", d);
	shown = a;
}

// --- CameraFPPDI::vf8 (thiscall, sem args); câmera = [this+4] (IBaseCamera), mundo 3x4 em cam+0x34 ---
typedef void(__thiscall* camvf8_t)(char*);
camvf8_t o_camvf8;

void __fastcall hk_camvf8(char* self, void*) {
	mark('C');
	o_camvf8(self);
	char* cam = *(char**)(self + 4);
	char* p = local_player();
	if (p) memcpy(g_anchor, p + 0x390, sizeof g_anchor), g_anchor_pc = present_count();
	if (p && cam) gatling_anchor(p, (float*)(cam + 0x34), g_anchor);
	// Câmera de update = cabeça sobre a âncora interpolada; o SetFrame (vf5) refaz a inversa e a VP
	if (cam && p && g_head_pc == present_count()) {
		head_cam(g_hmd, g_anchor, g_head);
		float back[3] = {g_head[2], g_head[6], g_head[10]}, up[3] = {g_head[1], g_head[5], g_head[9]},
		      pos[3] = {g_head[3], g_head[7], g_head[11]};
		if (!setting("update_roll", 1)) up[0] = 0, up[1] = 1, up[2] = 0;
		auto setframe = (void(__thiscall*)(char*, float*, float*, float*))(*(void***)self)[5];
		setframe(self, back, up, pos);
		leash(pos[0] - g_anchor[0], pos[2] - g_anchor[2]);
		weapon_camera();
		duel_tutorial_log(p, 0);
	} else if (p && player_paused()) {
		weapon_paused();
		duel_tutorial_log(p, 1);
	} else if (p) {
		duel_tutorial_log(p, 2);
	}
	if (cam) {  // passo da câmera por frame no plano: com a âncora interpolada, constante ao andar (sem saltos de 0/9 cm)
		static float last[2];
		float* m = (float*)(cam + 0x34);
		stats_add("camera.passo", sqrtf((m[3] - last[0]) * (m[3] - last[0]) + (m[11] - last[1]) * (m[11] - last[1])));
		last[0] = m[3], last[1] = m[11];
	}
	if (p && g_trace) trace("câmera", p);
	if (!cam || !p || !want_log()) return;
	float* m = (float*)(cam + 0x34);
	logf("jogador: câmera de update pos %.1f %.1f %.1f ψ %.1f (mira %.1f) frente.y %.3f", m[3], m[7], m[11], yaw_of_cam(m),
	     *(float*)(p + 0x834), -m[6]);
}

// --- GetEyePos (vf119, thiscall (vec3* out)). Com a cabeça aplicada, o olho do jogador local (origem do tiro, do som
// do jogador, dos testes de visão) é a câmera, não o osso animado. ---
typedef float*(__thiscall* eyepos_t)(char*, float*);
eyepos_t o_eyepos;

float* __fastcall hk_eyepos(char* self, void*, float* out) {
	float* r = o_eyepos(self, out);
	if (self == local_player() && weapon_muzzle(out)) return r;  // dentro do Shoot do jogador, a origem é o cano da arma
	if (g_head_pc != present_count() || self != local_player()) return r;
	static void* callers[48];  // diagnóstico: quem lê o olho do jogador (log dos primeiros chamadores distintos)
	void* ra = __builtin_return_address(0);
	for (auto& c : callers)
		if (c == ra) break;
		else if (!c) { c = ra, logf("jogador: GetEyePos do jogador chamado de %p (osso %.1f %.1f %.1f -> câmera %.1f %.1f %.1f)", ra,
		                          out[0], out[1], out[2], g_head[3], g_head[7], g_head[11]); break; }
	out[0] = g_head[3], out[1] = g_head[7], out[2] = g_head[11];
	return r;
}

// --- GetLookDir (vf120, slot 0x0134BE24 → 0x009DCC80, thiscall (vec3* out), ret 4). Quando chamado de dentro da
// escolha do alvo do "usar" (FUN_00A01AF0..FUN_00A01D30, o Hud3dTrigger a cada 0,1 s), a direção vira a linha da
// cabeça até a mão livre estendida (weapon_use_dir); o resto do jogo continua com o olhar. ---
typedef float*(__thiscall* lookdir_t)(char*, float*);
lookdir_t o_lookdir;

float* __fastcall hk_lookdir(char* self, void*, float* out) {
	float* r = o_lookdir(self, out);
	uintptr_t ra = (uintptr_t)__builtin_return_address(0);
	static int calls, hand;
	static DWORD t_log;
	if (ra >= 0x00A01AF0 && ra < 0x00A02778 && self == local_player() && present_count() - g_head_pc <= 1)
		calls++, hand += weapon_use_dir(out);
	if (ra >= 0x00BDDA80 && ra < 0x00BDDBA7 && self == local_player()) gatling_lookdir(self, out);  // mira da gatling (FUN_00BDDA80)
	if (setting("use_log", 0) && GetTickCount() - t_log >= 2000)
		logf("usar: escolha do alvo: %d leituras do olhar, %d pela mão", calls, hand), calls = hand = 0, t_log = GetTickCount();
	return r;
}

// --- Conversão local -> mundo do movimento, só para log (call E8 em 0x009DDC28 -> FUN_005EC7B0; ECX, ESI = &local,
// EDI = &mundo) ---
extern "C" void move_log(const float* local, const float* world) {
	mark('M');
	if (!want_log()) return;
	if (local[0] || local[2]) logf("jogador: movimento local %.1f %.1f %.1f -> mundo %.1f %.1f %.1f", local[0], local[1], local[2],
	                               world[0], world[1], world[2]);
}

extern "C" void move_thunk();
asm(".globl _move_thunk\n"
    "_move_thunk:\n"
    "  mov $0x005EC7B0, %eax\n"
    "  call *%eax\n"  // a original, com ECX, ESI e EDI intactos; ela devolve EAX = EDI
    "  pushal\n"
    "  push %edi\n"
    "  push %esi\n"
    "  call _move_log\n"
    "  add $8, %esp\n"
    "  popal\n"
    "  ret\n");

// --- MoveController::vf200 (thiscall, float dt) ---
typedef void(__thiscall* mcvf200_t)(char*, float);
mcvf200_t o_mcvf200;

void __fastcall hk_mcvf200(char* mc, void*, float dt) {
	o_mcvf200(mc, dt);
	float* v = (float*)(mc + 0x38);
	// Fator de conforto (move_speed) na velocidade do jogador local com a cabeça aplicada, fora do sprint (+0x44)
	char* p = local_player();
	if (p && mc == *(char**)(p + 0x4F0) && present_count() - g_head_pc <= 1 && !mc[0x44]) {
		float k = setting("move_speed", 0.75f);
		v[0] *= k, v[1] *= k, v[2] *= k;
	}
	static bool who;
	if (p && !who && (v[0] || v[2])) who = true, logf("jogador: MoveController %p, PlayerDI+0x4F0 %p", (void*)mc, *(void**)(p + 0x4F0));
	static int n;
	if ((v[0] || v[2]) && n++ < 40) logf("jogador: MoveController v %.1f %.1f %.1f sprint %d", v[0], v[1], v[2], mc[0x44]);
}

// --- Foco da janela. Sem foco o jogo zera o bit 4 de CInput+0x14 (portão de todo o input), muta o som, baixa a
// prioridade e dá Sleep(200) na volta. Com o headset a janela quase nunca tem foco, então o mod esconde a perda. ---
WNDPROC g_wndproc;
HWND g_hwnd;

LRESULT CALLBACK focus_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (setting("keep_focus", 1)) {
		static int n;
		if (m == WM_ACTIVATEAPP && !w) {
			if (n++ < 5) logf("foco: WM_ACTIVATEAPP(0) trocado por 1");
			w = TRUE;
		} else if ((m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS) {
			if (n++ < 5) logf("foco: %s engolido", m == WM_KILLFOCUS ? "WM_KILLFOCUS" : "WM_ACTIVATE(WA_INACTIVE)");
			return 0;
		}
	}
	return CallWindowProcA(g_wndproc, h, m, w, l);
}

bool calls(uintptr_t site, uintptr_t target) { return *(uint8_t*)site == 0xE8 && site + 5 + *(int32_t*)(site + 1) == target; }

void patch_call(uintptr_t site, void* to) {
	DWORD old;
	VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old);
	*(int32_t*)(site + 1) = (int32_t)((uintptr_t)to - (site + 5));
	VirtualProtect((void*)site, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
}
}  // namespace

void player_init() {
	// Sem VR (nem fake_hmd) nenhum hook do jogador é instalado e o jogo fica igual ao original. [dev] player_log=1 instala
	// os hooks mesmo assim, só pelos logs (sem HMD a cabeça não é aplicada).
	if (!video_eye_mode() && !cfg_int("dev", "player_log", 0)) return logf("jogador: sem VR; hooks do jogador desligados");
	struct Slot {
		uintptr_t slot, expect;
	} const slots[] = {{0x0134C230, 0x009DA4A0}, {0x0138A8F0, 0x00ACAB80}, {0x013DD5A4, 0x00BAEFE0}, {0x0134BE20, 0x009DFF50}};
	for (auto& s : slots)
		if (*(uintptr_t*)s.slot != s.expect) return logf("jogador: slot %p não confere; hooks do jogador desligados", (void*)s.slot);
	if (!calls(0x009DDC28, 0x005EC7B0)) return logf("jogador: call de movimento em 0x009DDC28 não confere; hooks do jogador desligados");
	o_onframe = (onframe_t)patch_ptr(0x0134C230, (void*)hk_onframe);
	o_camvf8 = (camvf8_t)patch_ptr(0x0138A8F0, (void*)hk_camvf8);
	o_mcvf200 = (mcvf200_t)patch_ptr(0x013DD5A4, (void*)hk_mcvf200);
	o_eyepos = (eyepos_t)patch_ptr(0x0134BE20, (void*)hk_eyepos);
	patch_call(0x009DDC28, (void*)move_thunk);
	if (*(uintptr_t*)0x0134BE24 == 0x009DCC80) o_lookdir = (lookdir_t)patch_ptr(0x0134BE24, (void*)hk_lookdir);
	logf("jogador: hooks PlayerDI::OnFrame, CameraFPPDI::vf8, MoveController::vf200, GetEyePos, movimento 0x009DDC28; GetLookDir (usar pela mão) %s",
	     o_lookdir ? "ok" : "não confere");
	pose_selftest();
	weapon_init();
	duel_init();
	tutorial_init();  // dicas do tutorial adaptadas ao VR
	release_init();   // placar online desligado em VR e checagem de versão
}

void focus_install(HWND hwnd) {
	if (g_wndproc || !hwnd) return;
	g_hwnd = hwnd;
	g_wndproc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)focus_wndproc);
	logf("foco: WndProc do jogo %p trocada (foco mantido com VR)", (void*)g_wndproc);
}

// No Present: rede de segurança do portão de input e o teste do foco (SET focus_test 1).
void focus_frame() {
	if (!g_wndproc) return;
	if (float fps = setting("fake_hmd", 0) ? setting("fake_fps", 0) : 0) {  // sem headset: simular a cadência do HMD
		static LONGLONG next;
		LARGE_INTEGER t, f;
		QueryPerformanceFrequency(&f);
		QueryPerformanceCounter(&t);
		LONGLONG period = (LONGLONG)(f.QuadPart / fps);
		if (next - t.QuadPart > period || t.QuadPart - next > period) next = t.QuadPart;
		while (QueryPerformanceCounter(&t), t.QuadPart < next)
			if (next - t.QuadPart > f.QuadPart / 500) Sleep(1);
		next += period;
	}
	char* in = *(char**)0x0165D7B0;
	if (in && setting("keep_focus", 1) && !(in[0x14] & 4)) {
		static int n;
		if (n++ < 5) logf("foco: portão de input CInput+0x14 = %#x; bit 4 religado", (unsigned char)in[0x14]);
		in[0x14] |= 4;
	}
	// o jogo baixa a prioridade ao perder o foco e só devolve no próximo WM_ACTIVATE ativo
	if (setting("keep_focus", 1) && GetPriorityClass(GetCurrentProcess()) == BELOW_NORMAL_PRIORITY_CLASS)
		SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS), logf("foco: prioridade do processo de volta a NORMAL");
	if (setting("focus_test", 0)) {
		setting_set("focus_test", 0);
		logf("foco: teste de foco: enviando WM_ACTIVATEAPP(0), WM_ACTIVATE(WA_INACTIVE) e WM_KILLFOCUS; prioridade %#lx",
		     GetPriorityClass(GetCurrentProcess()));
		PostMessageA(g_hwnd, WM_ACTIVATEAPP, FALSE, 0);
		PostMessageA(g_hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
		PostMessageA(g_hwnd, WM_KILLFOCUS, 0, 0);
	}
	static LONGLONG last;
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t);
	QueryPerformanceFrequency(&f);
	if (t.QuadPart - last > 5 * f.QuadPart) {
		last = t.QuadPart;
		logf("foco: input %#x, prioridade %#lx, janela em foco %d", in ? (unsigned char)in[0x14] : 0xFF,
		     GetPriorityClass(GetCurrentProcess()), GetForegroundWindow() == g_hwnd);
	}
}

// --- Listener do som. O jogo o monta da câmera do update (FUN_004C3710: posição + cam+0x100, frente = −col2,
// "cima" = −col1, velocidade por diferença) e o entrega ao som pelo vf25 (thiscall, 6 args) de *(0x0165D7C4). Com a
// cabeça na câmera o som já segue o HMD; o hook só mede o erro contra a cabeça (stats som.erro, graus). ---
typedef uintptr_t(__thiscall* listener_t)(void*, float*, float*, float*, float*, int, void*);
listener_t o_listener;

uintptr_t __fastcall hk_listener(void* self, void*, float* pos, float* fwd, float* up, float* vel, int a5, void* cam) {
	if (present_count() - g_head_pc <= 1) {
		float hf[3] = {-g_head[2], -g_head[6], -g_head[10]}, dot = hf[0] * fwd[0] + hf[1] * fwd[1] + hf[2] * fwd[2];
		stats_add("som.erro", acosf(dot > 1 ? 1 : dot < -1 ? -1 : dot) * 57.29578f);
		static LONGLONG last;
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
		if (t.QuadPart - last > 2 * f.QuadPart)
			last = t.QuadPart, logf("jogador: listener pos %.1f %.1f %.1f (cabeça %.1f %.1f %.1f) frente %.3f %.3f %.3f vel %.0f %.0f %.0f",
			                        pos[0], pos[1], pos[2], g_head[3], g_head[7], g_head[11], fwd[0], fwd[1], fwd[2], vel[0], vel[1], vel[2]);
	}
	return o_listener(self, pos, fwd, up, vel, a5, cam);
}

// O VR só entra na CENA (e renderiza os olhos) com a cena liberada; player_tela_reason dá o motivo da TELA para o log
// do VR (NULL = nenhum).
bool player_scene() { return !o_onframe || scene_allowed(); }
const char* player_tela_reason() {
	if (!o_onframe || scene_allowed()) return NULL;
	return setting("mode", 0) == 2 ? "TELA forçada" : *g_why ? g_why : "voltando";
}

// Sense of Death (modo 30): o jogo escolhe o lado da esquiva pelo x do stick esquerdo (|x| > 0,3). Com dodge_lean=1,
// inclinar a cabeça mais de dodge_cm para um lado (no eixo direita-esquerda do corpo, a partir de onde ela estava
// quando o modo começou) empurra o stick esquerdo todo para esse lado; o analógico continua valendo. Devolve 0 sem
// esquiva pela cabeça.
float player_dodge() {
	static float ref[2];
	static bool in;
	char* p = local_player();
	if (!o_onframe || !p || present_count() - g_head_pc > 1 || *(int*)(p + 0x5A8) != 30 || !setting("dodge_lean", 1))
		return in = false, 0.f;
	float off[2] = {g_head[3] - g_anchor[0], g_head[11] - g_anchor[2]};
	if (!in) in = true, ref[0] = off[0], ref[1] = off[1];
	float psi = *(float*)(p + 0x834) * 0.01745329f;
	float lat = (off[0] - ref[0]) * -sinf(psi) + (off[1] - ref[1]) * cosf(psi);  // + = direita(ψ)
	float d = fabsf(lat) > setting("dodge_cm", 15) ? (lat > 0 ? 1.f : -1.f) : 0.f;
	static float last;
	if (d != last && d) logf("modo: esquiva pela cabeça para a %s (%.0f cm)", d > 0 ? "direita" : "esquerda", lat);
	last = d;
	return d;
}

// O hook do XInput entrega o stick direito; true = o mod usa (e o jogo recebe 0).
bool player_rstick(float x) {
	g_rstick = x;
	return o_onframe && present_count() - g_head_pc <= 1;
}

// Fim do update do LevelDI (chamado depois do CLevel::vf161, em stereo.cpp).
void player_update_end(char* lvl) {
	if (!o_onframe) return;  // sem os hooks do jogador (sem VR), nada a fazer
	if (lvl != g_level && g_level) pose_state().calibrated = false, logf("jogador: nível novo; recentrar no próximo frame");
	void** snd = *(void***)0x0165D7C4;
	if (!o_listener && snd) o_listener = (listener_t)patch_ptr((uintptr_t)*snd + 0x64, (void*)hk_listener), logf("jogador: hook do listener do som");
	g_level = lvl;
	mark('S');
	if (!g_trace && setting("trace", 0)) g_trace = (int)setting("trace", 0), setting_set("trace", 0);
	char* p = local_player();
	if (g_trace && p) g_trace--, trace("fim do update", p);
	if (p) {  // cada troca de modo do jogador e da câmera fixa por script vai para o log
		static int mode = -2, fixed = -1;
		int m = *(int*)(p + 0x5A8), f = (*(unsigned char*)(p + 0x730) & 4) != 0;
		if (m != mode) logf("modo: modo do jogador %d -> %d", mode, m), mode = m;
		if (f != fixed) logf("modo: câmera fixa por script %s", f ? "ligada" : "desligada"), fixed = f;
		duel_frame(p), gatling_frame(p);
	}
	const char* why = tela_reason(p, p && g_anchor_pc == present_count());
	if (why) {
		if (strcmp(why, g_why)) logf("modo: TELA (%s)", why), snprintf(g_why, sizeof g_why, "%s", why);
		if (g_scene_frames >= 10 && p) g_tela_psi = *(float*)(p + 0x834);
		g_scene_frames = 0;
	} else if (++g_scene_frames == 10) {
		logf("modo: CENA (10 frames sem motivo; saiu de: %s)", g_why);
		*g_why = 0;
	}
}

// Estado do jogador para os outros módulos (arma, duelo, VR).
char* player_local() { return o_onframe ? local_player() : NULL; }
bool player_head_frame() { return o_onframe && present_count() - g_head_pc <= 1; }
const float* player_head_cam() { return g_head; }
const float* player_hmd() { return g_hmd; }
const float* player_anchor() { return g_anchor; }
void player_mark(char c) { mark(c); }
void player_face(float psi) { g_face = psi; }
void player_to_game(const float trk[12], float out[12]) { head_cam(trk, anchor_of(local_player()), out); }
const char* player_order() { return g_last_order; }

// Pausa em 3D: estados 7/8 (pausa) e 9 (volta dela), e também a dica do tutorial que pausa o jogo (o tempo para e o
// jogador sai do input), com o HUD da dica num painel parado no mundo. pause_3d 0 e hint_pause_3d 0 desligam cada caso.
bool player_paused() {
	char* gd = game_di();
	int st = gd ? *(int*)(gd + 0xC3C) : 0;
	return (st >= 7 && st <= 9 && setting("pause_3d", 1)) || (tut_paused() && setting("hint_pause_3d", 1));
}

// Late latch: câmera da cabeça para o render deste frame (pose de render do WaitGetPoses, ou a sintética) sobre a
// âncora lida no CameraFPPDI::vf8, no mundo do jogo; false se a cabeça não foi aplicada no update deste frame. Na
// pausa em 3D o update do jogador para, mas a cabeça continua seguindo o HMD sobre a última âncora.
bool head_render(float cam[12]) {
	if (g_head_pc != present_count() && !(player_paused() && g_head_pc != ~0u)) return false;
	float hmd[12];
	if (!vr_hmd_pose(false, hmd)) {
		if (!setting("fake_hmd", 0)) return false;
		fake_hmd_pose(hmd);
	}
	head_cam(hmd, g_anchor, cam);
	return true;
}
