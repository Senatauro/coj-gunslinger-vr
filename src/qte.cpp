// QTE do tiroteio em VR. O QTEController do jogador (PlayerDI+0x5A0, ativo com +0x20; modo do jogador 34) passa por uma
// fila de alvos (+0x4C, handle do inimigo): para cada um, no estado 1 (+0x60) mostra um botão (+0x88: 0 a 3) e espera
// a ação 200 + esse índice (_ACTION_QTE_1…4, no vf207 do controller); o certo vai ao estado 2 e ao próximo alvo, o errado
// ao 3 (falha), e passar do tempo (+0x40, contado em +0x68) também falha. Em VR o jogador mira e atira de verdade: um
// disparo (gatilho apertado) com o cano de uma arma a menos de qte_aim_deg do alvo atual conta como o botão certo;
// errar não conta. Um anel amarelo marca o inimigo do alvo atual e o tempo de cada alvo cresce por qte_time_scale.
// qte_auto 1 manda o botão sozinho (teste).
#include "mod.h"
#include <math.h>
#include <windows.h>

namespace {
const char* rtti(char* o) {  // classe pelo RTTI do MSVC (vtable[-1] = COL, +0xC = TypeDescriptor, nome em +8)
	if (!o || IsBadReadPtr(o, 4) || IsBadReadPtr(*(char**)o - 4, 4)) return "?";
	char* col = (*(char***)o)[-1];
	return IsBadReadPtr(col + 0xC, 4) || IsBadReadPtr(*(char**)(col + 0xC) + 8, 4) ? "?" : *(char**)(col + 0xC) + 8;
}
double now_s() {
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	return (double)t.QuadPart / f.QuadPart;
}
// Objeto de um handle (0x00ECCB80, stdcall (handle), EDI = gerenciador de objetos do mundo), como o QTE acha o alvo.
extern "C" char* obj_of_handle(char* mgr, int h);
asm(".globl _obj_of_handle\n"
    "_obj_of_handle:\n"
    "  push %edi\n"
    "  push %ebx\n"
    "  push %esi\n"
    "  push %ebp\n"
    "  mov 20(%esp), %edi\n"
    "  push 24(%esp)\n"
    "  mov $0x00ECCB80, %ecx\n"
    "  call *%ecx\n"
    "  pop %ebp\n"
    "  pop %esi\n"
    "  pop %ebx\n"
    "  pop %edi\n"
    "  ret\n");
char* target_of(char* p, char* q) {
	int h = *(int*)(q + 0x4C);
	char *lvl = h ? *(char**)(p + 0x20) : NULL, *a = lvl ? *(char**)(lvl + 0x24) : NULL, *w = a ? *(char**)(a + 0x74) : NULL;
	char* mgr = w ? *(char**)(w + 0x5B0) : NULL;
	return mgr ? obj_of_handle(mgr, h) : NULL;
}
// Ponto de mira do alvo: o tronco (ou a cabeça) pelos ossos; sem eles, a posição do objeto 1,2 m acima. bone = qual.
bool aim_point(char* t, float P[3], const char** bone = NULL) {
	static const char* const bones[] = {"Spine2", "Spine1", "Spine", "Head"};
	for (const char* b : bones)
		if (actor_bone_pos(t, b, P)) return bone && (*bone = b), true;
	float out[3];
	const float* o = ((float*(__thiscall*)(char*, float*))(*(void***)t)[0xC0 / 4])(t, out);
	return o && (P[0] = o[0], P[1] = o[1] + 120, P[2] = o[2], bone && (*bone = "posição"), true);
}
// Ângulo (graus) entre a direção d e o ponto P visto de o; n = distância (cm).
float angle_to(const float* o, const float* d, const float* P, float* n) {
	float v[3] = {P[0] - o[0], P[1] - o[1], P[2] - o[2]};
	*n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-3f;
	return acosf(fminf(1, (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / *n)) * 57.29578f;
}
void press(char* q, int want) { ((void(__thiscall*)(char*, int, float, char))(*(void***)q)[207])(q, 200 + want, 1.f, 0); }
}  // namespace

void qte_frame(char* p) {
	char* q = p ? *(char**)(p + 0x5A0) : NULL;
	if (!q || IsBadReadPtr(q, 0x94)) return;
	static char* seen;
	if (q != seen) seen = q, logf("qte: controller %p %s", (void*)q, rtti(q));
	bool on = *(char*)(q + 0x20);
	int st = *(int*)(q + 0x60), want = *(int*)(q + 0x88);
	static bool was_on;
	static int was_st = -1;
	if (on != was_on || (on && st != was_st))
		logf("qte: %s, estado %d, botão %d, modo do jogador %d", on ? "ativo" : "parado", st, want, *(int*)(p + 0x5A8));
	was_on = on, was_st = st;
	static float prev[2];
	static bool ring, last_on;
	if (!on || !(g_vr || setting("fake_hmd", 0))) {
		last_on = false;
		if (ring) vr_reticle(3, NULL, 0), ring = false;
		return void(prev[0] = prev[1] = 0);
	}
	static int last_h;
	bool start = !last_on;
	last_on = true;
	if (int h = *(int*)(q + 0x4C); h && (h != last_h || start)) {  // alvo novo (ou QTE de novo): mais tempo para mirar com a mão
		last_h = h;
		char* t = h ? target_of(p, q) : NULL;
		*(float*)(q + 0x40) *= setting("qte_time_scale", 2);
		logf("qte: alvo %08x %p %s, %.2f s para acertar", h, (void*)t, rtti(t), *(float*)(q + 0x40));
	}
	if (st != 0 && st != 1) return;
	if (setting("qte_auto", 0)) {
		static double since;
		if (st != 1) return void(since = 0);
		if (!since) since = now_s();
		if (now_s() - since >= setting("qte_auto_ms", 300) / 1000) press(q, want), since = 0, logf("qte: botão %d enviado (auto)", want);
		return;
	}
	// Alvo atual: um anel amarelo no tronco dele (o inimigo só aparece no alvo quando chega ao ponto de ação dele).
	char* t = target_of(p, q);
	float P[3], n;
	const char* bone = NULL;
	const float* C = player_head_cam();
	float f[3] = {-C[2], -C[6], -C[10]}, h[3] = {C[3], C[7], C[11]};
	bool have = t && aim_point(t, P, &bone);
	static char* told;
	if (have && t != told) {
		float a = angle_to(h, f, P, &n);
		told = t, logf("qte: inimigo do alvo %p %s, mira em %s, a %.1f m e %.0f° da frente da cabeça", (void*)t, rtti(t), bone, n / 100, a);
	}
	if (have) {
		float k[3];
		angle_to(h, f, P, &n);
		game_to_trk(P, k);
		vr_reticle(3, k, n / (100 * setting("world_scale", 1)) * 2 * tanf(setting("qte_ring_deg", 3) * 0.01745329f), (const float[3]){1, .9f, .2f});
		ring = true;
	} else if (ring) {
		vr_reticle(3, NULL, 0), ring = false;
	}
	for (int k = 0; k < 2; k++) {
		float tr = weapon_trigger(k);
		bool shot = tr > 0.5f && prev[k] <= 0.5f;
		prev[k] = tr;
		float o[3], d[3];
		if (!shot) continue;
		if (!have || !weapon_barrel(k, o, d)) {
			logf("qte: tiro da mão lógica %d sem %s", k, have ? "cano" : "inimigo no alvo");
			continue;
		}
		float ang = angle_to(o, d, P, &n), head = 0, hn;
		head = angle_to(h, f, P, &hn);
		if (ang > setting("qte_aim_deg", 8)) {
			logf("qte: tiro da mão lógica %d fora do alvo (cano a %.1f°, cabeça a %.0f°)", k, ang, head);
			continue;
		}
		if (*(int*)(q + 0x60) == 0) *(int*)(q + 0x60) = 1;  // ainda sem o botão na tela: o tiro certeiro vale do mesmo jeito
		press(q, *(int*)(q + 0x88));
		logf("qte: tiro da mão lógica %d no alvo (a %.1f°, %.0f m) -> estado %d", k, ang, n / 100, *(int*)(q + 0x60));
		break;
	}
}
