// Armas e mãos em VR: pose das mãos pelos controles, armas e mãos do próprio jogo desenhadas nessa pose, tiro pelo
// cano, dinamite, teleporte, coldres, menu de armas e "usar" pela mão.
// A arma de cada mão é filha do osso R_/L_HandHolder do corpo em primeira pessoa, e o render só enxerga os ossos (um
// SetWorld no objeto da arma não chega à imagem). Por isso o mod escreve o mundo desses ossos depois do
// CameraFPPDI::vf8, já com a âncora final do frame, e esconde os braços encolhendo os ossos deles.
#include "mod.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {
inline unsigned present_count() { return *(volatile unsigned*)game::present_counter; }

// Mundo de um osso: 0x006BAAB0 (EDI = objeto cujo +4 é o modelo, ESI = índice) devolve um mtx34*.
extern "C" float* bone_world(char* obj, int idx);
asm(".globl _bone_world\n"
    "_bone_world:\n"
    "  push %edi\n"
    "  push %esi\n"
    "  mov 12(%esp), %edi\n"
    "  mov 16(%esp), %esi\n"
    "  mov $0x006BAAB0, %eax\n"
    "  call *%eax\n"
    "  pop %esi\n"
    "  pop %edi\n"
    "  ret\n");

// bone_node: nó do osso no esqueleto, pelo mesmo caminho que o 0x006BAAB0 percorre. bone_slot: 0x0065C3B0 (EAX = nó)
// devolve o ponteiro do mundo do nó e marca os descendentes para recálculo.
extern "C" char* bone_node(char* obj, int idx);
extern "C" float* bone_slot(char* node);
asm(".globl _bone_node\n"
    "_bone_node:\n"
    "  mov 4(%esp), %eax\n"
    "  mov 4(%eax), %eax\n"
    "  add $0x2B4, %eax\n"
    "  push %eax\n"
    "  mov $0x004F9AE0, %eax\n"
    "  call *%eax\n"  // stdcall com 1 argumento: devolve o esqueleto do contexto da thread
    "  mov (%eax), %eax\n"
    "  mov 0xC(%eax), %eax\n"
    "  mov 0x18(%eax), %edx\n"
    "  mov 8(%esp), %ecx\n"
    "  lea (%ecx,%ecx,2), %ecx\n"
    "  lea (%edx,%ecx,8), %eax\n"
    "  ret\n"
    ".globl _bone_slot\n"
    "_bone_slot:\n"
    "  mov 4(%esp), %eax\n"
    "  mov $0x0065C3B0, %ecx\n"
    "  call *%ecx\n"
    "  ret\n");

// Mundo de um objeto de cena (nó = *(obj+0x88)). Chamado com o TLS do render, lê a cópia em T+0x08.
inline float* get_world(char* obj) { return ((float*(__thiscall*)(char*))0x004B61D0)(*(char**)(obj + 0x88)); }
// Índice de um osso pelo nome no modelo (vtable +0x3EC); -1 sem modelo.
int bone_index(char* model, const char* name) {
	return model ? ((int(__thiscall*)(char*, const char*))(*(void***)model)[0x3EC / 4])(model, name) : -1;
}

// Passa M (3x4 row-major; colunas direita, cima, trás, posição) para o frame da câmera C: R_Cᵀ·(R, p − p_C).
void to_cam(const float* C, const float* M, float* out) {
	for (int c = 0; c < 4; c++) {
		float v[3] = {M[c], M[4 + c], M[8 + c]};
		if (c == 3) v[0] -= C[3], v[1] -= C[7], v[2] -= C[11];
		for (int r = 0; r < 3; r++) out[r * 4 + c] = C[r] * v[0] + C[4 + r] * v[1] + C[8 + r] * v[2];
	}
}
void log_mat(const char* what, const float* C, const float* M) {
	float m[12];
	to_cam(C, M, m);
	logf("armas:   %-13s pos %6.1f %6.1f %6.1f | x %5.2f %5.2f %5.2f | y %5.2f %5.2f %5.2f | z %5.2f %5.2f %5.2f", what,
	     m[3], m[7], m[11], m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]);
}

// out = A·B (afins 3x4 row-major)
void mul34(const float* A, const float* B, float* out) {
	float o[12];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			o[r * 4 + c] = A[r * 4] * B[c] + A[r * 4 + 1] * B[4 + c] + A[r * 4 + 2] * B[8 + c] + (c == 3 ? A[r * 4 + 3] : 0);
	memcpy(out, o, sizeof o);
}
// [Ry(yaw)·Rx(pitch) | t] (graus; t em metros), no frame do OpenVR (y cima, −z frente)
void rot_yp(float yaw, float pitch, float tx, float ty, float tz, float* m) {
	float y = yaw * 0.01745329f, p = pitch * 0.01745329f, cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p);
	const float r[12] = {cy, sy * sp, sy * cp, tx, 0, cp, -sp, ty, -sy, cy * sp, cy * cp, tz};
	memcpy(m, r, sizeof r);
}

// --- Pose das mãos: pose crua do controle no WaitGetPoses (mesma predição da cabeça) × offset do controle no INI.
// O padrão é o do Quest 3: origem no handgrip e rotação do "tip" do modelo oculus_quest_plus_controller. Sem headset
// (fake_hmd), a mão é sintética, presa à cabeça sintética e girada por fake_hand_yaw/fake_hand_pitch. ---
char* g_vis;          // PlayerFppVis do jogador local (relido a cada frame)
inline char* player_vis(char* p) {  // PlayerFppVis do jogador: vf1 do subobjeto PlayerDI+0x1C8
	char* sub = p + 0x1C8;
	return ((char*(__thiscall*)(char*))(*(void***)sub)[1])(sub);
}
float g_hand[2][12];  // mundo do jogo: colunas direita, cima, trás, posição (cm)
bool g_hand_ok[2];

// --- Segurar pelo grip e mão dominante. A mão lógica k (o WFC e o HandHolder do jogo: 0 = R_, 1 = L_) fica na mão
// física phys(k), a que sacou a arma; a arma só aparece e só atira enquanto essa mão a segura (no modo com coldres,
// enquanto o grip está apertado). hold 0 desliga isso: as duas armas ficam sempre nas mãos e lógica = física. ---
int g_p0;           // mão física da mão lógica 0 (a lógica 1 fica na outra)
bool g_held[2];     // mão lógica k segurada
char* g_expect[2];  // item+0x14 pedido na troca: a arma só aparece quando o jogo entrega esse item
// Ao pegar a dinamite, as duas mãos lógicas se esvaziam (como no LB do jogo); depois do arremesso volta o que a outra
// mão física segurava (no modo com coldres, só se ela ainda estiver com o grip apertado).
int g_rs_slot[2] = {-1, -1}, g_rs_hand = -1, g_rs_k;  // slots de antes, a mão física que segurava e qual mão lógica
bool g_rs_pending;
inline int phys(int k) { return k ? 1 - g_p0 : g_p0; }
inline int dom() { return setting("left_handed", 0) ? 1 : 0; }  // mão dominante (física), a da arma única

// --- Duas mãos. A mão física livre que aperta o grip perto da arma da outra vira apoio. Arma longa com apoio: mira
// pela linha entre as mãos; sem apoio: mais espalhamento e mão "pesada" (filtro mais lento). Revólver com apoio: menos
// espalhamento e menos tremor. g_aim[k] = pose do Holder da arma k sem coice (x esquerda, y cima, z frente): é a usada
// no desenho, no cano e no ponto de mira. ---
bool g_support[2];      // mão física h apoia a arma da outra
bool g_long[2];         // mão lógica k está com a arma longa (slot 3)
int g_wtype[2] = {-1, -1};  // tipo do item na mão lógica k (0xF revólver, 0x11 rifle, 0x14 escopeta), −1 sem
char g_wname[2][24];        // modelo da arma na mão lógica k ("sixshooter" de Firearm_Sixshooter), "" sem
// Ajustes finos por arma (cm e graus; dá para mudar ao vivo com SET), pelo índice which: 0 arma (junto com a mão)
// para trás, 1 só a mão principal para trás, 2/3 mão de apoio para cima / para a esquerda, 4/5/6 giro da mão principal
// em torno da pega (pitch, yaw, roll, em graus), 7/8 mão principal para cima / para a esquerda, 9 arma (com a mão)
// para a esquerda (espelhado quando a arma está na mão esquerda). Cada modelo que o jogador pode pegar tem a própria
// variável, <ajuste>_<modelo> (ex.: hand_back_sixshooter), com o padrão da tabela md, porque modelos do mesmo tipo
// pedem pegas diferentes. Modelo fora da tabela usa o ajuste do tipo (<ajuste>_rev, _rifle ou _shotgun).
float wv(int k, int which) {
	static const char* base[10] = {"gun_back", "hand_back", "support_up", "support_left", "hand_pitch", "hand_yaw",
	                               "hand_roll", "hand_up", "hand_left", "gun_left"};
	static const struct { const char* model; float v[10]; } md[] = {
		// gun_back hand_back support_up support_left hand_pitch hand_yaw hand_roll hand_up hand_left gun_left
		{"sixshooter", {1.75f, -1.75f, 0, 0, 0, 0, 0, 0, 0}},  // revólveres (inventory.scr, ItemType_Revolver)
		{"schofield", {2, -7, 0, 0, -30, 0, 0, -1, -0.5f}},
		{"walker", {0, 0, 0, 0, 0, 0, 0, 0, 0}},  // zerado: já ajustado com a pega do akimbo
		{"duel", {2, -7, 0, 0, -30, 0, 0, -1, -0.5f}},
		{"duelquickshooter", {2, -7, 0, 0, -30, 0, 0, -1, -0.5f}},
		{"remington", {2, 0, 6, -5, 0, 0, 0, 0, 0}},  // escopetas
		{"sawnoff", {2, 0, 6, -5, 0, 0, 0, 0, 0}},
		{"winchester", {0, 0, 0, 0, 0, 0, 0, 0, 0}},  // rifles
		{"winchesternear", {0, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"winchestersawnoff", {0, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"winchestersniper", {0, 0, 0, 0, 0, 0, 0, 0, 0}}};
	if (*g_wname[k]) {
		char key[32];
		snprintf(key, sizeof key, "%s_%s", base[which], g_wname[k]);
		float d = NAN;
		for (auto& m : md)
			if (!strcmp(m.model, g_wname[k])) d = m.v[which];
		float v = setting(key, d);
		if (!isnan(v)) return v;
	}
	static const struct { const char* key[3]; float def[3]; } t[10] = {
		{{"gun_back_rev", "gun_back_rifle", "gun_back_shotgun"}, {2, 0, 2}},
		{{"hand_back_rev", "hand_back_rifle", "hand_back_shotgun"}, {-7, 0, 0}},
		{{"support_up_rev", "support_up_rifle", "support_up_shotgun"}, {0, 0, 6}},
		{{"support_left_rev", "support_left_rifle", "support_left_shotgun"}, {0, 0, -5}},
		{{"hand_pitch_rev", "hand_pitch_rifle", "hand_pitch_shotgun"}, {-30, 0, 0}},
		{{"hand_yaw_rev", "hand_yaw_rifle", "hand_yaw_shotgun"}, {0, 0, 0}},
		{{"hand_roll_rev", "hand_roll_rifle", "hand_roll_shotgun"}, {0, 0, 0}},
		{{"hand_up_rev", "hand_up_rifle", "hand_up_shotgun"}, {-1, 0, 0}},
		{{"hand_left_rev", "hand_left_rifle", "hand_left_shotgun"}, {-0.5f, 0, 0}},
		{{"gun_left_rev", "gun_left_rifle", "gun_left_shotgun"}, {0, 0, 0}}};
	int ty = g_wtype[k], i = ty == 0xF ? 0 : ty == 0x11 ? 1 : ty == 0x14 ? 2 : -1;
	return i < 0 ? 0 : setting(t[which].key[i], t[which].def[i]);
}
float g_holderl[2][3];  // HolderL da arma k no frame do Holder (cm)
float g_aim[2][12];
int g_fmode[2];         // filtro da mão física: 0 normal, 1 pesado (longa numa mão), 2 firme (revólver com apoio)
float g_trig[2];        // gatilho físico de cada mão (0..1), para os dedos sem o esqueleto do SteamVR
void holder_of(const float* G, float* m) {  // alvo do HandHolder de uma pose de mão (direita, cima, trás)
	for (int r = 0; r < 3; r++) m[r * 4] = -G[r * 4], m[r * 4 + 1] = G[r * 4 + 1], m[r * 4 + 2] = -G[r * 4 + 2], m[r * 4 + 3] = G[r * 4 + 3];
}
// Base (colunas x, y, z = f) com z = f e y o mais perto de up: x = up × f, y = f × x.
void basis(const float* f, const float* up, float* B) {
	float x[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
	float n = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) + 1e-9f;
	for (int i = 0; i < 3; i++) x[i] /= n;
	float y[3] = {f[1] * x[2] - f[2] * x[1], f[2] * x[0] - f[0] * x[2], f[0] * x[1] - f[1] * x[0]};
	for (int r = 0; r < 3; r++) B[r * 3] = x[r], B[r * 3 + 1] = y[r], B[r * 3 + 2] = f[r];
}
void aim_update() {
	for (int k = 0; k < 2; k++) {
		int h = phys(k), o = 1 - h;
		if (!g_hand_ok[h]) continue;
		float* A = g_aim[k];
		holder_of(g_hand[h], A);
		bool sup = g_support[o] && g_hand_ok[o];
		g_fmode[h] = g_long[k] ? (sup ? 0 : 1) : sup ? 2 : 0;
		if (!g_long[k] || !sup) continue;
		const float* S = g_hand[o];
		float u[3] = {S[3] - A[3], S[7] - A[7], S[11] - A[11]}, n = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
		const float *L = g_holderl[k], nl = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
		if (n < 5 || nl < 5) continue;  // mãos juntas demais ou arma sem HolderL: fica a pose da dominante
		float uw[3] = {L[0] / nl, L[1] / nl, L[2] / nl}, yw[3] = {0, 1, 0}, up[3] = {A[1], A[5], A[9]}, Bt[9], Bw[9];
		for (int i = 0; i < 3; i++) u[i] /= n;
		basis(u, up, Bt), basis(uw, yw, Bw);  // gira a arma: eixo Holder -> HolderL na linha das mãos, com o "cima" da dominante
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) A[r * 4 + c] = Bt[r * 3] * Bw[c * 3] + Bt[r * 3 + 1] * Bw[c * 3 + 1] + Bt[r * 3 + 2] * Bw[c * 3 + 2];
	}
	for (int k = 0; k < 2; k++) {  // ajuste por arma: a arma (com a mão) para trás pelo "frente" dela e para a esquerda
		float b = wv(k, 0), l = wv(k, 9) * (phys(k) == 1 ? -1.f : 1.f), *A = g_aim[k];
		if ((b || l) && g_hand_ok[phys(k)])
			for (int r = 0; r < 3; r++) A[r * 4 + 3] += -A[r * 4 + 2] * b + A[r * 4] * l;
	}
}

bool hand_tracking(int h, float m[12]) {
	float raw[12], T[12];
	float sx = h ? -1.f : 1.f;  // a mão esquerda espelha x
	if (vr_hand_pose(h, true, raw)) {
		rot_yp(0, setting("grip_pitch", -37.4f), sx * setting("grip_x", 0) / 100, setting("grip_y", 0.3f) / 100,
		       setting("grip_z", 9.7f) / 100, T);
		mul34(raw, T, m);
		return true;
	}
	if (!setting("fake_hmd", 0)) return false;
	fake_hmd_pose(raw);
	float fx = setting("fake_hand_x", 25), fy = setting("fake_hand_y", -35), fz = setting("fake_hand_z", 40);
	if (h) fx = setting("fake_hand_lx", fx), fy = setting("fake_hand_ly", fy), fz = setting("fake_hand_lz", fz);  // a mão esquerda tem posição própria
	rot_yp(-setting("fake_hand_yaw", 0), setting("fake_hand_pitch", 0), sx * fx / 100, fy / 100, -fz / 100, T);  // cm: direita, cima, frente
	mul34(raw, T, m);
	return true;
}

// --- Filtro da mão (One Euro), aplicado uma vez por frame para o desenho e o tiro usarem a mesma pose. Corte
// adaptativo fc = min_hz + beta·velocidade: com a mão parada, a tremedeira do tracking some; com ela rápida, quase não
// há atraso. Rotação por quaternion (velocidade em rad/s), posição em m/s. hand_filter 0 desliga. ---
struct Euro {
	float q[4], p[3], wq, wp;  // pose filtrada e velocidades filtradas
	bool ok;
};
Euro g_euro[2];

void mat_to_quat(const float* m, float* q) {  // 3x3 dentro de 3x4 row-major
	float t = m[0] + m[5] + m[10];
	if (t > 0) {
		float s = sqrtf(t + 1) * 2;
		q[0] = s / 4, q[1] = (m[9] - m[6]) / s, q[2] = (m[2] - m[8]) / s, q[3] = (m[4] - m[1]) / s;
	} else if (m[0] > m[5] && m[0] > m[10]) {
		float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
		q[0] = (m[9] - m[6]) / s, q[1] = s / 4, q[2] = (m[1] + m[4]) / s, q[3] = (m[2] + m[8]) / s;
	} else if (m[5] > m[10]) {
		float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
		q[0] = (m[2] - m[8]) / s, q[1] = (m[1] + m[4]) / s, q[2] = s / 4, q[3] = (m[6] + m[9]) / s;
	} else {
		float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
		q[0] = (m[4] - m[1]) / s, q[1] = (m[2] + m[8]) / s, q[2] = (m[6] + m[9]) / s, q[3] = s / 4;
	}
}
void quat_to_mat(const float* q, float* m) {
	float w = q[0], x = q[1], y = q[2], z = q[3];
	m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y - w * z), m[2] = 2 * (x * z + w * y);
	m[4] = 2 * (x * y + w * z), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z - w * x);
	m[8] = 2 * (x * z - w * y), m[9] = 2 * (y * z + w * x), m[10] = 1 - 2 * (x * x + y * y);
}
inline float euro_alpha(float hz, float dt) { return 1 / (1 + 1 / (6.2831853f * hz * dt)); }

void hand_filter(int h, float m[12], float dt) {
	Euro& e = g_euro[h];
	float q[4], p[3] = {m[3], m[7], m[11]};
	mat_to_quat(m, q);
	if (!e.ok || dt <= 0 || dt > 0.1f || !setting("hand_filter", 1)) {
		memcpy(e.q, q, sizeof q), memcpy(e.p, p, sizeof p), e.wq = e.wp = 0, e.ok = true;
		return;
	}
	float d = e.q[0] * q[0] + e.q[1] * q[1] + e.q[2] * q[2] + e.q[3] * q[3];
	if (d < 0) d = -d, q[0] = -q[0], q[1] = -q[1], q[2] = -q[2], q[3] = -q[3];  // mesmo hemisfério
	float ang = 2 * acosf(d > 1 ? 1 : d), dp = sqrtf((p[0] - e.p[0]) * (p[0] - e.p[0]) + (p[1] - e.p[1]) * (p[1] - e.p[1]) +
	                                                  (p[2] - e.p[2]) * (p[2] - e.p[2]));
	float ad = euro_alpha(1, dt);  // corte da derivada: 1 Hz
	e.wq += ad * (ang / dt - e.wq), e.wp += ad * (dp / dt - e.wp);
	float mz = g_fmode[h] == 2 ? setting("hand_min_hz_2h", 0.7f) : setting("hand_min_hz", 1.5f);
	float bt = g_fmode[h] == 1 ? setting("hand_beta_1h", 0.3f) : setting("hand_beta", 1.0f);
	float aq = euro_alpha(mz + bt * e.wq, dt);
	float ap = euro_alpha(setting("hand_pos_min_hz", 4.0f) + setting("hand_pos_beta", 20.0f) * e.wp, dt);
	float n = 0;
	for (int i = 0; i < 4; i++) e.q[i] += aq * (q[i] - e.q[i]), n += e.q[i] * e.q[i];
	n = 1 / sqrtf(n);
	for (int i = 0; i < 4; i++) e.q[i] *= n;
	for (int i = 0; i < 3; i++) e.p[i] += ap * (p[i] - e.p[i]);
	quat_to_mat(e.q, m);
	m[3] = e.p[0], m[7] = e.p[1], m[11] = e.p[2];
}
float g_trk[2][12];  // pose filtrada da mão no tracking, do frame (pré-OnFrame): desenho e tiro usam a mesma

void dump(char* vis, char* p) {
	char* sub = p + 0x1C8;  // o vis do jogador vem do vf1 desse subobjeto (é o que o GetMuzzlePos usa)
	char* pvis = ((char*(__thiscall*)(char*))(*(void***)sub)[1])(sub);
	const float *C = player_head_cam(), *anc = player_anchor(), *a390 = (float*)(p + 0x390);
	logf("armas: vis %p (do jogador %p), ordem %s; âncora no PlayerFppVis::vf110 %.1f %.1f %.1f, na câmera %.1f %.1f %.1f", (void*)vis, (void*)pvis,
	     player_order(), a390[0], a390[1], a390[2], anc[0], anc[1], anc[2]);
	char* body = vis + 0x20;  // subobjeto do corpo: vf2 = objeto com o modelo; 0x00E93420 = índice de osso
	char* bobj = ((char*(__thiscall*)(char*))(*(void***)body)[2])(body);
	for (int h = 0; h < 2; h++) {
		int hh = ((int(__thiscall*)(char*, const char*))0x00E93420)(body, h ? "L_HandHolder" : "R_HandHolder");
		if (hh >= 0) log_mat(h ? "L_HandHolder" : "R_HandHolder", C, bone_world(bobj, hh));
		char* wv = *(char**)(vis + 0x650 + 4 * h);
		if (!wv) {
			logf("armas: mão %d: sem arma", h);
			continue;
		}
		char *obj = *(char**)(wv + 0x14), *model = *(char**)(wv + 4), *node = obj ? *(char**)(obj + 0x88) : NULL;
		int ih = bone_index(model, "Holder"), ib = bone_index(model, "Barrel"), il = bone_index(model, "HolderL");
		logf("armas: mão %d: WeaponVis %p obj %p nó %p (pai %p; nó do HandHolder %p, do Holder %p) modelo %p; ossos Holder %d Barrel %d"
		     " HolderL %d", h, (void*)wv, (void*)obj, (void*)node, node ? *(void**)node : NULL, hh >= 0 ? (void*)bone_node(bobj, hh) : NULL,
		     ih >= 0 ? (void*)bone_node(wv, ih) : NULL, (void*)model, ih, ib, il);
		if (!obj) continue;
		log_mat("arma", C, get_world(obj));
		if (ih >= 0) log_mat("Holder", C, bone_world(wv, ih));
		if (ib >= 0) log_mat("Barrel", C, bone_world(wv, ib));
		if (il >= 0) log_mat("HolderL", C, bone_world(wv, il));
	}
}

// --- Hook do PlayerFppVis::vf110 (thiscall, sem args): guarda o vis do jogador; "SET weapon_dump 1" registra os ossos ---
typedef void(__thiscall* vf110_t)(char*);
vf110_t o_vf110;

void __fastcall hk_vf110(char* vis, void*) {
	o_vf110(vis);
	player_mark('V');
	char* p = player_local();
	if (!p || !player_head_frame()) return;
	g_vis = vis;
	if (setting("weapon_dump", 0)) setting_set("weapon_dump", 0), dump(vis, p);
}

// --- Tiro pelo cano. O hook do Shoot (hk_shoot) põe em g_shot a mão do WFC; enquanto isso, o GetEyePos (hook em
// player.cpp) devolve o cano e a direção base do tiro (hk_aimdir) vira o eixo do cano, ambos da pose da mão do frame
// (pré-OnFrame). O spread do jogo continua valendo; tiro com alvo explícito (Concentração, Sense of Death, QTE) também
// sai do cano. ---
int g_shot = -1;  // mão do Shoot em curso (−1 = fora)
float g_muzzle[3], g_dir[3];

// Cano e eixo da arma da mão lógica h na pose g_aim[h] (Holder: x esquerda, y cima, z frente). O cano fica em
// Holderᵀ·(B − H) no espaço do Holder (dá para usar os ossos do frame anterior, porque a relação é rígida) e o eixo é
// o z do Holder.
bool muzzle_of(int h, float pos[3], float dir[3]) {
	char* wv = g_vis ? *(char**)(g_vis + 0x650 + 4 * h) : NULL;
	char* model = wv ? *(char**)(wv + 4) : NULL;
	int ih = bone_index(model, "Holder"), ib = bone_index(model, "Barrel");
	if (ih < 0 || ib < 0) return false;
	const float *H = bone_world(wv, ih), *B = bone_world(wv, ib), *A = g_aim[h];
	float d[3] = {B[3] - H[3], B[7] - H[7], B[11] - H[11]}, o[3];
	for (int c = 0; c < 3; c++) o[c] = H[c] * d[0] + H[4 + c] * d[1] + H[8 + c] * d[2];
	for (int r = 0; r < 3; r++) pos[r] = A[r * 4 + 3] + o[0] * A[r * 4] + o[1] * A[r * 4 + 1] + o[2] * A[r * 4 + 2], dir[r] = A[r * 4 + 2];
	float lim = 0.9998f;  // sin 89°: o spread (0x00BE9E40) refaz a direção por yaw/pitch, e na vertical exata daria NaN
	if (fabsf(dir[1]) > lim) {
		float k = sqrtf((1 - lim * lim) / (dir[0] * dir[0] + dir[2] * dir[2] + 1e-12f));
		dir[0] *= k, dir[2] *= k, dir[1] = dir[1] > 0 ? lim : -lim;
	}
	return true;
}

// --- Coice visual (só no desenho: o tiro sai da pose em repouso) e vibração na mão que atirou. Curva do coice
// f(t) = (t/τ)·e^(1 − t/τ): pico 1 em τ = kick_ms, some em ~8τ. ---
LONGLONG g_kick_t0[2];
float kick_f(int h) {
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	float tau = setting("kick_ms", 45) / 1000, x = g_kick_t0[h] ? (float)(t.QuadPart - g_kick_t0[h]) / f.QuadPart / tau : 99;
	return x < 8 ? x * expf(1 - x) : 0;
}

// --- Raycast do jogo (0x004E67E0) com a assinatura da bala. Convenção: EAX = EDX = 0, EDI = saída de 0x24 B, pilha
// (PlayerDI+0x18, 0, 0xFF, &de, &para, 0x11, PlayerDI+0x10), ret 0x1C, resultado em AL; "para" volta encurtado até o
// acerto. Só na thread do update. Usado no teste cano × cobertura, no ponto de mira e nos arcos. ---
extern "C" int raycast(char* p, const float* from, float* to, void* out);
asm(".globl _raycast\n"
    "_raycast:\n"
    "  push %ebp\n"
    "  mov %esp, %ebp\n"
    "  push %edi\n"
    "  push %ebx\n"
    "  push %esi\n"
    "  mov 8(%ebp), %eax\n"
    "  mov 20(%ebp), %edi\n"
    "  lea 0x10(%eax), %ecx\n"
    "  push %ecx\n"        // ignorar = PlayerDI+0x10
    "  push $0x11\n"       // tipo da bala
    "  push 16(%ebp)\n"    // &para
    "  push 12(%ebp)\n"    // &de
    "  push $0xFF\n"       // subsistemas
    "  push $0\n"          // filtro
    "  lea 0x18(%eax), %ecx\n"
    "  push %ecx\n"        // ctx = PlayerDI+0x18
    "  xor %eax, %eax\n"
    "  xor %edx, %edx\n"
    "  mov $0x004E67E0, %ecx\n"
    "  call *%ecx\n"
    "  movzbl %al, %eax\n"
    "  pop %esi\n"
    "  pop %ebx\n"
    "  pop %edi\n"
    "  pop %ebp\n"
    "  ret\n");

// Cano × cobertura: raio da cabeça até o cano. Se bater, a origem do tiro vai para o contato − 2 cm, e a bala acerta a
// cobertura do lado de cá em vez de sair do cano enfiado nela.
void wall_test(char* p, float muzzle[3]) {
	if (!setting("wall_test", 1)) return;
	const float* C = player_head_cam();
	float from[3] = {C[3], C[7], C[11]}, to[3] = {muzzle[0], muzzle[1], muzzle[2]};
	char out[0x24 + 12];
	LARGE_INTEGER t0, t1, f;
	QueryPerformanceCounter(&t0);
	int hit = raycast(p, from, to, out);
	QueryPerformanceCounter(&t1), QueryPerformanceFrequency(&f);
	float d[3] = {muzzle[0] - from[0], muzzle[1] - from[1], muzzle[2] - from[2]}, len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float got[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]}, at = sqrtf(got[0] * got[0] + got[1] * got[1] + got[2] * got[2]);
	if (hit && len > 0 && at < len) {
		float k = fmaxf(0, at - 2) / len;
		for (int i = 0; i < 3; i++) muzzle[i] = from[i] + d[i] * k;
	}
	if (setting("weapon_log", 0))
		logf("armas: cabeça -> cano %.1f cm: %s %.1f cm (%.0f µs)", len, hit ? "bateu a" : "livre", at, (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart);
}

typedef bool(__stdcall* shoot_t)(char*, float, float, float);
bool __stdcall hk_shoot(char* wfc, float x, float y, float z) {
	char* p = player_local();
	int h = *(int*)(wfc + 0x160);
	bool gat = false;
	if (p && *(char**)(wfc + 0x28) == p && player_head_frame() && (h == 0 || h == 1) &&
	    ((gat = gatling_muzzle(p, g_muzzle, g_dir)) || (setting("weapons", 1) && g_hand_ok[phys(h)] && muzzle_of(h, g_muzzle, g_dir))))
		g_shot = h;  // na gatling o tiro sai da boca dela, na mira da mão de apoio
	player_mark('F');
	if (g_shot >= 0 && setting("weapon_log", 0))
		logf("armas: Shoot mão %d (ordem do frame anterior %s) cano %.1f %.1f %.1f direção %.3f %.3f %.3f", h, player_order(), g_muzzle[0], g_muzzle[1], g_muzzle[2], g_dir[0],
		     g_dir[1], g_dir[2]);
	if (g_shot >= 0 && !gat) wall_test(p, g_muzzle);  // na gatling o corpo dela fica entre a cabeça e a boca
	if (g_shot >= 0) {
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		g_kick_t0[h] = t.QuadPart;
		vr_haptic(phys(h), setting("haptic_ms", 60) / 1000, setting("haptic_hz", 160), setting("haptic_amp", 0.8f));
	}
	// Spread: a magnitude do cone (0x00BE9E40) é o acumulado WFC+0xA4/+0xA8 (graus) mais a perda por movimento. Em akimbo
	// sem ADS o acumulado fica parado em ~1,64° (medido), e a mão já é a mira: o mod o escala por spread_scale só
	// dentro do Shoot e devolve o valor do jogo depois (o acréscimo deste tiro se perde; o acumulado já está no teto).
	float* acc = (float*)(wfc + 0xA4);
	// Arma longa numa mão só espalha mais; revólver com apoio espalha menos.
	bool sup = g_shot >= 0 && g_support[1 - phys(h)], lg = g_shot >= 0 && g_long[h];
	float a0[2] = {acc[0], acc[1]};
	float k = lg && !sup ? setting("spread_1h", 0.5f) : !lg && sup ? setting("spread_2h", 0.05f) : setting("spread_scale", 0.15f);
	if (g_shot >= 0) acc[0] *= k, acc[1] *= k;
	bool r = ((shoot_t)0x00BE9000)(wfc, x, y, z);
	if (g_shot >= 0) acc[0] = a0[0], acc[1] = a0[1];
	g_shot = -1;
	return r;
}

// Direção base do tiro (0x00BEA2B0, stdcall (wfc, vec3* out)): dentro do Shoot do jogador, vira o eixo do cano.
typedef float*(__stdcall* aimdir_t)(char*, float*);
float* __stdcall hk_aimdir(char* wfc, float* out) {
	float* r = ((aimdir_t)0x00BEA2B0)(wfc, out);
	if (g_shot >= 0) memcpy(out, g_dir, sizeof g_dir);
	return r;
}

// Bullet::Launch do Shoot (call em 0x00BE9A63, EAX = bala, antes do init que copia o visual): o traçador (+0xCC, osso
// Barrel) sai da origem do tiro, que difere do cano visível quando o cano está dentro da cobertura (wall_test).
extern "C" void bullet_log(char* b) {
	if (g_shot >= 0) memcpy(b + 0xCC, b + 0x44, 12);
	if (!setting("weapon_log", 0)) return;
	float *o = (float*)(b + 0x44), *v = (float*)(b + 0x50), *t = (float*)(b + 0xCC), n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	logf("armas: bala origem %.1f %.1f %.1f direção %.3f %.3f %.3f traçador %.1f %.1f %.1f", o[0], o[1], o[2], v[0] / n, v[1] / n,
	     v[2] / n, t[0], t[1], t[2]);
}
extern "C" void launch_thunk();
asm(".globl _launch_thunk\n"
    "_launch_thunk:\n"
    "  push %eax\n"
    "  push %eax\n"
    "  call _bullet_log\n"
    "  add $4, %esp\n"
    "  pop %eax\n"
    "  mov $0x00F4C7F0, %ecx\n"
    "  jmp *%ecx\n");

// Recarga automática (StartReload 0x00BEE480, stdcall (char pelo jogador), EAX = WFC): com o pente vazio o jogo recarrega
// sozinho no update do WFC, no Shoot sem bala e ao sacar uma arma vazia (as três chamadas com 0). Em VR a do jogador só
// vem do X (a chamada com 1); auto_reload 1 devolve as automáticas. Os inimigos continuam recarregando.
extern "C" int reload_allowed(char* wfc) {
	char* p = player_local();
	if (setting("auto_reload", 0) || !p || *(char**)(wfc + 0x28) != p || !player_head_frame()) return 1;
	static int n;
	if (n < 5) n++, logf("armas: recarga automática bloqueada (WFC %p, mão %d)", (void*)wfc, *(int*)(wfc + 0x160));
	return 0;
}
extern "C" void reload_thunk();
asm(".globl _reload_thunk\n"
    "_reload_thunk:\n"
    "  push %eax\n"
    "  push %eax\n"
    "  call _reload_allowed\n"
    "  add $4, %esp\n"
    "  mov %eax, %ecx\n"
    "  pop %eax\n"
    "  test %ecx, %ecx\n"
    "  jz 1f\n"
    "  mov $0x00BEE480, %ecx\n"
    "  jmp *%ecx\n"
    "1:\n"
    "  ret $4\n");

// Recarga do akimbo em VR com a mecânica do jogo, nas duas armas ao mesmo tempo. O jogo faz uma de cada vez: com a
// recarga pedida nas duas (WFC no estado 3), o FUN_009E61A0 tira da mão a arma cujo WFC está parado ou esperando
// enquanto o da outra recarrega (slot desejado zerado pela chamada em 0x009E6531), e o update do WFC só começa a
// recarga (FUN_00BEB520, estado 4) quando a outra mão está vazia. Com akimbo_reload 1, em VR: a outra arma fica na mão
// (a chamada em 0x009E6531 é pulada), o WFC esperando começa já (begin_reload por frame) e, com as duas recarregando
// juntas, a escala de tempo da recarga (0x00BF4090, usada pela lógica e pela animação) cresce por 1/akimbo_reload_speed
// (0,7: 30% mais lenta). O resto é do jogo: bala por bala ou pente inteiro, tiro interrompendo, toques no X com a
// habilidade.
bool g_akpar asm("_g_akpar") __attribute__((used));  // akimbo em VR com a recarga paralela ligada (atualizado por frame)
extern "C" void holster_thunk();
asm(".globl _holster_thunk\n"
    "_holster_thunk:\n"
    "  cmpb $0, _g_akpar\n"
    "  jne 1f\n"
    "  push $0x0098CAB0\n"
    "  ret\n"
    "1:\n"
    "  ret\n");
extern "C" void begin_reload(char* wfc);
asm(".globl _begin_reload\n"
    "_begin_reload:\n"
    "  mov 4(%esp), %eax\n"
    "  push %ebx\n"
    "  push %esi\n"
    "  push %edi\n"
    "  push %ebp\n"
    "  push $0\n"
    "  mov $0x00BEB520, %ecx\n"
    "  call *%ecx\n"
    "  pop %ebp\n"
    "  pop %edi\n"
    "  pop %esi\n"
    "  pop %ebx\n"
    "  ret\n");
int wfc_state(char* p, int k) {
	char* w = *(char**)(p + 0x538 + 4 * k);
	return w ? *(int*)(w + 0x30) : -1;
}
extern "C" float __stdcall reload_scale(char* wfc) {
	float s = ((float(__stdcall*)(char*))0x00BF4090)(wfc);
	char* p = player_local();
	if (g_akpar && p) {
		int a = wfc_state(p, 0), b = wfc_state(p, 1);
		if ((a == 3 || a == 4) && (b == 3 || b == 4)) s /= fmaxf(0.1f, setting("akimbo_reload_speed", 0.7f));
	}
	return s;
}
// Por frame, no update: liga o modo e começa a recarga do WFC que ficou esperando a outra mão.
void akimbo_reload_frame(char* p) {
	bool was = g_akpar;
	g_akpar = setting("akimbo_reload", 1) && player_head_frame() && *(char**)(p + 0x538) && *(char**)(p + 0x53C) &&
	          *(char**)(p + 0xA84) && *(char**)(p + 0xA90);
	if (g_akpar != was) logf("armas: recarga paralela do akimbo %s", g_akpar ? "ligada" : "desligada");
	for (int k = 0; g_akpar && k < 2; k++)
		if (wfc_state(p, k) == 3) begin_reload(*(char**)(p + 0x538 + 4 * k)), logf("armas: recarga da mão lógica %d começada junto", k);
}

inline char* body_obj(char* body) { return ((char*(__thiscall*)(char*))(*(void***)body)[2])(body); }
inline int body_bone(char* body, const char* name) { return ((int(__thiscall*)(char*, const char*))0x00E93420)(body, name); }
}  // namespace

bool patch_call(uintptr_t site, uintptr_t expect, void* to) {
	uint8_t* s = (uint8_t*)site;
	if (*s != 0xE8 || site + 5 + *(int32_t*)(s + 1) != expect) return logf("armas: call em %p não confere", (void*)site), false;
	DWORD old;
	VirtualProtect(s, 5, PAGE_EXECUTE_READWRITE, &old);
	*(int32_t*)(s + 1) = (int32_t)((uintptr_t)to - (site + 5));
	VirtualProtect(s, 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), s, 5);
	return true;
}

// Para o hook do GetEyePos (player.cpp): dentro do Shoot do jogador, a origem do tiro é o cano da mão que atirou.
bool weapon_muzzle(float out[3]) {
	if (g_shot < 0) return false;
	memcpy(out, g_muzzle, sizeof g_muzzle);
	return true;
}

// Cano e eixo da arma da mão lógica k na pose do frame (o mesmo raio do tiro e do ponto de mira).
bool weapon_barrel(int k, float pos[3], float dir[3]) { return g_vis && muzzle_of(k, pos, dir); }
// Gatilho físico da mão que segura a arma da mão lógica k (0..1); 0 sem arma nessa mão lógica.
float weapon_trigger(int k) { return g_vis && *(char**)(g_vis + 0x650 + 4 * k) ? g_trig[phys(k)] : 0; }
void hand_body(int h, float o[3]);
// Mão física h no frame do corpo (cm: x direita, y cima, z frente), como as zonas dos coldres.
bool weapon_hand_body(int h, float o[3]) { return g_hand_ok[h] && (hand_body(h, o), true); }
// Pose da mão física h no mundo do jogo (3x4, cm); NULL sem pose.
const float* weapon_hand_game(int h) { return (h == 0 || h == 1) && g_hand_ok[h] ? g_hand[h] : NULL; }
// Posição da mão física h no tracking (cm): o saque do duelo mede o movimento da mão, não da mão em relação à cabeça.
bool weapon_hand_trk(int h, float o[3]) {
	for (int i = 0; i < 3; i++) o[i] = g_trk[h][i * 4 + 3] * 100;
	return g_hand_ok[h];
}

// --- Dinamite na mão. Selecionar (stick direito para baixo = D-pad esquerdo, slot 5) põe a dinamite na mão lógica 0;
// o modo do jogador vira 4 (arremesso). Gatilho segurado = o arco aparece e soltar o gatilho lança por ele; grip
// segurado = o mod segura o RT e soltar o grip lança com a velocidade da mão. O jogo lança no evento da animação; o
// mod zera o tempo do evento (WTC+0x44/+0x50) para lançar na soltura, troca o ponto de saída pela mão (0x00BFAB20) e
// grava a velocidade no objeto (obj+0xBC: o "impulso" do jogo vale como velocidade em cm/s, 1:1, medido). ---
int g_dyn_src;            // quem segura o arremesso: 0 nenhum, 1 gatilho (arco), 2 grip (braço)
float g_hand_v[2][3];     // velocidade de cada mão no mundo do jogo (cm/s; inclui andar, vale para o arremesso)
float g_trk_v[2][3];      // velocidade de cada mão no tracking (cm/s; só o braço, vale para o golpe)
LONGLONG g_throw_qpc;     // último arremesso (QPC)
bool g_throwing;          // dentro do ThrowInventoryItem
char* g_thrown;           // objeto do último arremesso

inline bool dyn_in_hand(char* p) { return p && *(int*)(p + 0x5A8) == 4 && *(char**)(p + 0x544); }

// Velocidade de saída do arco: frente da mão × 1500 + 300 para cima (a fórmula do jogo com a mão no lugar do olhar).
void arc_velocity(float v[3]) {
	const float* G = g_hand[phys(0)];
	float sp = setting("dyn_speed", 1500), up = setting("dyn_up", 300);
	for (int i = 0; i < 3; i++) v[i] = -G[i * 4 + 2] * sp;
	v[1] += up;
}

extern "C" void throw_pre() { g_throwing = g_hand_ok[phys(0)] && setting("weapons", 1); }
extern "C" void throw_post(char* obj) {
	if (g_throwing && obj) {
		float* v = (float*)(obj + 0xBC), w[3];
		if (g_dyn_src == 2) {
			float k = setting("dyn_gain", 1.2f);
			for (int i = 0; i < 3; i++) w[i] = g_hand_v[phys(0)][i] * k;
		} else {
			arc_velocity(w);
		}
		if (setting("weapon_log", 0))
			logf("dinamite: lançada por %s: impulso do jogo %.0f %.0f %.0f -> %.0f %.0f %.0f", g_dyn_src == 2 ? "grip" : "gatilho", v[0], v[1], v[2],
			     w[0], w[1], w[2]);
		memcpy(v, w, sizeof w);
	}
	// O cozimento (WTC+0x60, quanto do pavio já queimou) não volta a zero sozinho entre arremessos com a soltura imediata
	// do mod: sem zerar aqui, ele acumula e uma dinamite seguinte sai com o pavio no fim e explode na mão.
	if (char* p = player_local())
		if (char* wtc = *(char**)(p + 0x544)) *(float*)(wtc + 0x60) = 0;
	g_throwing = false, g_dyn_src = 0, g_thrown = obj, g_rs_pending = g_rs_hand >= 0;
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	g_throw_qpc = t.QuadPart;
}
extern "C" void throw_thunk();
asm(".globl _throw_thunk\n"
    "_throw_thunk:\n"
    "  call _throw_pre\n"
    "  push 4(%esp)\n"
    "  mov $0x00BFA2E0, %eax\n"
    "  call *%eax\n"
    "  push %eax\n"
    "  call _throw_post\n"
    "  pop %eax\n"
    "  ret $4\n");

// Saída da dinamite (0x00BFAB20, stdcall (WTC*, vec3* out), call site 0x00BFA3A7): 10 cm à frente da mão dela.
typedef float*(__stdcall* spawn_t)(char*, float*);
float* __stdcall hk_spawn(char* wtc, float* out) {
	float* r = ((spawn_t)0x00BFAB20)(wtc, out);
	if (g_throwing) {
		const float* G = g_hand[phys(0)];
		for (int i = 0; i < 3; i++) out[i] = G[i * 4 + 3] - 10 * G[i * 4 + 2];
	}
	return r;
}

// Diagnóstico (só com weapon_log): mudanças de estado do WTC (PlayerDI+0x544) e cada dinamite lançada.
void dyn_trace(char* p) {
	char* wtc = *(char**)(p + 0x544);
	if (!wtc || !setting("weapon_log", 0)) return;
	static int st = -1;
	static char* obj;
	int s = *(int*)(wtc + 0x30);
	if (s != st) logf("dinamite: WTC estado %d -> %d (ação %d, evento %.3f/%.3f s, cozimento %.2f)", st, s, *(int*)(wtc + 0x58),
	                  *(float*)(wtc + 0x44), *(float*)(wtc + 0x50), *(float*)(wtc + 0x60)), st = s;
	// Só o ponteiro da dinamite lançada vai para o log, nunca o conteúdo: depois do arremesso ela pode já ter explodido,
	// e ler o objeto liberado derruba o jogo.
	if (g_thrown != obj) obj = g_thrown, logf("dinamite: dinamite lançada %p (modo do jogador %d)", (void*)obj, *(int*)(p + 0x5A8));
}

void hands_select_test(char* p);
void hold_frame(char* p);
void hands_dump(char* vis, char* p);
void weapon_frame() {
	if (char* p = player_local()) {
		// Relê o PlayerFppVis do jogador a cada frame (PlayerDI+0x1C8 → vf1). O g_vis do último vf110 fica velho quando o
		// jogador renasce (morte, checkpoint): o pré-OnFrame roda antes do vf110 do vis novo e leria o vis já liberado,
		// derrubando o jogo.
		g_vis = player_vis(p);
		dyn_trace(p);
		akimbo_reload_frame(p);
		if (player_head_frame()) hands_select_test(p);
		if (g_vis && setting("hands_dump", 0)) setting_set("hands_dump", 0), hands_dump(g_vis, p);
	}
	static LONGLONG prev;
	LARGE_INTEGER now, fq;
	QueryPerformanceCounter(&now), QueryPerformanceFrequency(&fq);
	float dt = prev ? (float)(now.QuadPart - prev) / fq.QuadPart : 0;
	prev = now.QuadPart;
	// Pose do controle inválida num frame (perda de rastreio): a mão fica na última pose boa. Sem isso o HandHolder não é
	// escrito, a arma volta para o braço encolhido e some (a arma pisca).
	static int lost[2];
	for (int h = 0; h < 2; h++) {
		float trk[12];
		if (hand_tracking(h, trk)) {
			memcpy(g_trk[h], trk, sizeof trk), hand_filter(h, g_trk[h], dt), g_hand_ok[h] = true;
		} else if (g_hand_ok[h] && ++lost[h] % 90 == 1) {
			logf("armas: mão %s sem pose válida (%d frames); fica na última", h ? "E" : "D", lost[h]);
		}
		if (g_hand_ok[h]) player_to_game(g_trk[h], g_hand[h]);
	}
	if (char* p = player_local()) hold_frame(p);  // saque e soltura pelo grip
	aim_update();                                 // pose de cada arma (uma ou duas mãos)
	static float hist[2][8][7];  // (t, mundo xyz, tracking xyz em cm) de cada mão; velocidade numa janela de ~40 ms
	static int nh;
	static LONGLONG t0 = now.QuadPart;  // relativo ao início: em float, segundos desde o boot perdem os ms
	float tnow = (float)(now.QuadPart - t0) / fq.QuadPart;
	int i0 = nh % 8;
	for (int h = 0; h < 2; h++) {
		float(*hh)[7] = hist[h];
		const float *G = g_hand[h], *T = g_trk[h];
		const float now7[7] = {tnow, G[3], G[7], G[11], T[3] * 100, T[7] * 100, T[11] * 100};
		memcpy(hh[i0], now7, sizeof now7);
		for (int k = 1; k < 8 && k <= nh; k++) {
			float* o = hh[(nh - k) % 8];
			if (tnow - o[0] >= 0.04f || k == 7 || k == nh) {
				float d = tnow - o[0];
				if (d > 0)
					for (int c = 0; c < 3; c++) g_hand_v[h][c] = (now7[c + 1] - o[c + 1]) / d, g_trk_v[h][c] = (now7[c + 4] - o[c + 4]) / d;
				break;
			}
		}
	}
	nh++;
	if (float ht = setting("haptic_test", 0)) {  // SET haptic_test <s>: pulso longo nas duas mãos (teste da vibração)
		setting_set("haptic_test", 0);
		for (int h = 0; h < 2; h++) vr_haptic(h, ht, setting("haptic_hz", 160), 1.0f);
	}
	static LONGLONG last;
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	if (!setting("weapon_log", 0) || t.QuadPart - last < 5 * f.QuadPart) return;
	last = t.QuadPart;
	for (int h = 0; h < 2; h++) {
		if (!g_hand_ok[h]) continue;
		float m[12];
		to_cam(player_head_cam(), g_hand[h], m);  // na cabeça: x direita, y cima, z trás (cm)
		logf("armas: mão %s na cabeça pos %.1f %.1f %.1f, frente %.2f %.2f %.2f", h ? "E" : "D", m[3], m[7], m[11], -m[2], -m[6], -m[10]);
	}
}

// Ponto do jogo (cm) para o tracking (m): inversa afim da conversão da cabeça (trk = hmd + R_hmd·R_camᵀ·(P − cam)/k).
void game_to_trk(const float* P, float* t) {
	const float *C = player_head_cam(), *H = player_hmd();
	float v[3] = {P[0] - C[3], P[1] - C[7], P[2] - C[11]}, k = 100 * setting("world_scale", 1), w[3];
	for (int c = 0; c < 3; c++) w[c] = (C[c] * v[0] + C[4 + c] * v[1] + C[8 + c] * v[2]) / k;
	for (int r = 0; r < 3; r++) t[r] = H[r * 4 + 3] + H[r * 4] * w[0] + H[r * 4 + 1] * w[1] + H[r * 4 + 2] * w[2];
}

// --- Teleporte (um dos tipos de movimento nas opções de VR). Com o stick esquerdo empurrado aparece um arco saindo da
// mão não dominante (balística como a da dinamite, tp_arc_speed cm/s pela frente do controle, até bater em algo).
// Soltar o stick com o arco verde teleporta de fato, sem correr até lá no escuro: no pré-OnFrame seguinte o mod chama o
// SetPosition do ator (PlayerDI vf46, slot +0xB8 → vf35, o mesmo que a escada do jogo usa; o pé fica em +0x390) com o
// pé no ponto menos o deslocamento da cabeça no quarto, e a tela pisca preta por tp_blink_ms. O destino só vale com
// chão, altura certa e caminho livre (tp_arc); nesse modo o stick não anda. ---
static bool g_tp_aim, g_tp_valid;
static float g_tp_target[3];
static LONGLONG g_tp_blink, g_tp_go;  // fim da piscada; hora do teleporte (0 = nenhum)
static LONGLONG tp_now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
float teleport_fade() { return g_tp_blink && qpc_ms(tp_now() - g_tp_blink) < 0 ? 1 : 0; }
// Teste (pad.txt "TP x y z"): teleporte direto para um ponto do chão, em coordenadas do mundo do jogo.
void teleport_request(const float pos[3]) { memcpy(g_tp_target, pos, sizeof g_tp_target), g_tp_go = tp_now(); }

// Pré-OnFrame (player.cpp): executa o teleporte pedido no input.
void teleport_frame(char* p) {
	if (!g_tp_go || tp_now() < g_tp_go) return;  // espera a tela ficar preta (o fade do compositor leva 50 ms)
	g_tp_go = 0;
	const float *A = (float*)(p + 0x390), *C = player_head_cam();
	float to[3] = {g_tp_target[0] - (C[3] - A[0]), g_tp_target[1] + 2, g_tp_target[2] - (C[11] - A[2])};
	logf("teleporte: de %.0f %.0f %.0f para %.0f %.0f %.0f", A[0], A[1], A[2], to[0], to[1], to[2]);
	((void(__thiscall*)(char*, float*))(*(void***)p)[0xB8 / 4])(p, to);
}

static void tp_arc(char* p) {
	const float* G = g_hand[phys(1)];
	float sp = setting("tp_arc_speed", 900), v[3], pos[3], pts[16][3], out[0x30];
	for (int i = 0; i < 3; i++) v[i] = -G[i * 4 + 2] * sp, pos[i] = G[i * 4 + 3] - 10 * G[i * 4 + 2];
	int n = 0;
	g_tp_valid = false;
	const float dt = 0.05f;
	for (int k = 0; k < 40 && n < 16; k++) {
		float nxt[3] = {pos[0] + v[0] * dt, pos[1] + v[1] * dt - 490 * dt * dt, pos[2] + v[2] * dt};
		v[1] -= 980 * dt;
		bool hit = raycast(p, pos, nxt, out);  // nxt volta encurtado até o acerto
		memcpy(pos, nxt, sizeof pos);
		if (hit || k % 3 == 2) game_to_trk(pos, pts[n++]);
		if (hit) {
			// destino válido: batida descendo (chão), no máximo tp_step cm acima ou abaixo do pé e sem nada no caminho
			// numa linha a 1 m do chão entre o jogador e o ponto (parede, grade, beirada alta)
			const float *A = player_anchor(), *C = player_head_cam();
			float from[3] = {C[3], A[1] + 100, C[11]}, to[3] = {pos[0], pos[1] + 100, pos[2]}, o2[0x30];
			g_tp_valid = v[1] < 0 && fabsf(pos[1] - A[1]) <= setting("tp_step", 100) && !raycast(p, from, to, o2);
			break;
		}
	}
	if (g_tp_valid) memcpy(g_tp_target, pos, sizeof pos);
	static const float ok[3] = {0.3f, 1, 0.45f}, bad[3] = {1, 0.2f, 0.2f};
	vr_arc(pts[0], n, g_tp_valid ? ok : bad);
}

bool teleport_pad(LONG* cur) {
	char* p = player_local();
	if (!p || !player_head_frame() || setting("move_type", 0) != 1 || duel_active(p) || player_dodge()) {
		if (g_tp_aim) vr_arc(NULL, 0);
		g_tp_aim = false;
		return false;
	}
	float mag = hypotf((float)cur[1], (float)cur[2]) / 32767;
	cur[1] = cur[2] = 0;
	if (mag > 0.6f) g_tp_aim = true;
	if (!g_tp_aim) return true;
	if (mag > 0.25f) {
		if (g_hand_ok[phys(1)]) tp_arc(p);
		return true;
	}
	g_tp_aim = false, vr_arc(NULL, 0);
	if (g_tp_valid) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		LONGLONG now = tp_now();
		g_tp_go = now + f.QuadPart * 60 / 1000, g_tp_blink = now + (LONGLONG)(setting("tp_blink_ms", 180) / 1000 * f.QuadPart);
	}
	return true;
}

void belt_trk(float b[3]);
// Arco da dinamite (gatilho segurado): balística da mão com g = −980 cm/s² (medido), passos de 0,07 s até bater
// (raycast por trecho) ou 2,5 s; os pontos vão como overlays pequenos.
void dyn_arc(char* p) {
	if (g_tp_aim) return;  // o arco do teleporte usa os mesmos pontos
	char* wtc = *(char**)(p + 0x544);
	int st = wtc ? *(int*)(wtc + 0x30) : 0;
	if (!dyn_in_hand(p) || g_dyn_src != 1 || (st != 1 && st != 2) || !g_hand_ok[phys(0)]) {
		// Sem a dinamite na mão (modo com coldres), um ponto laranja no cinto mostra onde pegá-la.
		char* iq = *(char**)(*(char**)(p + 0x4B4) + 0x34);
		if (!dyn_in_hand(p) && setting("hold", 1) && setting("holsters", 0) && setting("belt_show", 1) && *(char**)(iq + 0x24 + 4 * 8)) {
			float b[3];
			belt_trk(b);
			return vr_arc(b, 1);
		}
		return vr_arc(NULL, 0);
	}
	float v[3], pos[3], pts[16][3], out[0x30];
	arc_velocity(v);
	const float* G = g_hand[phys(0)];
	for (int i = 0; i < 3; i++) pos[i] = G[i * 4 + 3] - 10 * G[i * 4 + 2];
	int n = 0;
	const float dt = 0.07f;
	for (int k = 0; k < 36 && n < 16; k++) {
		float nxt[3] = {pos[0] + v[0] * dt, pos[1] + v[1] * dt - 490 * dt * dt, pos[2] + v[2] * dt};
		v[1] -= 980 * dt;
		bool hit = raycast(p, pos, nxt, out);  // nxt volta encurtado até o acerto
		memcpy(pos, nxt, sizeof pos);
		if (hit || k % 2 == 1) game_to_trk(pos, pts[n++]);
		if (hit) break;
	}
	vr_arc(pts[0], n);
}

// --- Pavio aceso na mão. O jogo só liga fumaça e faíscas na dinamite já lançada (Grenade::vf89 @0x00A0F240 cria a
// sequência dynamite_smoke_trail_sequence.fx presa ao objeto, Grenade::vf90 a remove); a dinamite ainda na mão não
// tem efeito. O mod cria a mesma sequência enquanto o WTC está em 1 ou 2 (acendendo ou acesa).
// Prender o efeito ao osso R_HandHolder não funciona (só o som aparece): o FX lê o osso da animação, que está com o
// braço encolhido na clavícula (escala 0,001). Por isso o mod refaz o FXCreateSequence (CFXManagerBind::vf37
// @0x0066FCF0) em partes para ficar com a instância: FUN_004DAE30 (stdcall (mgr, seq)) cria, FUN_004DC1F0 (cdecl (fx,
// pos, pai, osso, 0), ECX = dir) liga ao objeto do jogador e FUN_004DC530 (ESI = fx, ECX = mgr) devolve o id. A cada
// frame ele grava a matriz local do FX (fx+0x40, 3x4; o mundo é pai × local, CFXLinkRoot::vf9) com a ponta do pavio
// no HandHolder que o mod posicionou.
// O pai é o objeto do engine do PlayerDI (*(p+0x14), como a granada usa main+0x14). Não use o modelo do corpo: ele
// não tem nó em +0x88, e o GetWorld com ECX nulo derruba o jogo.
// Remover = CFXManagerBind::vf32(id). mgr = *(bind+4)+0x5C0, bind = CLevel+0x78 (vtable 0x0129FBF4). A sequência vem
// do ITools::vf385 (carrega por nome); a instância guarda a própria referência, então o mod solta a sua logo depois
// com o vf389. Ponta e base do pavio no HandHolder, em cm (x esquerda, y cima, z frente), são lidas a cada frame
// (ajustáveis com SET); os padrões foram medidos em captura no headset.
extern "C" void fx_place(char* fx, float* pos, float* dir, char* parent, int bone);
asm(".globl _fx_place\n"
    "_fx_place:\n"
    "  mov 12(%esp), %ecx\n"
    "  push $0\n"
    "  push 24(%esp)\n"
    "  push 24(%esp)\n"
    "  push 20(%esp)\n"
    "  push 20(%esp)\n"
    "  mov $0x004DC1F0, %eax\n"
    "  call *%eax\n"
    "  add $20, %esp\n"
    "  ret\n");
extern "C" unsigned fx_id(char* mgr, char* fx);
asm(".globl _fx_id\n"
    "_fx_id:\n"
    "  push %esi\n"
    "  mov 8(%esp), %ecx\n"
    "  mov 12(%esp), %esi\n"
    "  mov $0x004DC530, %eax\n"
    "  call *%eax\n"
    "  pop %esi\n"
    "  ret\n");

// A instância ainda é a do id? (tabela do gerente em mgr+0x34, como o FUN_004DC530 confere)
bool fx_alive(char* mgr, unsigned id, char* fx) {
	char* page = *(char**)(mgr + 0x34 + ((id >> 10) & 0x3F) * 4);
	char* e = page ? page + (id & 0x3FF) * 0x20 : NULL;
	return e && *(unsigned short*)e == (unsigned short)(id >> 16) && *(char**)(fx + 0x14) == e + 4;
}

void dyn_fuse_fx(char* p, char* bobj, int bone) {
	char* parent = p ? *(char**)(p + 0x14) : NULL;
	if (parent && !*(char**)(parent + 0x88)) parent = NULL;
	if (parent) {  // conferência: o objeto do jogador fica no pé (âncora); longe disso a suposição está errada
		const float *W = get_world(parent), *A = player_anchor();
		float d = fabsf(W[3] - A[0]) + fabsf(W[7] - A[1]) + fabsf(W[11] - A[2]);
		static bool warned;
		if (d > 500) {
			if (!warned) warned = true, logf("dinamite: pavio desligado: objeto do jogador a %.0f cm da âncora", d);
			parent = NULL;
		}
	}
	static unsigned id;
	static char *bind, *mgr, *fx;
	char *go = p ? *(char**)(p + 0x20) : NULL, *lvl = go ? *(char**)(go + 0x24) : NULL, *cur = lvl ? lvl + 0x78 : NULL;
	char* wtc = dyn_in_hand(p) ? *(char**)(p + 0x544) : NULL;
	int st = wtc ? *(int*)(wtc + 0x30) : 0;  // 1 = acendendo (~1 s de animação), 2 = acesa
	bool lit = (st == 1 || st == 2) && bone >= 0 && parent && setting("dyn_fx", 1);
	if (fx && (cur != bind || !fx_alive(mgr, id, fx))) fx = NULL;  // nível trocado ou o FX acabou sozinho
	if (fx && !lit) {
		((void(__thiscall*)(char*, unsigned))(*(void***)bind)[32])(bind, id);
		fx = NULL;
	}
	if (!lit) return;
	if (!fx) {
		if (!cur || *(uintptr_t*)cur != 0x0129FBF4 || !(mgr = *(char**)(*(char**)(cur + 4) + 0x5C0))) return;
		const char* name = "dynamite_smoke_trail_sequence.fx";  // string do engine {ptr, len, cap}: o vf385 só lê o ptr
		void* tools = *(void**)0x02E64184;
		int seq = ((int(__thiscall*)(void*, const char**))0x00476CB0)(tools, &name);
		if (!seq) return;
		fx = ((char*(__stdcall*)(char*, int))0x004DAE30)(mgr, seq);
		((void(__thiscall*)(void*, int))0x00476D20)(tools, seq);
		if (!fx) return;
		float zero[3] = {0, 0, 0}, up[3] = {0, 1, 0};
		fx_place(fx, zero, up, parent, -1);
		id = fx_id(mgr, fx), bind = cur;
		if (setting("weapon_log", 0)) logf("dinamite: pavio na mão: sequência %#x, efeito %p id %#x", seq, (void*)fx, id);
	}
	// A faísca desce da ponta (dyn_fx_x/y/z) até a base do pavio (dyn_fx_bx/by/bz) com o cozimento (WTC+0x60, s) sobre
	// o pavio (dyn_fuse): mostra quanto falta para estourar. Mundo desejado W = HandHolder (como o mod gravou) com esse
	// ponto; local = pai⁻¹ × W (pai rígido).
	const float *H = bone_slot(bone_node(bobj, bone)), *P = get_world(parent);
	float k = fminf(1, fmaxf(0, *(float*)(wtc + 0x60) / setting("dyn_fuse", 3.0f))), w[3], *L = (float*)(fx + 0x40);
	float o[3] = {setting("dyn_fx_x", 0), setting("dyn_fx_y", 5), setting("dyn_fx_z", 0)};
	float b[3] = {setting("dyn_fx_bx", 0), setting("dyn_fx_by", 1), setting("dyn_fx_bz", 0)};
	for (int i = 0; i < 3; i++) o[i] += (b[i] - o[i]) * k;
	for (int r = 0; r < 3; r++) w[r] = H[r * 4] * o[0] + H[r * 4 + 1] * o[1] + H[r * 4 + 2] * o[2] + H[r * 4 + 3] - P[r * 4 + 3];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) L[i * 4 + j] = P[i] * H[j] + P[4 + i] * H[4 + j] + P[8 + i] * H[8 + j];
		L[i * 4 + 3] = P[i] * w[0] + P[4 + i] * w[1] + P[8 + i] * w[2];
	}
}

// Ponto de mira: raio do cano até onde ele bate (até 100 m); um segundo raio, da cabeça até o ponto, esconde o ponto
// se algo estiver no meio (oclusão de tudo ou nada). O ponto vai para o tracking (game_to_trk) e vira um overlay do
// SteamVR com tamanho angular fixo.
void reticle(char* p, int h) {
	float m[3], d[3], to[3], out[0x30];
	int ph = phys(h);  // o ponto tem a cor da mão física
	if (!setting("crosshair_vr", 1) || !muzzle_of(h, m, d)) return vr_reticle(ph, NULL, 0);
	for (int i = 0; i < 3; i++) to[i] = m[i] + d[i] * 10000;
	if (!raycast(p, m, to, out)) return vr_reticle(ph, NULL, 0);
	const float* C = player_head_cam();
	float v[3] = {to[0] - C[3], to[1] - C[7], to[2] - C[11]}, len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), back[3];
	for (int i = 0; i < 3; i++) back[i] = C[i * 4 + 3] + v[i] * fmaxf(0, len - 5) / len;
	float from[3] = {C[3], C[7], C[11]};
	bool hidden = raycast(p, from, back, out);
	static int last[2] = {-1, -1};
	if (setting("weapon_log", 0) && (int)hidden != last[h])
		last[h] = hidden, logf("armas: ponto de mira mão %d a %.0f cm da cabeça%s", h, len, hidden ? ", escondido (oclusão)" : "");
	if (hidden) return vr_reticle(ph, NULL, 0);
	float t[3];
	game_to_trk(to, t);
	vr_reticle(ph, t, len / (100 * setting("world_scale", 1)) * tanf(setting("crosshair_deg", 1.5f) * 0.01745329f));
}

// --- Mãos visíveis, com a mão do próprio jogo: todos os ossos do braço (da clavícula aos twists, achados pela árvore
// do esqueleto) vão com escala ε para o punho, e o osso Hand vai para a pose em que o HandHolder dele cai no alvo da
// mão (HandHolder é filho de Hand; a relação entre os dois é a do encaixe do jogo, ver g_rel). Dedos e HandHolder
// seguem como filhos. Nó do esqueleto (FUN_004B6010): +0 pai, +4 primeiro filho, +0xC próximo irmão. ---
void inv34(const float* M, float* o) {  // inversa de afim rígida
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) o[r * 4 + c] = M[c * 4 + r];
		o[r * 4 + 3] = -(M[r] * M[3] + M[4 + r] * M[7] + M[8 + r] * M[11]);
	}
}
int side_bone(char* body, int side, const char* n) {
	char name[32];
	snprintf(name, sizeof name, "%s%s", side ? "L_" : "R_", n);
	return body_bone(body, name);
}
// Filhos de um nó, conferidos (o pai do filho tem de ser o nó); n = quantos, -1 se a estrutura não bater.
int children(char* nd, char** out, int max) {
	int n = 0;
	for (char* c = *(char**)(nd + 4); c; c = *(char**)(c + 0xC)) {
		if (n == max || *(char**)c != nd) return -1;
		out[n++] = c;
	}
	return n;
}

// Relação Hand no frame do HandHolder de cada lado: o encaixe do jogo, lido uma vez por PlayerFppVis, antes da
// primeira escrita do mod. O local do HandHolder não é animado e guarda o que o mod escreveu: relida a cada frame, a
// relação mudaria quando o HandHolder vai para a outra mão (arma única na esquerda) e a mão se mexeria sozinha.
float g_rel[2][12];
bool g_rel_ok[2];
char* g_rel_vis;
// Dedos como no HL2VR (FUN_1021E2E0 dele): cada falange interpola (nlerp do quaternion local) entre a pose aberta e a
// fechada pelo curl do dedo dela (0 aberto, 1 fechado). As poses vêm da animação do próprio jogo: fechada = mão
// direita segurando o revólver; aberta = mão esquerda solta no mesmo frame (arma única curta). O lado que falta é o
// espelho: o rig é espelhado com y invertido no frame de cada osso (E = diag(1, −1, 1)·D), então o quaternion
// (w, x, y, z) de um lado vira (w, −x, y, −z) no outro. Ossos: 0 = Hand1 (pai de anelar e mínimo), 1 + 3·dedo + falange.
const char* k_fingers[5][3] = {{"Finger01", "Finger02", "Finger03"}, {"Finger11", "Finger12", "Finger13"},
                               {"Finger21", "Finger22", "Finger23"}, {"Finger31", "Finger32", "Finger33"},
                               {"Finger41", "Finger42", "Finger43"}};
float g_pose[2][2][16][4];  // [lado][0 aberta, 1 fechada][osso] quaternion local
bool g_pose_ok;
inline int finger_parent(int i) { return i == 0 ? -1 : (i - 1) % 3 ? i - 1 : i >= 10 ? 0 : -1; }  // -1 = Hand

// Locais (quaternions) dos 16 ossos a partir dos mundos da animação: Fa[i], Ha = Hand.
void finger_locals(const float* Ha, const float (*Fa)[12], float (*q)[4]) {
	for (int i = 0; i < 16; i++) {
		const float* P = finger_parent(i) < 0 ? Ha : Fa[finger_parent(i)];
		float inv[12], L[12];
		inv34(P, inv), mul34(inv, Fa[i], L), mat_to_quat(L, q[i]);
	}
}

// Põe a mão do lado side no alvo T do HandHolder (direct: T já é o mundo do osso Hand) e esconde o braço. curl = dedos
// pelo controle (NULL: dedos da animação); own = a mão segura a própria arma. freeze = recarga em curso: a animação
// de recarga mexe os dedos da mão que segura a arma, então eles ficam na última pose de antes da recarga (locais
// guardados a cada frame fora dela).
bool hand_place(char* body, char* bobj, int side, const float* T, bool direct = false, const float* curl = NULL, bool own = false,
                bool freeze = false) {
	int ic = side_bone(body, side, "Clavicle"), ih = side_bone(body, side, "Hand"), ihh = side_bone(body, side, "HandHolder");
	if (ic < 0 || ih < 0 || ihh < 0) return false;
	float inv[12], W[12], E[12], Fa[16][12];
	int fi[16];
	const float *Ha = bone_world(bobj, ih), *HHa = bone_world(bobj, ihh);
	bool held = own && !curl;  // mão que segura a própria arma: dedos da animação (congelados na recarga)
	if (curl || held) {  // mundos da animação antes de qualquer escrita deste lado: 0 = Hand1, 1 + 3·dedo + falange
		fi[0] = side_bone(body, side, "Hand1");
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++) fi[1 + f * 3 + j] = side_bone(body, side, k_fingers[f][j]);
		for (int i = 0; i < 16; i++) {
			if (fi[i] < 0) curl = NULL, held = false;
			else memcpy(Fa[i], bone_world(bobj, fi[i]), 48);
		}
	}
	float Hcopy[12];
	memcpy(Hcopy, Ha, sizeof Hcopy);
	Ha = Hcopy;
	if (g_rel_vis != g_vis) g_rel_vis = g_vis, g_rel_ok[0] = g_rel_ok[1] = false;
	if (!g_rel_ok[side] && fabsf(sqrtf(HHa[0] * HHa[0] + HHa[4] * HHa[4] + HHa[8] * HHa[8]) - 1) < 0.05f)
		inv34(HHa, inv), mul34(inv, Ha, g_rel[side]), g_rel_ok[side] = true,
		logf("mãos: relação Hand no HandHolder lado %d (vis %08x): %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f | %.3f %.3f %.3f %.1f",
		     side, *(unsigned*)g_vis, g_rel[side][0], g_rel[side][1], g_rel[side][2], g_rel[side][3], g_rel[side][4],
		     g_rel[side][5], g_rel[side][6], g_rel[side][7], g_rel[side][8], g_rel[side][9], g_rel[side][10], g_rel[side][11]);
	if (!g_rel_ok[side]) return false;
	// O HandHolder esquerdo não é o espelho do direito (Hand -> HandHolder: D (8,0; 1,1; −2,7), E (9,6; 0,0; −2,4)), e
	// com a relação lida a mão esquerda fica girada fora do akimbo. Sem a arma do próprio lado, ela usa o espelho da
	// relação da direita: frame do Holder espelhado em x (x = esquerda) e o da mão em y (o rig é espelhado, ver os dedos):
	// rel_E = diag(−1,1,1)·rel_D·diag(1,−1,1).
	// No corpo de 3ª pessoa do duelo o HandHolder direito fica a 25 cm da mão e girado (relação lida (−11,5; −1,0; −21,8)
	// cm), o que punha a mão ao lado da arma. Lá a direita usa a relação do PlayerFppVis (k_rel_fpp, medida na campanha)
	// e a esquerda o espelho dela.
	static const float k_rel_fpp[12] = {-0.120f, 0.125f, -0.985f, -2.0f, 0.176f, 0.979f, 0.103f, -2.3f, 0.977f, -0.161f, -0.139f, -7.6f};
	bool duel = *(uintptr_t*)g_vis == 0x01342C2C;
	const float* r0 = duel ? k_rel_fpp : g_rel[0];
	float rel[12];
	memcpy(rel, side ? g_rel[1] : r0, sizeof rel);
	// No akimbo, a relação lida do próprio HandHolder também deixa a mão esquerda fora do lugar; akimbo_mirror (padrão 1)
	// usa o espelho da direita também com a arma na mão esquerda.
	if (side == 1 && (!own || duel || setting("akimbo_mirror", 1)) && g_rel_ok[0])
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++) rel[r * 4 + c] = r0[r * 4 + c] * (r == 0 ? -1 : 1) * (c == 1 ? -1 : 1);
	if (direct) memcpy(W, T, sizeof W);
	else mul34(T, rel, W);
	float s = setting("hand_scale", 1), e = s * 0.001f;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++) E[r * 4 + c] = c < 3 ? W[r * 4 + c] * e : W[r * 4 + 3], W[r * 4 + c] *= c < 3 ? s : 1;
	char *hand = bone_node(bobj, ih), *stack[96], *kids[16];
	int n = 0;
	stack[n++] = bone_node(bobj, ic);
	while (n) {  // pré-ordem: o pai é escrito antes dos filhos
		char* nd = stack[--n];
		if (nd == hand) continue;
		int k = children(nd, kids, 16);
		if (k < 0 || n + k > 96) {
			static bool warned;
			if (!warned) warned = true, logf("mãos: árvore do braço não confere (nó %p); mãos desligadas", (void*)nd);
			setting_set("hands", 0);
			return false;
		}
		memcpy(bone_slot(nd), E, sizeof E);
		for (int i = 0; i < k; i++) stack[n++] = kids[i];
	}
	memcpy(bone_slot(hand), W, sizeof W);
	static float held_l[2][16][12];  // locais dos dedos na mão que segura a arma, da última pose fora da recarga
	static bool held_ok[2];
	// O revólver único usa a pega do akimbo (a animação do jogo segura o revólver de um jeito com um e de outro com dois).
	// Os locais dos dedos de cada lado são guardados enquanto o akimbo está na mão e usados no revólver único desse lado;
	// antes do primeiro akimbo da sessão vale a pose abaixo, capturada com o Walker em akimbo. akimbo_grip 0 desliga.
	static float ak_l[2][16][12] = {{{0.9576f, 0.2814f, -0.0621f, 1.8555f, -0.2881f, 0.9351f, -0.2064f, -1.8594f, 0, 0.2155f, 0.9765f, 0.1562f}, {0.6197f, -0.5539f, -0.5560f, 1.8320f, 0.4458f, -0.3347f, 0.8302f, 2.3711f, -0.6459f, -0.7624f, 0.0395f, -0.8438f}, {0.7081f, -0.0973f, 0.6994f, 5.1250f, 0.0119f, 0.9920f, 0.1260f, 0, -0.7061f, -0.0808f, 0.7035f, 0}, {0.9116f, -0.0175f, 0.4106f, 3.5273f, -0.0458f, 0.9886f, 0.1438f, 0, -0.4084f, -0.1499f, 0.9004f, 0}, {0.9228f, 0.2950f, 0.2479f, 10.2695f, -0.2700f, 0.9540f, -0.1304f, 2.7188f, -0.2750f, 0.0533f, 0.9600f, 0.4219f}, {0.6007f, 0.0006f, 0.7995f, 4.6758f, -0.0003f, 1.0000f, -0.0005f, 0, -0.7995f, 0, 0.6007f, 0}, {0.9994f, -0.0105f, 0.0330f, 2.5938f, 0.0084f, 0.9981f, 0.0614f, 0.0020f, -0.0336f, -0.0611f, 0.9976f, 0}, {-0.0321f, 0.2683f, 0.9628f, 10.1797f, 0.0489f, 0.9626f, -0.2666f, 0.3203f, -0.9983f, 0.0385f, -0.0440f, 0.9688f}, {0.6410f, 0.0361f, 0.7667f, 5.1797f, -0.0157f, 0.9993f, -0.0340f, 0.0039f, -0.7674f, 0.0098f, 0.6411f, 0.0039f}, {0.6420f, 0.0583f, 0.7645f, 3.1016f, -0.0281f, 0.9982f, -0.0526f, 0.0039f, -0.7662f, 0.0123f, 0.6425f, 0.0039f}, {-0.2670f, -0.0303f, 0.9632f, 7.6523f, -0.1261f, 0.9920f, -0.0037f, 2.3086f, -0.9554f, -0.1224f, -0.2687f, 0.0469f}, {0.9780f, -0.0013f, 0.2087f, 4.4297f, 0.0010f, 1.0000f, 0.0014f, 0, -0.2087f, -0.0012f, 0.9780f, 0}, {0.3746f, 0.0391f, 0.9263f, 2.6953f, 0.0289f, 0.9981f, -0.0538f, 0, -0.9267f, 0.0469f, 0.3728f, -0.0039f}, {-0.0869f, -0.1811f, 0.9796f, 7.0938f, -0.0254f, 0.9834f, 0.1795f, -0.0010f, -0.9959f, -0.0093f, -0.0900f, 0}, {1.0000f, 0.0044f, 0, 3.6172f, -0.0044f, 0.9999f, -0.0124f, 0, 0, 0.0124f, 0.9999f, 0.0039f}, {0.4237f, -0.0312f, 0.9053f, 1.9844f, 0.0999f, 0.9949f, -0.0125f, -0.0020f, -0.9003f, 0.0957f, 0.4246f, 0.0078f}},
	                                 {{0.9576f, -0.2814f, -0.0621f, 1.8594f, 0.2881f, 0.9351f, 0.2064f, 1.8643f, 0, -0.2155f, 0.9765f, 0.1484f}, {0.6197f, 0.5539f, -0.5560f, 1.8359f, -0.4458f, -0.3347f, -0.8302f, -2.3682f, -0.6459f, 0.7624f, 0.0395f, -0.8438f}, {0.7081f, 0.0973f, 0.6994f, 5.1211f, -0.0119f, 0.9920f, -0.1260f, 0.0078f, -0.7061f, 0.0808f, 0.7035f, 0.0039f}, {0.9116f, 0.0175f, 0.4106f, 3.5273f, 0.0458f, 0.9886f, -0.1438f, 0.0078f, -0.4084f, 0.1499f, 0.9004f, -0.0078f}, {0.9336f, -0.2953f, 0.2027f, 10.2773f, 0.2763f, 0.9539f, 0.1169f, -2.7158f, -0.2279f, -0.0532f, 0.9722f, 0.4141f}, {0.5627f, -0.0006f, 0.8267f, 4.6797f, 0.0003f, 1.0000f, 0.0005f, 0.0020f, -0.8267f, 0, 0.5627f, 0}, {1.0000f, 0.0086f, 0.0015f, 2.5938f, -0.0085f, 0.9981f, -0.0614f, -0.0020f, -0.0020f, 0.0614f, 0.9981f, 0}, {-0.0321f, -0.2683f, 0.9628f, 10.1758f, -0.0489f, 0.9626f, 0.2666f, -0.3145f, -0.9983f, -0.0385f, -0.0440f, 0.9609f}, {0.6410f, -0.0361f, 0.7667f, 5.1797f, 0.0157f, 0.9993f, 0.0340f, 0, -0.7674f, -0.0098f, 0.6411f, 0.0039f}, {0.6716f, -0.0527f, 0.7390f, 3.1094f, 0.0253f, 0.9985f, 0.0482f, 0.0010f, -0.7405f, -0.0137f, 0.6719f, 0}, {-0.2670f, 0.0303f, 0.9632f, 7.6523f, 0.1261f, 0.9920f, 0.0037f, -2.3047f, -0.9554f, 0.1224f, -0.2687f, 0.0547f}, {0.9780f, 0.0013f, 0.2087f, 4.4219f, -0.0010f, 1.0000f, -0.0014f, 0.0020f, -0.2087f, 0.0012f, 0.9780f, -0.0039f}, {0.4107f, -0.0346f, 0.9111f, 2.6875f, -0.0286f, 0.9983f, 0.0509f, 0, -0.9113f, -0.0470f, 0.4090f, 0}, {-0.0869f, 0.1811f, 0.9796f, 7.0977f, 0.0254f, 0.9834f, -0.1795f, 0, -0.9959f, 0.0093f, -0.0900f, 0.0078f}, {1.0000f, -0.0044f, 0, 3.6016f, 0.0044f, 0.9999f, 0.0124f, 0.0039f, 0, -0.0124f, 0.9999f, 0}, {0.4589f, 0.0321f, 0.8879f, 1.9922f, -0.0952f, 0.9954f, 0.0133f, 0, -0.8834f, -0.0906f, 0.4598f, -0.0078f}}};
	static bool ak_ok[2] = {true, true};
	bool ak = g_wtype[0] == 0xF && g_wtype[1] == 0xF;
	bool use_ak = held && !ak && g_wtype[0] == 0xF && ak_ok[side] && setting("akimbo_grip", 1);
	if (held && !freeze && !use_ak) {
		for (int i = 0; i < 16; i++) {
			float inv[12];
			inv34(finger_parent(i) < 0 ? Ha : Fa[finger_parent(i)], inv), mul34(inv, Fa[i], held_l[side][i]);
		}
		held_ok[side] = true;
		if (ak) {
			if (!ak_ok[side]) logf("armas: pega do akimbo guardada (lado %d) para o revólver único", side);
			memcpy(ak_l[side], held_l[side], sizeof ak_l[side]), ak_ok[side] = true;
		}
	} else if (use_ak || (held && held_ok[side])) {
		const float(*src)[12] = use_ak ? ak_l[side] : held_l[side];
		float N[16][12];
		for (int i = 0; i < 16; i++) {
			mul34(finger_parent(i) < 0 ? W : N[finger_parent(i)], src[i], N[i]);
			memcpy(bone_slot(bone_node(bobj, fi[i])), N[i], 48);
		}
		return true;
	}
	if (!curl || !g_pose_ok) return true;
	float N[16][12];  // mundos novos, pais antes dos filhos (a ordem dos índices já garante)
	for (int i = 0; i < 16; i++) {
		int pi = finger_parent(i);
		const float *Pa = pi < 0 ? Ha : Fa[pi], *Pn = pi < 0 ? W : N[pi];
		float c = i == 0 ? (curl[3] + curl[4]) / 2 : curl[(i - 1) / 3], q[4], n = 0, inv[12], L[12];
		c = fminf(1, fmaxf(0, c));
		const float *a = g_pose[side][0][i], *b = g_pose[side][1][i];
		float sg = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.f : 1.f;
		for (int k = 0; k < 4; k++) q[k] = a[k] + (sg * b[k] - a[k]) * c, n += q[k] * q[k];
		n = 1 / sqrtf(n);
		for (int k = 0; k < 4; k++) q[k] *= n;
		inv34(Pa, inv), mul34(inv, Fa[i], L);  // a posição da falange no pai vem da animação
		quat_to_mat(q, L);
		mul34(Pn, L, N[i]);
		memcpy(bone_slot(bone_node(bobj, fi[i])), N[i], 48);
	}
	return true;
}

// Diagnóstico: SET hands_dump 1 registra a árvore do braço de cada lado (índice, profundidade, nome, posição na câmera),
// o InventoryQuick (6 slots: item e tipo), o item de cada mão (PlayerDI+0xA84/+0xA90 = item+0x14) e o HolderL no
// frame do Holder de cada arma.
void hands_dump(char* vis, char* p) {
	static const char* names[] = {"Clavicle", "Upperarm", "Forearm", "Hand", "HandHolder", "Normal", "Normal2", "Foretwist",
	                              "Foretwist1", "ForetwistT", "Finger01", "Finger01extra", "Finger02", "Finger03", "Finger11",
	                              "Finger12", "Finger13", "Finger21", "Finger22", "Finger23", "Hand1", "Finger31", "Finger32",
	                              "Finger33", "Finger41", "Finger42", "Finger43"};
	char *body = vis + 0x20, *bobj = body_obj(body), *base = bone_node(bobj, 0);
	const float* C = player_head_cam();
	for (int side = 0; side < 2; side++) {
		int ic = side_bone(body, side, "Clavicle");
		if (ic < 0) continue;
		char* stack[96];
		int depth[96], n = 0, seen = 0;
		stack[n] = bone_node(bobj, ic), depth[n++] = 0;
		while (n && seen < 96) {
			char* nd = stack[--n];
			int d = depth[n], idx = (int)((nd - base) / 0x18), par = (int)((*(char**)nd - base) / 0x18);
			const char* nm = "?";
			for (const char* x : names)
				if (side_bone(body, side, x) == idx) nm = x;
			const float* W = bone_world(bobj, idx);
			float sx = sqrtf(W[0] * W[0] + W[4] * W[4] + W[8] * W[8]);
			char lbl[40];
			snprintf(lbl, sizeof lbl, "%*s%s%s", d, "", side ? "L_" : "R_", nm);
			logf("mãos: osso %3d pai %3d escala %.3f", idx, par, sx);
			log_mat(lbl, C, W);
			seen++;
			char* kids[16];
			int k = children(nd, kids, 16);
			if (k < 0) {
				logf("mãos: filhos do osso %d não conferem", idx);
				break;
			}
			for (int i = 0; i < k && n < 96; i++) stack[n] = kids[i], depth[n++] = d + 1;
		}
	}
	char* iq = *(char**)(*(char**)(p + 0x4B4) + 0x34);
	char *sel0 = *(char**)(p + 0xA84), *sel1 = *(char**)(p + 0xA90);
	for (int i = 0; i < 6; i++) {
		char* item = *(char**)(iq + 0x24 + i * 8);
		char* sub = item ? item + 0x2C : NULL;
		int type = sub ? ((int(__thiscall*)(char*))(*(void***)sub)[0x44 / 4])(sub) : -1;
		logf("mãos: slot %d item %p tipo %#x%s%s", i, (void*)item, type, item && sel0 == item + 0x14 ? " (mão 0)" : "",
		     item && sel1 == item + 0x14 ? " (mão 1)" : "");
	}
	for (int h = 0; h < 2; h++) {
		char* wv = *(char**)(vis + 0x650 + 4 * h);
		char* model = wv ? *(char**)(wv + 4) : NULL;
		int ih = bone_index(model, "Holder"), il = bone_index(model, "HolderL");
		if (ih < 0 || il < 0) continue;
		float inv[12], L[12];
		inv34(bone_world(wv, ih), inv), mul34(inv, bone_world(wv, il), L);
		logf("mãos: mão %d HolderL no Holder (x esq., y cima, z frente) %.1f %.1f %.1f cm", h, L[3], L[7], L[11]);
	}
}

// Troca de arma do jogo (FUN_009FFE30: ESI = PlayerDI, EDX = slot da mão 0, ECX = slot da mão 1 ou −1). A função tem
// convenção própria (LTCG), então o thunk guarda todos os registradores que a convenção do C manda preservar.
extern "C" void select_slots(char* p, int r, int l);
asm(".globl _select_slots\n"
    "_select_slots:\n"
    "  push %ebp\n"
    "  push %ebx\n"
    "  push %esi\n"
    "  push %edi\n"
    "  mov 20(%esp), %esi\n"
    "  mov 24(%esp), %edx\n"
    "  mov 28(%esp), %ecx\n"
    "  mov $0x009FFE30, %eax\n"
    "  call *%eax\n"
    "  pop %edi\n"
    "  pop %esi\n"
    "  pop %ebx\n"
    "  pop %ebp\n"
    "  ret\n");

// Diagnóstico: SET hands_sel <slot da mão 0> (com hands_sel_l = slot da mão 1, −1 = nenhum) troca de arma e mede quanto a
// arma nova leva para aparecer em cada mão (vis+0x650/+0x654).
void hands_select_test(char* p) {
	static char* wv0[2];
	static LONGLONG t0;
	static int frames = -1;
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	int r = (int)setting("hands_sel", -1);
	if (r >= 0 && g_vis) {
		setting_set("hands_sel", -1);
		int l = (int)setting("hands_sel_l", -1);
		for (int h = 0; h < 2; h++) wv0[h] = *(char**)(g_vis + 0x650 + 4 * h);
		select_slots(p, r, l);
		t0 = t.QuadPart, frames = 0;
		logf("mãos: troca pedida: mão 0 slot %d, mão 1 slot %d (modo %d)", r, l, *(int*)(p + 0x5A8));
		return;
	}
	if (frames < 0 || !g_vis) return;
	frames++;
	for (int h = 0; h < 2; h++) {
		char* now = *(char**)(g_vis + 0x650 + 4 * h);
		if (now != wv0[h])
			logf("mãos: mão %d: WeaponVis %p -> %p em %.0f ms (%d frames)", h, (void*)wv0[h], (void*)now,
			     (t.QuadPart - t0) * 1000.0 / f.QuadPart, frames), wv0[h] = now;
	}
	if (t.QuadPart - t0 > 3 * f.QuadPart) frames = -1;
}


// Pode mostrar a arma da mão lógica k? (segura pela mão e já entregue pelo jogo)
bool shown(char* p, int k) {
	if (!g_vis || !p || !*(char**)(g_vis + 0x650 + 4 * k)) return false;
	if (!setting("hold", 1)) return true;
	if (!g_held[k]) return false;
	if (g_expect[k] && *(char**)(p + 0xA84 + 0xC * k) != g_expect[k]) return false;
	return g_expect[k] = NULL, true;
}

// Esconde o corpo do duelo (player_tpp, em pé, com a cabeça onde fica a câmera). Os braços já foram colapsados no
// punho por hand_place e as mãos ficam; todos os outros ossos vão para um ponto só (a cabeça) com escala ε, escritos
// direto no mundo já calculado (sem marcar os filhos). Com dois pontos de colapso (punho e cabeça), os triângulos
// entre o braço e o tronco ficam colineares (área zero); colapsar cada osso no próprio lugar deixaria "tendas" e fios
// entre as mãos e o tronco. all: esconde também as mãos e os braços (pausa).
void hide_duel_body(char* body, char* bobj, bool all = false) {
	char* skip[4] = {NULL, NULL, NULL, NULL};  // mãos e clavículas (hand_place cuida delas)
	for (int s = 0; s < 2 && !all; s++) {
		if (int i = side_bone(body, s, "Hand"); i >= 0) skip[s] = bone_node(bobj, i);
		if (int i = side_bone(body, s, "Clavicle"); i >= 0) skip[2 + s] = bone_node(bobj, i);
	}
	const float* C = player_head_cam();
	char *base = bone_node(bobj, 0), *root = base, *stack[192], *kids[32];
	while (*(char**)root) root = *(char**)root;
	int n = 0, seen = 0, depth[192];
	bool dump = setting("duel_dump", 0);  // SET duel_dump 1: árvore do corpo do duelo (índice, pai, posição na câmera)
	if (dump) setting_set("duel_dump", 0);
	stack[n] = root, depth[n++] = 0;
	while (n && seen++ < 256) {
		char* nd = stack[--n];
		int dp = depth[n], idx = (int)((nd - base) / 0x18);
		bool own = nd >= base && idx < 256, sk = false;  // a raiz do esqueleto fica fora do vetor de nós: só os filhos dela
		for (char* x : skip) sk |= x && nd == x;
		if (dump && own) {
			float m[12];
			to_cam(C, bone_world(bobj, idx), m);
			logf("duelo:   %*s%d (pai %d) pos %.0f %.0f %.0f escala %.3f%s", dp, "", idx,
			     *(char**)nd ? (int)((*(char**)nd - base) / 0x18) : -1, m[3], m[7], m[11],
			     sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sk ? " (mão)" : "");
		}
		if (sk) continue;
		int k = children(nd, kids, 32);
		if (k < 0 || n + k > 192) return;
		if (own) {
			float* W = bone_world(bobj, idx);
			for (int r = 0; r < 3; r++)  // ε·I, não ε·anim: na pausa o mundo não é recalculado e o ε acumularia
				for (int c = 0; c < 4; c++) W[r * 4 + c] = c == 3 ? C[r * 4 + 3] : r == c ? 0.001f : 0;
		}
		for (int i = 0; i < k; i++) depth[n] = dp + 1, stack[n++] = kids[i];
	}
}

// SET duel_dump 1 (no duelo): HandHolder, arma e ossos dela na câmera, com ou sem o corpo escondido.
void dump_duel_gun(char* body, char* bobj) {
	const float* C = player_head_cam();
	char* wv = *(char**)(g_vis + 0x650);  // arma da mão 0 e o HandHolder dela
	int ih = body_bone(body, "R_HandHolder"), ia = *(int*)(*(char**)(g_vis + 0x3C) + 0x26C);  // osso do anexo (vf311)
	float m[12];
	if (ih >= 0) to_cam(C, bone_world(bobj, ih), m), logf("duelo: R_HandHolder (%d) na câmera %.0f %.0f %.0f", ih, m[3], m[7], m[11]);
	if (ia >= 0) to_cam(C, bone_world(bobj, ia), m), logf("duelo: osso do anexo da mão 0 (%d) na câmera %.0f %.0f %.0f", ia, m[3], m[7], m[11]);
	if (wv && *(char**)(wv + 0x14))
		to_cam(C, get_world(*(char**)(wv + 0x14)), m), logf("duelo: arma da mão 0 na câmera %.0f %.0f %.0f", m[3], m[7], m[11]);
	static const char* const bones[] = {"Holder", "Barrel"};
	for (const char* nm : bones)
		if (int ib = wv ? bone_index(*(char**)(wv + 4), nm) : -1; ib >= 0)
			to_cam(C, bone_world(wv, ib), m), logf("duelo: %s da arma na câmera %.0f %.0f %.0f escala %.3f", nm, m[3], m[7], m[11],
			                                      sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]));
}

void holster_trk(int slot, float m[12]);
// Posição do osso name de um ator (modelo em ator+4), como o DuelMinigameEye::vf26 lê o inimigo.
bool actor_bone_pos(char* actor, const char* name, float o[3]) {
	int i = actor ? bone_index(*(char**)(actor + 4), name) : -1;
	if (i < 0) return false;
	const float* m = bone_world(actor, i);
	return o[0] = m[3], o[1] = m[7], o[2] = m[11], true;
}
// Posição atual do osso idx do corpo do duelo.
bool duel_bone_pos(int idx, float o[3]) {
	char* p = player_local();
	if (!p || !duel_body(p)) return false;
	char* body = player_vis(p) + 0x20;
	const float* m = bone_world(body_obj(body), idx);
	return o[0] = m[3], o[1] = m[7], o[2] = m[11], true;
}

// Hook do PlayerDuelVis::vf110 (slot 0x01342DE4 → 0x009ABE30, thiscall sem args), o update do vis do duelo. Logo
// depois dele, a arma do duelo ainda no coldre já vai para o coldre virtual (weapon_camera repete isso no fim do
// frame). Sem isso, entre a animação e o CameraFPPDI::vf8 a arma fica onde a animação de 3ª pessoa deixa a mão (que se
// mexe no aquecimento), e a caixa de corte dela, calculada nesse momento, tira a arma da imagem em ~1 de cada 10
// frames quando se olha para o coldre (a mão, que é do corpo, continua). duel_early_holster 0 desliga.
typedef void(__thiscall* duel_vf110_t)(char*);
duel_vf110_t o_duel_vf110;
void holster_trk(int slot, float m[12]);
void __fastcall hk_duel_vf110(char* vis, void*) {
	o_duel_vf110(vis);
	char* p = player_local();
	if (!p || !player_head_frame() || !duel_gun_holstered(p) || !setting("duel_early_holster", 1) || !*(char**)(vis + 0x650)) return;
	char* body = vis + 0x20;
	int i = body_bone(body, "R_HandHolder");
	if (i < 0) return;
	float trk[12], G[12];
	holster_trk(dom() ? 2 : 1, trk), player_to_game(trk, G), holder_of(G, bone_slot(bone_node(body_obj(body), i)));
}

// Diagnóstico do revólver piscando no coldre do duelo (SET duel_probe 1): no frame de uma captura SHOT (eye_loop),
// registra onde estão o R_HandHolder, o Holder da arma e o objeto dela, na câmera, e a escala de cada um.
void weapon_probe() {
	char* p = player_local();
	if (!g_vis || !p || !duel_body(p)) return;
	char *body = g_vis + 0x20, *bobj = body_obj(body), *wv = *(char**)(g_vis + 0x650);
	int ih = body_bone(body, "R_HandHolder"), ig = wv ? bone_index(*(char**)(wv + 4), "Holder") : -1;
	char* obj = wv ? *(char**)(wv + 0x14) : NULL;
	const float* C = player_head_cam();
	float a[12] = {}, b[12] = {}, c[12] = {};
	if (ih >= 0) to_cam(C, bone_world(bobj, ih), a);
	if (ig >= 0) to_cam(C, bone_world(wv, ig), b);
	if (obj) to_cam(C, get_world(obj), c);
	auto sc = [](const float* m) { return sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]); };
	logf("duelo, sonda: HandHolder %.1f %.1f %.1f (%.3f) | Holder %.1f %.1f %.1f (%.3f) | arma %.1f %.1f %.1f (%.3f) | saque %d",
	     a[3], a[7], a[11], sc(a), b[3], b[7], b[11], sc(b), c[3], c[7], c[11], sc(c), *(int*)(*(char**)(p + 0x580) + 0x118));
}

// Na tela do fim do duelo (pausa em 3D, estado 8) o jogo segue animando o corpo de 3ª pessoa e o weapon_camera não
// roda (sem cabeça aplicada): o corpo inteiro some, e a arma some junto pelo HandHolder com escala ε.
void weapon_paused() {
	char* p = player_local();
	if (!p || !duel_body(p) || !setting("duel_hide", 1)) return;
	char *body = player_vis(p) + 0x20, *bobj = body_obj(body);
	for (int k = 0; k < 2; k++)
		if (int i = body_bone(body, k ? "L_HandHolder" : "R_HandHolder"); i >= 0) {
			float* m = bone_slot(bone_node(bobj, i));
			for (int r = 0; r < 3; r++)  // ε·I na cabeça, como os outros ossos
				for (int c = 0; c < 4; c++) m[r * 4 + c] = c == 3 ? player_head_cam()[r * 4 + 3] : r == c ? 0.001f : 0;
		}
	hide_duel_body(body, bobj, true);
}
// Mãos e armas, depois do CameraFPPDI::vf8 com a cabeça aplicada. T = alvo do HandHolder de cada mão física sem coice;
// W = o da mão lógica com coice, na mão física dela. A mão que segura a arma vai com o coice; a arma que não está na
// mão vai para o coldre ou encolhe no HandHolder (escala ε). paused: na pausa em 3D o jogo segue animando o corpo e
// os braços voltariam; as mãos ficam na última pose (o tracking só é lido no update) e sem os pontos de mira.
void weapon_camera(bool paused) {
	char* vis = g_vis;
	if (!vis || !setting("weapons", 1)) return;
	char *body = vis + 0x20, *bobj = body_obj(body);
	char* p = player_local();
	// Captura das poses de referência dos dedos (uma vez por sessão, depois de 60 frames seguidos com arma única curta na
	// mão lógica 0 e a 1 vazia: a direita segura o revólver e a esquerda fica solta).
	static int steady;
	if (!g_pose_ok && p) {
		char* iq = *(char**)(*(char**)(p + 0x4B4) + 0x34);
		char *s0 = *(char**)(p + 0xA84), *i1 = *(char**)(iq + 0x24 + 8), *i2 = *(char**)(iq + 0x24 + 16);
		bool single = s0 && !*(char**)(p + 0xA90) && ((i1 && s0 == i1 + 0x14) || (i2 && s0 == i2 + 0x14));
		steady = single ? steady + 1 : 0;
		if (steady >= 60) {
			for (int sd = 0; sd < 2; sd++) {
				float Fa[16][12];
				int ih = side_bone(body, sd, "Hand"), ok = ih >= 0;
				for (int i = 0; ok && i < 16; i++) {
					int b = side_bone(body, sd, i ? k_fingers[(i - 1) / 3][(i - 1) % 3] : "Hand1");
					if (b < 0) ok = 0;
					else memcpy(Fa[i], bone_world(bobj, b), 48);
				}
				if (!ok) break;
				finger_locals(bone_world(bobj, ih), Fa, g_pose[sd][sd ? 0 : 1]);  // direita = fechada, esquerda = aberta
				g_pose_ok = sd == 1;
			}
			for (int i = 0; g_pose_ok && i < 16; i++)
				for (int m = 0; m < 2; m++) {  // o outro lado pelo espelho
					const float* q = g_pose[m ? 0 : 1][m][i];  // m = 0: aberta da esquerda -> direita; m = 1: fechada da direita -> esquerda
					float* o = g_pose[m ? 1 : 0][m][i];
					o[0] = q[0], o[1] = -q[1], o[2] = q[2], o[3] = -q[3];
				}
			if (g_pose_ok) logf("dedos: poses da mão capturadas (fechada da direita, aberta da esquerda, espelhadas)");
		}
	}
	float T[2][12], W[2][12];
	bool on[2] = {false, false};
	for (int h = 0; h < 2; h++)
		if (g_hand_ok[h]) player_to_game(g_trk[h], g_hand[h]), holder_of(g_hand[h], T[h]);  // a pose do frame, âncora final
	aim_update();
	if (p && setting("reload_log", 0)) {  // diagnóstico: armas do vis, estado dos WFC e itens do jogador, quando mudam
		static char last[256];
		char cur[256];
		auto st = [&](int k) { char* w = *(char**)(p + 0x538 + 4 * k); return w ? *(int*)(w + 0x30) : -1; };
		auto it = [&](int i) { return *(void**)(p + 0xA54 + 0xC * i); };
		snprintf(cur, sizeof cur, "vis %p %p | wfc %d %d | itens %p %p %p %p %p %p", *(void**)(vis + 0x650), *(void**)(vis + 0x654), st(0), st(1),
		         it(0), it(1), it(2), it(3), it(4), it(5));
		if (strcmp(cur, last)) strcpy(last, cur), logf("armas: %s", cur);
	}
	bool duel = duel_active(p), gat = gatling_on(p);  // na gatling as mãos ficam vazias, nos controles (o ponto de mira é o dela)
	for (int k = 0; k < 2; k++) {
		if (gat || !g_hand_ok[phys(k)] || !shown(p, k) || (k == 0 && duel_gun_holstered(p))) continue;  // duelo: arma no coldre até o saque
		float K[12], c = kick_f(k);  // coice em torno da pega (frame do Holder): cano para cima e arma para trás
		rot_yp(0, -setting("kick_deg", 12) * c, 0, 0, -setting("kick_cm", 2) * c, K);
		mul34(g_aim[k], K, W[k]), on[k] = true;
	}
	// support_snap 1 leva a mão esquerda de apoio na arma longa da direita ao guarda-mão, com a pose da animação do jogo:
	// L_Hand = R_Hand alvo · R_Hand⁻¹ · L_Hand, da animação do frame. Vem desligado porque assim a mão fica bem acima da
	// mão real; desligado, a mão de apoio fica no próprio controle (a arma já passa por ela), com os dedos do controle.
	float LH[12];
	bool snap = on[0] && phys(0) == 0 && g_long[0] && g_support[1] && g_rel_ok[0] && setting("support_snap", 0);
	if (snap) {
		int ir = side_bone(body, 0, "Hand"), il = side_bone(body, 1, "Hand");
		float inv[12], a[12], b[12];
		snap = ir >= 0 && il >= 0;
		if (snap) inv34(bone_world(bobj, ir), inv), mul34(W[0], g_rel[0], a), mul34(a, inv, b), mul34(b, bone_world(bobj, il), LH);
	}
	// Dedos pelo controle (esqueleto do SteamVR; sem ele, o fallback do HL2VR: polegar e indicador pelo gatilho, os
	// outros pelo grip), menos na mão que segura a arma do próprio lado ou o guarda-mão: ali vale a animação do jogo.
	static float gc[2];
	float curl[2][5];
	for (int h = 0; h < 2; h++) {
		gc[h] += ((vr_grip(h) ? 1.f : 0.1f) - gc[h]) * 0.35f;
		if (!vr_finger_curl(h, curl[h])) {
			float t = g_trig[h];
			curl[h][0] = fmaxf(0.5f, t), curl[h][1] = fmaxf(0.1f, t), curl[h][2] = curl[h][3] = curl[h][4] = gc[h];
		}
		if (float fc = setting("fake_curl", -1); fc >= 0)
			for (int f = 0; f < 5; f++) curl[h][f] = fc;
	}
	// A mão de apoio fica presa no ponto da arma onde agarrou, em vez de deslizar com o controle: no primeiro frame do
	// apoio guarda a pose dela no frame da arma da outra mão e depois a desenha nesse ponto.
	static float sup_rel[2][12];
	static bool sup_ok[2];
	float SH[12];
	int sup_side = -1;
	for (int side = 0; side < 2; side++) {
		int k = phys(0) == 1 - side ? 0 : 1;  // arma da outra mão física
		if (!g_support[side] || !on[k] || phys(k) != 1 - side || !g_hand_ok[side] || (side && snap) || !setting("support_fixed", 1)) {
			sup_ok[side] = false;
			continue;
		}
		if (!sup_ok[side]) {
			float inv[12];
			inv34(W[k], inv), mul34(inv, T[side], sup_rel[side]), sup_ok[side] = true;
		}
		mul34(W[k], sup_rel[side], SH), sup_side = side;
		// Ajuste por arma (wv 2/3): na escopeta a mão de apoio ficava abaixo do guarda-mão (o HolderL dela está 3,2 cm mais
		// baixo que o do rifle: (2,1; 1,5; 38,9) contra (0,1; 4,7; 38,9) cm); sobe pelo "cima" e anda pelo "esquerda" da arma.
		float up = wv(k, 2), left = wv(k, 3);
		for (int r = 0; r < 3; r++) SH[r * 4 + 3] += W[k][r * 4 + 1] * up + W[k][r * 4] * left;
	}
	// Mão principal: a relação mão ↔ HandHolder é a do revólver, e na escopeta a mão ficaria à frente da pega. Os ajustes
	// por arma movem só a mão (não a arma) pelos eixos da arma (wv 1, 7, 8) e a giram em torno da pega (wv 4, 5, 6).
	float HB[2][12];
	for (int k = 0; k < 2; k++) {
		if (!on[k]) continue;
		memcpy(HB[k], W[k], sizeof HB[k]);
		float b = wv(k, 1);
		// A mão esquerda é o espelho da direita: o deslocamento lateral (e o yaw e o roll abaixo) troca de sinal nela.
		float mir = phys(k) == 1 ? -1.f : 1.f, hu = wv(k, 7), hl = wv(k, 8) * mir;
		for (int r = 0; r < 3; r++) HB[k][r * 4 + 3] += -W[k][r * 4 + 2] * b + W[k][r * 4 + 1] * hu + W[k][r * 4] * hl;
		float pt = wv(k, 4), yw = wv(k, 5) * mir, rl = wv(k, 6) * mir * 3.14159265f / 180;
		if (pt || yw || rl) {  // giro da mão em torno da pega (o HandHolder), no frame da arma
			float R[12], Rr[12] = {cosf(rl), -sinf(rl), 0, 0, sinf(rl), cosf(rl), 0, 0, 0, 0, 1, 0}, a[12];
			rot_yp(yw, pt, 0, 0, 0, R), mul34(HB[k], R, a), mul34(a, Rr, HB[k]);
		}
	}
	float GH[12];  // na gatling, a mão na manivela vai para o cabo dela, fechada
	static const float fist[5] = {1, 1, 1, 1, 1};
	for (int side = 0; side < 2; side++) {
		bool crank = gat && gatling_grip(side, T[side], GH);
		const float* t = crank ? GH : on[0] && phys(0) == side ? HB[0] : on[1] && phys(1) == side ? HB[1] : side == sup_side ? SH : T[side];
		bool anim = (on[side] && phys(side) == side) || (side && snap) || !setting("fingers", 1);
		bool own = on[side] && phys(side) == side;
		bool reload = false;  // recarga da arma desta mão (WFC +0x30: 3 começo, 4 em curso; WFC D/E em PlayerDI+0x538/+0x53C)
		for (int k = 0; k < 2 && own && p; k++)
			if (on[k] && phys(k) == side)
				if (char* wfc = *(char**)(p + 0x538 + 4 * k)) reload = (*(int*)(wfc + 0x30) == 3 || *(int*)(wfc + 0x30) == 4) && setting("reload_freeze", 1);
		if (g_hand_ok[side] && setting("hands", 1) &&
		    hand_place(body, bobj, side, side && snap ? LH : t, side && snap, crank ? fist : anim ? NULL : curl[side], own, reload))
			continue;
		int i = body_bone(body, side ? "L_Clavicle" : "R_Clavicle");
		if (i < 0 || !setting("hide_arms", 1)) continue;
		float* m = bone_slot(bone_node(bobj, i));
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) m[r * 4 + c] *= 0.001f;
	}
	for (int k = 0; k < 2; k++) {
		int i = body_bone(body, k ? "L_HandHolder" : "R_HandHolder");
		if (!on[k]) vr_reticle(phys(k), NULL, 0);
		if (!*(char**)(vis + 0x650 + 4 * k) || i < 0) continue;
		float* m = bone_slot(bone_node(bobj, i));
		if (on[k]) {
			memcpy(m, W[k], sizeof W[k]);
		} else {  // no coldre: revólver pendurado no quadril (visível); longa e dinamite encolhem no HandHolder
			char* iq = p ? *(char**)(*(char**)(p + 0x4B4) + 0x34) : NULL;
			char* sl = p ? *(char**)(p + 0xA84 + 0xC * k) : NULL;
			int slot = duel && !k ? (dom() ? 2 : 1) : 0;  // a arma do duelo (fora do inventário) vai para o quadril dominante
			for (int s = 1; s <= 2 && iq && sl && !slot; s++)
				if (char* it = *(char**)(iq + 0x24 + s * 8); it && sl == it + 0x14) slot = s;
			if (slot && setting("hold", 1) && setting("holster_show", 1)) {
				float trk[12], G[12];
				holster_trk(slot, trk), player_to_game(trk, G), holder_of(G, m);
				continue;
			}
			// O local do HandHolder não é animado: o que se escreve nele fica até a próxima escrita, então a pose da mão é
			// regravada antes de encolher.
			if (g_hand_ok[phys(k)]) memcpy(m, T[phys(k)], sizeof T[0]);
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++) m[r * 4 + c] *= 0.001f;
		}
	}
	if (duel && setting("duel_dump", 0) && (dump_duel_gun(body, bobj), !setting("duel_hide", 1))) setting_set("duel_dump", 0);
	if (duel_body(p) && setting("duel_hide", 1)) hide_duel_body(body, bobj);  // também na morte (controlador inativo)
	if (dyn_in_hand(p)) {  // lançar na soltura (sem esperar o evento da animação) e o arco do gatilho
		char* wtc = *(char**)(p + 0x544);
		*(float*)(wtc + 0x44) = 0, *(float*)(wtc + 0x50) = 0;
		// Pavio: com a dinamite acesa e segura (estado 2, o cozimento em WTC+0x60 corre até o ExplodeDelay de 3 s), um
		// tique de vibração na mão a cada 0,5 s, a cada 0,2 s no último segundo.
		static LONGLONG next;
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
		float left = setting("dyn_fuse", 3.0f) - *(float*)(wtc + 0x60);
		if (*(int*)(wtc + 0x30) == 2 && t.QuadPart >= next) {
			vr_haptic(phys(0), 0.03f, 160, left < 1 ? 1.0f : 0.5f);
			next = t.QuadPart + (LONGLONG)((left < 1 ? 0.2f : 0.5f) * f.QuadPart);
		}
	}
	if (p) dyn_arc(p);
	dyn_fuse_fx(p, bobj, body_bone(body, "R_HandHolder"));
	// Os raycasts do ponto de mira só depois de as duas armas estarem postas: com o raio da direita no meio, a consulta
	// de objetos dinâmicos pegaria a arma da esquerda ainda no braço encolhido, e ela piscaria.
	for (int k = 0; k < 2; k++)
		if (on[k] && !paused) reticle(p, k);
		else if (paused) vr_reticle(phys(k), NULL, 0);
}

// Chamado no fim do player_init, só quando os hooks de player.cpp estão instalados.
void weapon_init() {
	options_init();  // opções de VR no menu do jogo
	if (*(uintptr_t*)0x01342564 != 0x00990DD0) return logf("armas: slot do PlayerFppVis::vf110 não confere; armas na mão desligadas");
	o_vf110 = (vf110_t)patch_ptr(0x01342564, (void*)hk_vf110);
	if (*(uintptr_t*)0x01342DE4 == 0x009ABE30) o_duel_vf110 = (duel_vf110_t)patch_ptr(0x01342DE4, (void*)hk_duel_vf110);
	int n0 = patch_call(0x009E6531, 0x0098CAB0, (void*)holster_thunk);
	static const uintptr_t scale_sites[] = {0x00985253, 0x00985346, 0x0098541E, 0x00BF376B};
	for (uintptr_t a : scale_sites) n0 += patch_call(a, 0x00BF4090, (void*)reload_scale);
	logf("armas: recarga paralela do akimbo: %d de 5 chamadas", n0);
	static const uintptr_t shoot_sites[] = {0x00BE8FA5, 0x00BE8FEC, 0x00BEA65D, 0x00BEA82C, 0x00BEA8AF,
	                                        0x00C11A54, 0x00C134FE, 0x00C19DA1, 0x00C1F0C1};
	int n = 0;
	for (uintptr_t a : shoot_sites) n += patch_call(a, 0x00BE9000, (void*)hk_shoot);
	n += patch_call(0x00BE9E57, 0x00BEA2B0, (void*)hk_aimdir);  // só o do spread (dentro do Shoot); o raio de mira a 10 Hz fica
	patch_call(0x00BE9A63, 0x00F4C7F0, (void*)launch_thunk);
	static const uintptr_t reload_sites[] = {0x00BE84DF, 0x00BE9215, 0x00BEA7D2};
	for (uintptr_t a : reload_sites) patch_call(a, 0x00BEE480, (void*)reload_thunk);
	patch_call(0x00BFA24E, 0x00BFA2E0, (void*)throw_thunk);
	patch_call(0x00BFA3A7, 0x00BFAB20, (void*)hk_spawn);
	// Desliga as animações de ócio da mão: FUN_00993A40 (idle depois de 30–45 s parado) e FUN_00993BC0 (IdleTrick,
	// girar o tambor), ambas stdcall(mão) com ESI = PlayerFppVis e ret 4; o mod põe "ret 4" na entrada.
	// idle_anims 1 mantém as animações.
	static const uintptr_t idle_fns[] = {0x00993A40, 0x00993BC0};
	for (uintptr_t a : idle_fns) {
		uint8_t* f = (uint8_t*)a;
		static const uint8_t pro[3] = {0x55, 0x8B, 0xEC};
		if (setting("idle_anims", 0) || memcmp(f, pro, 3)) continue;
		DWORD old;
		VirtualProtect(f, 3, PAGE_EXECUTE_READWRITE, &old);
		f[0] = 0xC2, f[1] = 0x04, f[2] = 0x00;
		VirtualProtect(f, 3, old, &old);
		FlushInstructionCache(GetCurrentProcess(), f, 3);
	}
	static const uint8_t cross[3] = {0x53, 0xE8, 0x2A};  // mira 2D do jogo (0x00B2B490) vira "xor al,al; ret"
	uint8_t* c = (uint8_t*)0x00B2B490;
	if (setting("crosshair", 0) == 0 && !memcmp(c, cross, 3)) {
		DWORD old;
		VirtualProtect(c, 3, PAGE_EXECUTE_READWRITE, &old);
		c[0] = 0x32, c[1] = 0xC0, c[2] = 0xC3;
		VirtualProtect(c, 3, old, &old);
		FlushInstructionCache(GetCurrentProcess(), c, 3);
	}
	logf("armas: hook PlayerFppVis::vf110, Shoot e direção do tiro (%d de 10 sites), mira 2D %s", n, c[0] == 0x32 ? "desligada" : "do jogo");
}

// --- Coldres. Frame do corpo no tracking: origem na cabeça e yaw do corpo = yaw da cabeça com zona morta
// (body_deadzone), para que olhar para o lado não leve o coldre junto. Zonas em cm (x direita, y cima, z frente),
// esferas; as do quadril e do ombro espelham x na esquerda. Pegar = borda do grip com a mão dentro da zona (grip já
// apertado ao entrar não pega); cada aperto vai para o log com a posição, para calibrar no headset.
// ponytail: origem e altura do corpo são as da cabeça neste frame; se a calibração pedir, fazer o corpo seguir a
// cabeça devagar (para que inclinar não arraste o quadril).
float g_body_yaw;  // rad, no tracking (0 = −z, positivo para a direita)
void body_frame() {
	const float* H = player_hmd();
	float fx = H[1] - H[2], fz = H[9] - H[10];  // frente no plano (frente + cima da cabeça: vale olhando para baixo)
	float yaw = atan2f(fx, -fz), d = yaw - g_body_yaw, dz = setting("body_deadzone", 35) * 0.01745329f;
	d = atan2f(sinf(d), cosf(d));
	if (d > dz) g_body_yaw = yaw - dz;
	else if (d < -dz) g_body_yaw = yaw + dz;
}
void hand_body(int h, float o[3]) {
	const float *H = player_hmd(), *T = g_trk[h];
	float d[3] = {T[3] - H[3], T[7] - H[7], T[11] - H[11]}, sy = sinf(g_body_yaw), cy = cosf(g_body_yaw);
	o[0] = (d[0] * cy + d[2] * sy) * 100, o[1] = d[1] * 100, o[2] = (d[0] * sy - d[2] * cy) * 100;
}

// Coldre visível: pose de tracking de "mão" que deixa a arma pendurada no quadril, com a pega no centro da zona, o
// cano para baixo e o topo para a frente (o lado esquerdo do revólver encostado na perna, como no coldre do faroeste).
// A coluna da mão é (direita, frente, cima) do corpo porque holder_of dá (−direita, cima, −trás) = (esquerda, frente,
// baixo) do corpo.
void holster_trk(int slot, float m[12]) {
	const float* H = player_hmd();
	float sy = sinf(g_body_yaw), cy = cosf(g_body_yaw), x = setting("hip_x", 25) * (slot == 2 ? -1 : 1), y = setting("hip_y", -55),
	      z = setting("hip_z", 0) + setting("holster_dz", -4);  // a pega um pouco atrás do centro da zona
	const float R[3] = {cy, 0, sy}, F[3] = {sy, 0, -cy};
	for (int r = 0; r < 3; r++) {
		m[r * 4] = R[r], m[r * 4 + 1] = F[r], m[r * 4 + 2] = r == 1;
		m[r * 4 + 3] = H[r * 4 + 3] + (R[r] * x + (r == 1) * y + F[r] * z) / 100;
	}
}

// Centro do cinto no tracking (m), para o ponto que mostra a dinamite.
void belt_trk(float b[3]) {
	const float* H = player_hmd();
	float sy = sinf(g_body_yaw), cy = cosf(g_body_yaw), x = setting("belt_x", 0), y = setting("belt_y", -32), z = setting("belt_z", 12);
	b[0] = H[3] + (cy * x + sy * z) / 100, b[1] = H[7] + y / 100, b[2] = H[11] + (sy * x - cy * z) / 100;
}

// Slot do coldre onde a mão h está (0 = nenhum): quadril direito 1, esquerdo 2, ombro 3, cinto 4.
int zone_of(int h, float b[3]) {
	static const struct { const char* k; float x, y, z, r; int slot; bool mirror; } zones[] = {
		{"hip", 25, -55, 0, 16, 1, true}, {"back", 20, -10, -5, 20, 3, true}, {"belt", 0, -32, 12, 10, 4, false}};
	hand_body(h, b);
	int best = 0;
	float bd = 1;
	char key[16];
	for (auto& z : zones)
		for (int m = 0; m < (z.mirror ? 2 : 1); m++) {
			float c[4] = {z.x, z.y, z.z, z.r};
			for (int i = 0; i < 4; i++) snprintf(key, sizeof key, "%s_%c", z.k, "xyzr"[i]), c[i] = setting(key, c[i]);
			if (m) c[0] = -c[0];
			float dx = b[0] - c[0], dy = b[1] - c[1], dz = b[2] - c[2], d = sqrtf(dx * dx + dy * dy + dz * dz) / c[3];
			if (d < bd) bd = d, best = z.slot == 1 && m ? 2 : z.slot;
		}
	return best;
}

// Saque pela mão física h no coldre do slot. Seleção: PlayerDI+0xA84/+0xA90 = item+0x14 de cada mão lógica;
// InventoryQuick = *(*(PlayerDI+0x4B4)+0x34), 6 entradas de 8 B em +0x24.
void grab(char* p, int h, int slot) {
	char* iq = *(char**)(*(char**)(p + 0x4B4) + 0x34);
	auto item = [&](int s) { return *(char**)(iq + 0x24 + s * 8); };
	auto sel = [&](int k) { return *(char**)(p + 0xA84 + 0xC * k); };
	auto slot_of = [&](char* sl) { for (int s = 1; s < 6; s++) if (sl && item(s) && sl == item(s) + 0x14) return s; return 0; };
	int o = 1 - h, ko = g_held[0] && phys(0) == o ? 0 : g_held[1] && phys(1) == o ? 1 : -1;  // o que a outra mão segura
	if (slot == 2 && !item(2)) slot = 1;
	if (ko >= 0 && slot_of(sel(ko)) == slot) {  // a outra mão já tem essa: no quadril, a outra curta
		if (slot != 1 && slot != 2) return;
		slot = 3 - slot;
	}
	char* it = item(slot);
	if (!it) return (void)logf("mãos: coldre %d vazio", slot);
	for (int k = 0; k < 2; k++)  // já selecionada numa mão lógica livre: aparece na hora, sem troca
		if (k != ko && sel(k) == it + 0x14) {
			g_p0 = k ? o : h, g_held[k] = true, g_expect[k] = NULL;
			return (void)logf("mãos: mão %s pega o slot %d (já na mão lógica %d)", h ? "E" : "D", slot, k);
		}
	int so = ko >= 0 ? slot_of(sel(ko)) : 0;
	if ((slot == 1 || slot == 2) && (so == 1 || so == 2)) {  // akimbo: a outra mão mantém a dela (o jogo só aceita com o skill de akimbo)
		int kn = 1 - ko;
		select_slots(p, kn ? so : slot, kn ? slot : so);
		if (sel(kn) == it + 0x14) {
			g_p0 = kn ? o : h, g_held[kn] = true, g_expect[kn] = it + 0x14;
			return (void)logf("mãos: mão %s pega o slot %d: akimbo (mão lógica %d)", h ? "E" : "D", slot, kn);
		}
		logf("mãos: akimbo recusado pelo jogo (skill); arma única");
	}
	if (slot == 4 && ko >= 0) g_rs_slot[0] = slot_of(sel(0)), g_rs_slot[1] = slot_of(sel(1)), g_rs_hand = o, g_rs_k = ko, g_rs_pending = false;
	else if (slot == 4) g_rs_hand = -1;
	select_slots(p, slot, -1);
	g_p0 = h, g_held[0] = true, g_held[1] = false, g_expect[0] = it + 0x14;
	logf("mãos: mão %s pega o slot %d: troca (%s)", h ? "E" : "D", slot, sel(0) == it + 0x14 ? "aceita" : "recusada");
}

// --- Menu de armas no Y (como o seletor do HL2VR). Segurar o Y abre os itens em cruz na frente da mão não dominante
// (a esquerda, a do Y, para destros), virados para a cabeça; o item para onde essa mão aponta (eixo do cano da pega)
// fica destacado. Soltar o Y seleciona: a arma vai para a mão dominante, já segura (no modo com coldres, ela sai no
// próximo soltar do grip, como as do coldre). ---
bool g_ybtn;
// Skill de akimbo: o próprio FUN_009EF370 (EDI = PlayerDI, AL; ramo por jogador em PlayerDI+0x654 ou o global
// *0x02E65990), o mesmo que o SetSelectedItem(1, …) confere.
extern "C" int akimbo_ok(char* p);
asm(".globl _akimbo_ok\n"
    "_akimbo_ok:\n"
    "  push %edi\n"
    "  mov 8(%esp), %edi\n"
    "  mov $0x009EF370, %eax\n"
    "  call *%eax\n"
    "  movzbl %al, %eax\n"
    "  pop %edi\n"
    "  ret\n");
// Câmera lenta enquanto o menu está aberto (menu_slowmo, padrão 10%): timeScale do nível, CLevel+0xB78, o mesmo que o
// SlowMoManager da Concentração escreve (CLevelTimer em +0xB58, vtable 0x01288F44). A cabeça e as mãos seguem o
// tempo real (QPC). Volta ao valor de antes ao fechar o menu ou ao sair do modo CENA.
bool g_menu_open;
float* g_menu_ts;
float g_menu_ts0 = 1;
float* level_timescale(char* p) {
	char *go = p ? *(char**)(p + 0x20) : NULL, *lvl = go ? *(char**)(go + 0x24) : NULL;
	return lvl && *(uintptr_t*)(lvl + 0xB58) == 0x01288F44 ? (float*)(lvl + 0xB78) : NULL;
}
// O menu usa a cruz de armas do próprio jogo (HudItemSelect = *(*0x02E65DB0+0x13C), a que o direcional mostra por
// 1,2 s) no lugar das caixas de texto. Com o menu aberto, o mod zera o tempo dela (+0xCC; some em 1,2) e põe o
// destaque (+0xD0: 0 revólver, 1 dinamite, 2 arma longa, 3 akimbo; FUN_00B5B870) no item apontado; o painel é o
// recorte dela na textura do HUD. Ao fechar, o tempo vai a 1,2 (some) e o destaque volta. Se a cruz não ficar visível
// (+0x20 bit 0), o menu volta às caixas. (A roda HudInventoryCircleDI de +0xA4 não existe na campanha: ponteiro nulo.)
char* g_isel;    // cruz do jogo presa pelo menu
int g_isel_hl0;  // destaque de antes (a arma atual)
char* item_select() {
	char *hud = *(char**)0x02E65DB0, *s = hud && setting("menu_native", 1) ? *(char**)(hud + 0x13C) : NULL;
	return s && *(uintptr_t*)s == 0x013BF9A4 ? s : NULL;  // vtable do HudItemSelect
}

void menu_close() {
	if (g_menu_ts) *g_menu_ts = g_menu_ts0, g_menu_ts = NULL;
	if (g_isel) *(float*)(g_isel + 0xCC) = 1.2f, *(int*)(g_isel + 0xD0) = g_isel_hl0, g_isel = NULL;
	if (g_menu_open) g_menu_open = false, vr_menu(0, NULL, NULL, -1);
}
// Munição de uma arma do inventário: tambor (InventoryItem+0x20) e reserva do tipo dela em
// *(*(PlayerDI+0x4B4)+0x28)+0x24+tipo·4 (tipo = vf 0x84 do ItemDescWithContext em item+0x2C). Usada no pulso e no menu.
bool ammo_of(char* p, char* it, int* mag, int* res) {
	char* stock = *(char**)(*(char**)(p + 0x4B4) + 0x28);
	if (!it || !stock) return false;
	char* desc = it + 0x2C;
	int type = ((int(__thiscall*)(char*))(*(void***)desc)[0x84 / 4])(desc);
	*mag = *(int*)(it + 0x20), *res = type >= 0 && type < 8 ? *(int*)(stock + 0x24 + type * 4) : 0;
	return true;
}

void weapon_menu(char* p) {
	bool& open = g_menu_open;
	static int n, hl = -1, slots[6];
	static float pos[6][3], cross[6][2], ctr[3];
	static const char* lbl[6];
	static char txt[6][40];  // nome e munição (tambor / reserva) em duas linhas
	char* iq = *(char**)(*(char**)(p + 0x4B4) + 0x34);
	auto item = [&](int s) { return *(char**)(iq + 0x24 + s * 8); };
	auto type = [&](char* it) { char* sub = it + 0x2C; return ((int(__thiscall*)(char*))(*(void***)sub)[0x44 / 4])(sub); };
	bool y = g_ybtn && setting("weapon_menu", 1);
	int mh = 1 - dom();  // a outra mão aponta o menu
	if (y && !open && g_hand_ok[mh]) {  // abre: itens do inventário e posições fixas no mundo enquanto o Y estiver apertado
		n = 0, hl = -1;
		// Em cruz, como o D-pad do jogo (que só tem estas 4 seleções): direita revólver (174), cima arma longa (176), esquerda
		// dinamite (208), baixo akimbo (197). Só aparece o que existe; o revólver é o slot 1 (ou o 2 se o 1 estiver vazio);
		// akimbo só com os dois revólveres e o skill do jogo (o que o SetSelectedItem(1, …) confere).
		const float dir[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};  // (direita, cima) no plano do menu
		char* rv = item(1) ? item(1) : item(2);
		if (rv) lbl[n] = "Revólver", slots[n] = item(1) ? 1 : 2, memcpy(cross[n++], dir[0], 8);
		if (char* it = item(3)) {
			int t = type(it);
			lbl[n] = t == 0x14 ? "Escopeta" : t == 0x11 ? "Rifle" : "Arma longa", slots[n] = 3, memcpy(cross[n++], dir[1], 8);
		}
		if (item(4)) lbl[n] = "Dinamite", slots[n] = 4, memcpy(cross[n++], dir[2], 8);
		if (item(1) && item(2) && akimbo_ok(p)) lbl[n] = "Akimbo", slots[n] = -1, memcpy(cross[n++], dir[3], 8);
		for (int i = 0; i < n; i++) {
			int m0, r0, m1, r1;
			if (slots[i] < 0 && ammo_of(p, item(1), &m0, &r0) && ammo_of(p, item(2), &m1, &r1)) snprintf(txt[i], 40, "%s\n%d + %d / %d", lbl[i], m0, m1, r0);
			else if (slots[i] > 0 && slots[i] < 4 && ammo_of(p, item(slots[i]), &m0, &r0)) snprintf(txt[i], 40, "%s\n%d / %d", lbl[i], m0, r0);
			else snprintf(txt[i], 40, "%s", lbl[i]);
			lbl[i] = txt[i];
		}
		const float *T = g_trk[mh];
		float sy = sinf(g_body_yaw), cy = cosf(g_body_yaw), R[3] = {cy, 0, sy}, F[3] = {sy, 0, -cy}, gap = setting("menu_gap", 0.13f);
		for (int c = 0; c < 3; c++) ctr[c] = T[c * 4 + 3] + F[c] * setting("menu_dist", 0.30f) + (c == 1) * 0.06f;
		for (int i = 0; i < n; i++)
			for (int c = 0; c < 3; c++) pos[i][c] = ctr[c] + (c == 1) * cross[i][1] * gap + R[c] * cross[i][0] * gap;
		open = n > 0;
		if (open && (g_menu_ts = level_timescale(p))) g_menu_ts0 = *g_menu_ts;
		if (open && (g_isel = item_select())) g_isel_hl0 = *(int*)(g_isel + 0xD0), logf("HUD: cruz do jogo %p, destaque %d", (void*)g_isel, g_isel_hl0);
		if (open) logf("mãos: menu de armas aberto (%d itens; slots 1/2 %p %p, skill de akimbo %d)", n, (void*)item(1), (void*)item(2), akimbo_ok(p));
	}
	if (!open) return;
	if (y) {  // o item mais perto do eixo de apontar, até 20°
		const float* T = g_trk[mh];
		float f[3] = {-T[2], -T[6], -T[10]}, best = cosf(setting("menu_deg", 20) * 0.01745329f);
		int h = -1;
		for (int i = 0; i < n; i++) {
			float v[3] = {pos[i][0] - T[3], pos[i][1] - T[7], pos[i][2] - T[11]}, l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6f;
			float c = (v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / l;
			if (c > best) best = c, h = i;
		}
		if (h != hl && h >= 0) vr_haptic(mh, 0.02f, 160, 0.4f);
		hl = h;
		if (g_menu_ts) *g_menu_ts = setting("menu_slowmo", 0.1f);  // a cada frame: a Concentração também escreve aqui
		if (g_isel) {
			*(float*)(g_isel + 0xCC) = 0;  // continua aparecendo
			int sl = hl < 0 ? -1 : slots[hl], ix = sl == 1 || sl == 2 ? 0 : sl == 4 ? 1 : sl == 3 ? 2 : sl < 0 && hl >= 0 ? 3 : g_isel_hl0;
			*(int*)(g_isel + 0xD0) = ix;
			static int vis = -1;
			if ((g_isel[0x20] & 1) != vis) vis = g_isel[0x20] & 1, logf("HUD: cruz do jogo visível %d", vis);
		}
		if (g_isel && (g_isel[0x20] & 1)) {
			// Recorte da cruz na unidade de design 1280×720 da faixa da UI (medido numa captura: revólver em (719, 516), arma
			// longa em (640, 435), dinamite em (562, 516); o akimbo cai embaixo, ~(640, 597)). O painel tem o tamanho em que os
			// ícones caem nas posições de apontar (menu_gap entre o centro e cada ícone).
			// ponytail: o retângulo da cruz é fixo; se o layout mudar (outra resolução, modo Arcade), ler a posição do elemento.
			static const float D[4] = {476, 368, 804, 645};
			int b[6];
			vr_hud_band(b);
			if (b[0] > 0 && b[4] > 0) {
				float uv[4] = {(b[2] + D[0] * b[0] / 1280) / b[4], (b[3] + D[1] * b[1] / 720) / b[5],
				               (b[2] + D[2] * b[0] / 1280) / b[4], (b[3] + D[3] * b[1] / 720) / b[5]};
				float gap = setting("menu_gap", 0.13f), u = gap / 79, c[3] = {ctr[0], ctr[1] + (516 - (D[1] + D[3]) / 2) * u, ctr[2]};
				vr_menu(0, NULL, NULL, -1);
				return vr_wheel(c, (D[2] - D[0]) * u, uv);
			}
		}
		return vr_menu(n, lbl, pos[0], hl);
	}
	menu_close();
	if (hl < 0) return (void)logf("mãos: menu fechado sem escolha");
	int s = slots[hl];
	auto sel = [&](int k) { return *(char**)(p + 0xA84 + 0xC * k); };
	if (s < 0) {  // akimbo: revólver 1 na direita (lógica 0) e 2 na esquerda (lógica 1), se o skill deixar
		select_slots(p, 1, 2);
		if (sel(1) == item(2) + 0x14) {
			g_p0 = dom(), g_held[0] = g_held[1] = true, g_expect[0] = item(1) + 0x14, g_expect[1] = item(2) + 0x14;
			tut_action(197);  // a dica "change weapon" do tutorial espera o D-pad (AKIMBO1)
			return (void)logf("mãos: menu -> akimbo");
		}
		logf("mãos: menu -> akimbo recusado (skill); revólver 1");
		s = 1;
	}
	if (s == 4) {  // dinamite: depois do arremesso voltam as armas de antes (hold_frame)
		auto slot_of = [&](char* sl) { for (int k = 1; k < 4; k++) if (sl && item(k) && sl == item(k) + 0x14) return k; return 0; };
		g_rs_slot[0] = slot_of(sel(0)), g_rs_slot[1] = slot_of(sel(1)), g_rs_hand = dom(), g_rs_k = 1, g_rs_pending = false;
		if (!g_rs_slot[0]) g_rs_hand = -1;
	}
	select_slots(p, s, -1);
	g_p0 = dom(), g_held[0] = true, g_held[1] = false, g_expect[0] = item(s) + 0x14, g_support[0] = g_support[1] = false;
	tut_action(s == 3 ? 176 : s == 4 ? 208 : 174);  // o D-pad que a dica do tutorial espera (SLOT3, SLOT5, SLOT1)
	logf("mãos: menu -> slot %d na mão %s", s, dom() ? "esquerda" : "direita");
}

// --- Usar pela mão. O jogo escolhe o alvo pelo olhar (FUN_00A01D30: raio de 400 cm e cone a partir do olho e da
// direção dos vf119/vf120, que já são a cabeça) e o guarda no handle PlayerDI+0x9D0, que aponta para
// TriggerObjectDI+0x2C; o vf 0x15C do objeto dá o centro da caixa dele, como no seletor. A borda do grip de uma mão
// estendida na direção do alvo aperta o "usar" (ação 20, EXECUTE_TRIGGER) pelo OnInput, como o E do teclado, e
// soltar o grip solta a ação. O X do pad continua valendo (segurar = usar, toque = recarga). ---
int g_use_h = -1;  // mão física com o "usar" apertado
char* use_target(char* p) {
	char* hd = *(char**)(p + 0x9D0);
	return hd ? hd - 0x2C : NULL;
}
void use_input(char* p, float v) {
	if (v > 0 && ladder_busy()) return;  // na escada automática o "usar" soltaria a escada
	((void(__thiscall*)(char*, int, float, bool))0x009D60D0)(p + 0x488, 20, v, false);
}
void use_center(char* t, float T[3]) { ((void(__thiscall*)(char*, float*))(*(void***)t)[0x15C / 4])(t, T); }
// Ângulo cabeça→mão × cabeça→alvo até use_deg, com a mão a use_reach_cm da cabeça (ou a 80% do alvo, se ele está perto).
bool use_reach(int h, char* t, bool log) {
	float T[3];
	use_center(t, T);
	const float *E = player_head_cam(), *G = g_hand[h];
	float a[3] = {G[3] - E[3], G[7] - E[7], G[11] - E[11]}, b[3] = {T[0] - E[3], T[1] - E[7], T[2] - E[11]};
	float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
	float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb + 1e-3f), deg = acosf(fmaxf(-1, fminf(1, c))) * 57.29578f;
	bool ok = deg <= setting("use_deg", 25) && la >= fminf(setting("use_reach_cm", 35), 0.8f * lb);
	if (log)  // classe pelo RTTI do MSVC: vtable[-1] = COL, +0xC = TypeDescriptor, nome em +8
		logf("usar: %s %s: mão %s a %.0f° e %.0f cm da cabeça, alvo a %.0f cm (%.0f %.0f %.0f)", ok ? "usar" : "fora do alcance",
		     *(char**)((*(char***)t)[-1] + 0xC) + 8, h ? "E" : "D", deg, la, lb, T[0], T[1], T[2]);
	return ok;
}

// Na escolha do alvo do "usar" (o vf120 chamado de dentro de FUN_00A01AF0..FUN_00A01D30, hook em player.cpp), a
// direção do olhar vira cabeça → mão livre mais estendida, e o alvo segue a mão que vai ao objeto; só pelo olhar,
// muitos apertos do grip chegariam sem alvo do jogo. Estendida = a use_reach_cm da cabeça no plano e no máximo
// use_down_deg abaixo do horizonte (sentado, a mão no colo fica ~60 cm abaixo da cabeça e não conta). Sem mão
// estendida, ou com ela segurando arma, fica o olhar. use_hand_ray 0 desliga.
bool weapon_use_dir(float out[3]) {
	char* p = player_local();
	if (!p || !g_vis || !setting("use_hand_ray", 1)) return false;
	const float* E = player_head_cam();
	float best = setting("use_reach_cm", 35), down = tanf(setting("use_down_deg", 50) * 0.01745329f), v[3];
	bool ok = false;
	for (int h = 0; h < 2; h++) {
		bool armed = (shown(p, 0) && phys(0) == h) || (shown(p, 1) && phys(1) == h);
		if (!g_hand_ok[h] || armed) continue;
		const float* G = g_hand[h];
		float a[3] = {G[3] - E[3], G[7] - E[7], G[11] - E[11]}, hz = sqrtf(a[0] * a[0] + a[2] * a[2]);
		if (hz < best || -a[1] > hz * down) continue;
		float l = sqrtf(hz * hz + a[1] * a[1]);
		best = hz, ok = true, v[0] = a[0] / l, v[1] = a[1] / l, v[2] = a[2] / l;
	}
	if (ok) memcpy(out, v, sizeof v);
	return ok;
}

// --- Munição no pulso (e não na arma). Por mão física, "tambor / reserva" da arma que ela segura (ammo_of). Segue a
// configuração do jogo: só aparece se a dificuldade mostra o contador (DifficultyParams+5 = CanShowAmmoCounter,
// entrada devolvida por FUN_00ED11C0). ---
void wrist_ammo(char* p) {
	char* dp = ((char*(__cdecl*)())0x00ED11C0)();
	bool on = setting("ammo_wrist", 1) && dp[5];
	for (int h = 0; h < 2; h++) {
		int k = shown(p, 0) && phys(0) == h ? 0 : shown(p, 1) && phys(1) == h ? 1 : -1, mag, res;
		char* sel = k >= 0 ? *(char**)(p + 0xA84 + 0xC * k) : NULL;
		if (!on || !sel || (dyn_in_hand(p) && h == phys(0)) || !ammo_of(p, sel - 0x14, &mag, &res)) {
			vr_wrist(h, NULL);
			continue;
		}
		char txt[32];
		snprintf(txt, sizeof txt, "%d / %d", mag, res);
		static char last[2][32];
		if (strcmp(txt, last[h])) logf("HUD: pulso %s: %s", h ? "E" : "D", txt), snprintf(last[h], 32, "%s", txt);
		vr_wrist(h, txt);
	}
}

// Por frame (pré-OnFrame, thread do update): bordas do grip -> saque ou soltura.
void hold_frame(char* p) {
	static bool was[2];
	static DWORD use_wait[2];  // prazo (GetTickCount) do aperto à espera de um alvo, 0 = nenhum
	if (!setting("hold", 1) || !player_head_frame()) {
		g_p0 = dom();
		return menu_close();
	}
	if (gatling_on(p)) return menu_close(), vr_reticle(2, NULL, 0);  // na gatling os grips são dela (gatling.cpp)
	body_frame();
	weapon_menu(p);
	wrist_ammo(p);
	// Marcador no alvo do "usar": anel branco no centro do alvo, verde com uma mão ao alcance.
	static char* t_prev;
	if (char* t = use_target(p); t != t_prev) {  // diagnóstico: cada troca de alvo do jogo vai para o log
		float T[3] = {0, 0, 0};
		if (t) use_center(t, T);
		const float* E = player_head_cam();
		logf("usar: alvo do jogo %s a %.0f cm", t ? *(char**)((*(char***)t)[-1] + 0xC) + 8 : "nenhum",
		     t ? sqrtf((T[0] - E[3]) * (T[0] - E[3]) + (T[1] - E[7]) * (T[1] - E[7]) + (T[2] - E[11]) * (T[2] - E[11])) : 0.f);
		t_prev = t;
	}
	if (char* t = use_target(p); t && setting("use_mark", 1)) {
		static const float white[3] = {1, 1, 1}, green[3] = {0.3f, 1, 0.3f};
		float T[3], k[3];
		use_center(t, T), game_to_trk(T, k);
		const float* H = player_hmd();
		float d = sqrtf((k[0] - H[3]) * (k[0] - H[3]) + (k[1] - H[7]) * (k[1] - H[7]) + (k[2] - H[11]) * (k[2] - H[11]));
		bool ready = g_use_h >= 0 || (g_hand_ok[0] && use_reach(0, t, false)) || (g_hand_ok[1] && use_reach(1, t, false));
		vr_reticle(2, k, d * setting("use_mark_size", 0.06f), ready ? green : white);
	}
	// Modo padrão, só com o menu (holsters 1 volta aos coldres): as armas ficam nas mãos, a dominante é a mão lógica 0 e
	// a outra a 1; o grip só apoia (duas mãos) e arremessa a dinamite.
	bool hol = setting("holsters", 0);
	if (!hol) g_p0 = dom(), g_held[0] = g_held[1] = true;
	char* iq = *(char**)(*(char**)(p + 0x4B4) + 0x34);
	char* wtc = *(char**)(p + 0x544);
	if (g_rs_pending && (!wtc || *(int*)(wtc + 0x30) == 0)) {  // arremesso da dinamite terminado
		g_rs_pending = false;
		int h = g_rs_hand, s0 = g_rs_slot[0], s1 = g_rs_slot[1];
		g_rs_hand = -1;
		if ((!hol || vr_grip(h)) && s0 > 0) {  // só o menu: volta sempre, como o LB do jogo
			select_slots(p, s0, s1 > 0 ? s1 : -1);
			int k = s1 > 0 ? g_rs_k : 0;  // em akimbo a outra mão volta à mão lógica que tinha
			char* it = *(char**)(iq + 0x24 + (k ? s1 : s0) * 8);
			g_p0 = k ? 1 - h : h, g_held[0] = g_held[1] = false, g_held[k] = true, g_expect[k] = it ? it + 0x14 : NULL;
			logf("mãos: depois da dinamite, mão %s volta com o slot %d", h ? "E" : "D", k ? s1 : s0);
		}
	}
	for (int k = 0; k < 2; k++) {  // arma longa, tipo, modelo e HolderL de cada mão lógica
		char *it3 = *(char**)(iq + 0x24 + 3 * 8), *wv = g_vis ? *(char**)(g_vis + 0x650 + 4 * k) : NULL;
		g_long[k] = it3 && *(char**)(p + 0xA84 + 0xC * k) == it3 + 0x14;
		char* sel = *(char**)(p + 0xA84 + 0xC * k);
		g_wtype[k] = sel ? ((int(__thiscall*)(char*))(*(void***)(sel - 0x14 + 0x2C))[0x44 / 4])(sel - 0x14 + 0x2C) : -1;
		// modelo: nome do ItemDescFirearm (*(item+0x2C+4)+8, "Firearm_Sixshooter"), sem o prefixo, em minúsculas e até o
		// 1º "_" (as variantes _ULC, _Ghost e _Arcade usam o ajuste do modelo)
		char nm[24] = "";
		if (g_wtype[k] == 0xF || g_wtype[k] == 0x11 || g_wtype[k] == 0x14) {
			const char* n = *(const char**)(*(char**)(sel - 0x14 + 0x2C + 4) + 8);
			if (n && !_strnicmp(n, "Firearm_", 8)) n += 8;
			for (int i = 0; n && n[i] && n[i] != '_' && i < 23; i++) nm[i] = (char)tolower((unsigned char)n[i]), nm[i + 1] = 0;
		}
		if (strcmp(nm, g_wname[k])) strcpy(g_wname[k], nm), logf("armas: mão lógica %d com o modelo '%s'", k, nm);
		char* model = wv ? *(char**)(wv + 4) : NULL;
		int ih = bone_index(model, "Holder"), il = bone_index(model, "HolderL");
		if (ih < 0 || il < 0) continue;
		float inv[12], L[12];
		inv34(bone_world(wv, ih), inv), mul34(inv, bone_world(wv, il), L);
		g_holderl[k][0] = L[3], g_holderl[k][1] = L[7], g_holderl[k][2] = L[11];
	}
	static int inzone[2];
	for (int h = 0; h < 2; h++) {
		bool g = vr_grip(h) && g_hand_ok[h];
		int o = 1 - h, ko = shown(p, 0) && phys(0) == o ? 0 : shown(p, 1) && phys(1) == o ? 1 : -1;  // o que a outra segura
		bool busy = (shown(p, 0) && phys(0) == h) || (shown(p, 1) && phys(1) == h);
		// Mão vazia entrando num coldre com arma: um toque de vibração (útil sobretudo nas costas, fora da vista).
		float zb[3];
		int z = hol && !busy && !g_support[h] && g_hand_ok[h] ? zone_of(h, zb) : 0;
		if (z && !*(char**)(iq + 0x24 + (z == 2 && !*(char**)(iq + 0x24 + 16) ? 1 : z) * 8)) z = 0;
		if (z && z != inzone[h]) vr_haptic(h, setting("zone_haptic_ms", 40) / 1000, 160, setting("zone_haptic_amp", 0.6f));
		inzone[h] = z;
		if (g_support[h] && (ko < 0 || !shown(p, ko))) g_support[h] = false;  // a outra soltou a arma
		if (g && !was[h]) {
			float b[3];
			int slot = zone_of(h, b);
			// Apoio: perto do HolderL (longa) ou da pega (revólver) da arma que a outra mão segura.
			float dist = 1e9f;
			if (!busy && ko >= 0 && shown(p, ko)) {
				char* wv = *(char**)(g_vis + 0x650 + 4 * ko);
				int ib = bone_index(*(char**)(wv + 4), g_long[ko] ? "HolderL" : "Holder");
				if (ib >= 0) {
					const float *B = bone_world(wv, ib), *G = g_hand[h];
					dist = sqrtf((B[3] - G[3]) * (B[3] - G[3]) + (B[7] - G[7]) * (B[7] - G[7]) + (B[11] - G[11]) * (B[11] - G[11]));
				}
			}
			logf("mãos: grip %s no corpo %.0f %.0f %.0f cm, coldre %d, arma da outra a %.0f cm%s", h ? "E" : "D", b[0], b[1], b[2], slot,
			     dist < 1e8f ? dist : -1.f, busy ? " (mão ocupada)" : "");
			char* t = use_target(p);
			if (dist < setting("support_cm", 15)) g_support[h] = true, logf("mãos: mão %s apoia a arma da mão lógica %d", h ? "E" : "D", ko);
			else if (hol && slot && !busy) grab(p, h, slot);
			else if (t && g_use_h < 0 && !(dyn_in_hand(p) && h == phys(0)) && use_reach(h, t, true)) {  // usar pela mão
				use_input(p, 1), g_use_h = h;
				logf("usar: o jogo %s no aperto", p[0x990] & 1 ? "executou" : "não executou (fica para a soltura)");
			} else if (g_use_h < 0 && !(dyn_in_hand(p) && h == phys(0))) {
				// O jogo troca o alvo a cada 0,1 s; o aperto espera use_wait_ms por um alvo ao alcance da mão.
				use_wait[h] = GetTickCount() + (DWORD)setting("use_wait_ms", 300);
				if (!t) logf("usar: grip %s sem alvo do jogo (esperando %.0f ms)", h ? "E" : "D", setting("use_wait_ms", 300));
			}
		} else if (g && use_wait[h] && g_use_h < 0) {
			char* t = use_target(p);
			if ((int)(GetTickCount() - use_wait[h]) > 0) use_wait[h] = 0, logf("usar: o aperto da mão %s não achou alvo a tempo", h ? "E" : "D");
			else if (t && use_reach(h, t, true)) use_input(p, 1), g_use_h = h, use_wait[h] = 0, logf("usar: alvo chegou depois do aperto");
		} else if (!g && was[h]) {
			use_wait[h] = 0;
			if (g_use_h == h) use_input(p, 0), g_use_h = -1;
			if (g_support[h]) g_support[h] = false, logf("mãos: mão %s solta o apoio", h ? "E" : "D");
			for (int k = 0; k < 2 && hol; k++)
				if (g_held[k] && phys(k) == h) g_held[k] = false, logf("mãos: mão %s solta a mão lógica %d", h ? "E" : "D", k);
		}
		was[h] = g;
	}
}

// No hook do XInput (cur: [0] botões, [5] LT, [6] RT). O RT vem do gatilho da mão física da arma 0 e o LT do da arma
// 1, só se ela está na mão. Com a dinamite na mão, o grip segura o arremesso (o mod aperta o RT) e o gatilho marca o
// arco; até 3 s depois de um arremesso, o clique do stick direito vira LB, o especial do jogo (dividir a dinamite no
// ar, ação 171). Um golpe rápido de qualquer mão vira corpo a corpo.
void weapon_pad(LONG* cur) {
	char* p = player_local();
	if (!p || !player_head_frame()) return;
	g_vis = player_vis(p);  // o input pode vir antes do pré-OnFrame logo depois de o jogador renascer
	LARGE_INTEGER t, f;
	QueryPerformanceCounter(&t), QueryPerformanceFrequency(&f);
	if ((cur[0] & XINPUT_GAMEPAD_RIGHT_THUMB) && g_thrown && t.QuadPart - g_throw_qpc < 3 * f.QuadPart)
		cur[0] = (cur[0] & ~XINPUT_GAMEPAD_RIGHT_THUMB) | XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (setting("hold", 1)) {
		g_ybtn = cur[0] & XINPUT_GAMEPAD_Y;
		cur[0] &= ~XINPUT_GAMEPAD_Y;  // o Y não vai ao jogo: segurar abre o menu de armas do mod
		LONG tr[2] = {cur[6], cur[5]};  // gatilho físico: 0 = direito, 1 = esquerdo
		g_trig[0] = tr[0] / 255.f, g_trig[1] = tr[1] / 255.f;
		cur[6] = shown(p, 0) && !duel_gun_holstered(p) ? tr[phys(0)] : 0;  // no duelo, o gatilho só vale com a arma sacada
		cur[5] = shown(p, 1) ? tr[phys(1)] : 0;
	}
	bool dyn = dyn_in_hand(p);

	// Golpe: mão a mais de melee_speed (cm/s) se afastando da cabeça dispara o corpo a corpo do jogo (ele escolhe o alvo
	// no cone à frente, como no botão). Não vale para a mão com a dinamite nem logo depois de um arremesso pelo braço.
	static LONGLONG melee_until, melee_next;
	if (setting("melee_swing", 1) && !setting("fake_hmd", 0) && !duel_active(p) && !gatling_on(p) && t.QuadPart >= melee_next &&
	    t.QuadPart - g_throw_qpc > f.QuadPart / 2)  // no duelo o saque é um movimento rápido da mão; na gatling, a manivela
		for (int h = 0; h < 2; h++) {
			if (!g_hand_ok[h] || (h == phys(0) && dyn)) continue;
			const float *v = g_trk_v[h], *H = player_hmd(), *T = g_trk[h];  // tracking: andar não conta como golpe
			float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			float away = (T[3] - H[3]) * v[0] + (T[7] - H[7]) * v[1] + (T[11] - H[11]) * v[2];
			// > 25 m/s é salto da pose (perda de rastreio, mão sintética movida por SET), não golpe
			if (s < setting("melee_speed", 300) || s > 2500 || away <= 0) continue;
			logf("mãos: golpe da mão %s a %.0f cm/s: corpo a corpo", h ? "E" : "D", s);
			melee_until = t.QuadPart + f.QuadPart / 10;
			melee_next = t.QuadPart + (LONGLONG)(setting("melee_cd", 0.6f) * f.QuadPart);
			break;
		}
	if (t.QuadPart < melee_until) cur[0] |= XINPUT_GAMEPAD_RIGHT_THUMB;

	if (!dyn) return;
	// Dinamite: o grip segura e cozinha (o mod aperta o RT); o gatilho passa para o arco e soltá-lo lança por ele;
	// soltar o grip lança pelo braço. O arremesso do jogo nasce na borda do botão: com o grip apertado desde o cinto, o
	// RT só desce quando o WTC fica ativo (+0x20, o que o FUN_00BFA990 exige), senão a borda cairia durante a troca.
	// Só a borda de soltar o grip lança pelo braço: com a dinamite vinda do menu (sem grip), o gatilho viraria braço e
	// o arco sumiria.
	static bool gprev;
	bool grip = vr_grip(phys(0)), released = gprev && !grip;
	gprev = grip;
	if (released && g_dyn_src) g_dyn_src = 2;
	else if (cur[6] > 128 && g_dyn_src != 2) g_dyn_src = 1;
	if (g_dyn_src != 1 && grip && *(char*)(*(char**)(p + 0x544) + 0x20)) g_dyn_src = 2, cur[6] = 255;
}
