// Converte a pose do HMD (espaço do tracking do OpenVR) na câmera do jogo. As matrizes são 3x4 row-major, como as do
// OpenVR e as da câmera do jogo, com colunas direita, cima, trás e posição. Tracking: metros, Y para cima, -Z = frente.
// Jogo: centímetros; ψ (yaw) em graus, crescendo ao virar à direita; frente(ψ) = (cos ψ, 0, sin ψ),
// direita(ψ) = (-sin ψ, 0, cos ψ). A calibração (PoseState) guarda ψ0, o giro do tracking em relação ao jogo, e o centro
// do tracking no chão (cx, cz).
#include "mod.h"
#include <math.h>
#include <string.h>

namespace {
constexpr float kDeg = 57.29578f, kRad = 1 / 57.29578f;

PoseState g_ps = {0, 0, 0, 170, false};

// B(ψ0): tracking -> jogo. Colunas: direita(ψ0), cima, trás(ψ0).
void basis(float psi0, float B[9]) {
	float s = sinf(psi0 * kRad), c = cosf(psi0 * kRad);
	const float b[9] = {-s, 0, -c, 0, 1, 0, c, 0, -s};
	memcpy(B, b, sizeof b);
}

// out = B·R, com R e out 3x4 row-major. Só escreve a parte 3x3 de out; a coluna de posição fica por conta de quem chama.
void mul3(const float* B, const float* R34, float* out34) {
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) out34[r * 4 + c] = B[r * 3 + 0] * R34[0 * 4 + c] + B[r * 3 + 1] * R34[1 * 4 + c] + B[r * 3 + 2] * R34[2 * 4 + c];
}
}  // namespace

PoseState& pose_state() { return g_ps; }

// Câmera do jogo (3x4) a partir da pose do HMD no tracking e da âncora (posição dos pés do jogador no jogo, em cm).
void pose_to_game(const float hmd[12], const float anchor[3], float cam[12]) {
	float B[9];
	basis(g_ps.psi0, B);
	mul3(B, hmd, cam);
	float d[3] = {hmd[3] - g_ps.cx, hmd[7], hmd[11] - g_ps.cz}, k = 100 * setting("world_scale", 1);
	// Sentado: altura do olho do personagem em pé (1,64 m) mais quanto a altura da cabeça mudou desde o recentrar.
	if (setting("seated", 0)) d[1] += 1.64f - g_ps.height / 100;
	for (int r = 0; r < 3; r++) cam[r * 4 + 3] = anchor[r] + k * (B[r * 3] * d[0] + B[r * 3 + 1] * d[1] + B[r * 3 + 2] * d[2]);
}

// Yaw ψ e pitch θ, em graus, tirados do vetor frente (−coluna 2). Não use o vetor direita: ele inclina com o roll e
// desvia o yaw (com 15° de roll e 20° de pitch o erro chega a 5°). A frente só degenera olhando a prumo; aí o yaw sai do
// vetor cima, que aponta para trás (θ > 0) ou para a frente (θ < 0).
void game_angles(const float cam[12], float& psi, float& theta) {
	float fx = -cam[2], fz = -cam[10], fy = -cam[6];
	if (fx * fx + fz * fz < 1e-6f) fx = fy > 0 ? -cam[1] : cam[1], fz = fy > 0 ? -cam[9] : cam[9];
	psi = atan2f(fz, fx) * kDeg;
	theta = asinf(fy > 1 ? 1 : fy < -1 ? -1 : fy) * kDeg;
	if (theta > 89) theta = 89;
	if (theta < -89) theta = -89;
}

// Recentrar: o centro passa a ser a posição atual da cabeça projetada no chão, a altura atual vira a referência e ψ0 é
// escolhido para que o yaw da cabeça coincida com o do jogador (psi_player).
void pose_recenter(const float hmd[12], float psi_player) {
	g_ps.psi0 = 0;
	float cam[12], zero[3] = {0, 0, 0}, psi, theta;
	pose_to_game(hmd, zero, cam);
	game_angles(cam, psi, theta);
	g_ps.psi0 = psi_player - psi;
	g_ps.cx = hmd[3], g_ps.cz = hmd[11];
	g_ps.height = hmd[7] * 100;
	g_ps.calibrated = true;
	logf("jogador: recentrar: ψ0 %.1f, centro %.3f %.3f m, altura %.1f cm", g_ps.psi0, g_ps.cx, g_ps.cz, g_ps.height);
}

// Giro de delta graus com pivô na cabeça (a câmera não sai do lugar, só a direção muda):
// c' = h − B(ψ0+Δ)ᵀ·B(ψ0)·(h − c), no plano do chão.
void pose_turn(const float hmd[12], float delta) {
	float B0[9], B1[9];
	basis(g_ps.psi0, B0);
	basis(g_ps.psi0 + delta, B1);
	float hc[3] = {hmd[3] - g_ps.cx, 0, hmd[11] - g_ps.cz}, w[3], v[3];
	for (int r = 0; r < 3; r++) w[r] = B0[r * 3] * hc[0] + B0[r * 3 + 1] * hc[1] + B0[r * 3 + 2] * hc[2];
	for (int c = 0; c < 3; c++) v[c] = B1[0 * 3 + c] * w[0] + B1[1 * 3 + c] * w[1] + B1[2 * 3 + c] * w[2];  // B1ᵀ·w
	g_ps.cx = hmd[3] - v[0], g_ps.cz = hmd[11] - v[2];
	g_ps.psi0 = fmodf(g_ps.psi0 + delta + 540, 360) - 180;
}

// Pose sintética do HMD, para testar sem headset, montada das configurações: fake_yaw (graus, + = direita), fake_pitch
// (+ = cima), fake_roll, fake_x (cm, + = direita), fake_y (cm acima de fake_height), fake_z (cm, + = trás), tudo no
// espaço do tracking.
void fake_hmd_pose(float m[12]) {
	float y = -setting("fake_yaw", 0) * kRad, p = setting("fake_pitch", 0) * kRad, r = setting("fake_roll", 0) * kRad;
	float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
	// R = Ry(y)·Rx(p)·Rz(r)
	float R[9] = {cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp, cp * sr, cp * cr, -sp,
	              -sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) m[i * 4 + j] = R[i * 3 + j];
	m[3] = setting("fake_x", 0) / 100, m[7] = (setting("fake_height", 170) + setting("fake_y", 0)) / 100,
	m[11] = setting("fake_z", 0) / 100;
}

// Auto-teste das conversões acima: devolve o número de falhas e registra cada uma no log. Restaura a calibração no fim,
// mas deixa as configurações fake_* zeradas.
int pose_selftest() {
	PoseState saved = g_ps;
	int fails = 0;
	auto check = [&](bool ok, const char* what, float a, float b) {
		if (!ok) fails++, logf("jogador: AUTO-TESTE FALHOU: %s (%g × %g)", what, a, b);
	};
	auto near_deg = [](float a, float b) { float d = fmodf(a - b + 540, 360) - 180; return fabsf(d) < 0.05f; };
	float anchor[3] = {1000, 2000, 3000}, cam[12], psi, theta, hmd[12];
	// yaw: virar 30° à direita com ψ0 = 0 dá ψ = 30
	setting_set("fake_yaw", 30), setting_set("fake_pitch", 0), setting_set("fake_roll", 0);
	setting_set("fake_x", 0), setting_set("fake_y", 0), setting_set("fake_z", 0);
	g_ps = {0, 0, 0, 170, true};
	fake_hmd_pose(hmd);
	pose_to_game(hmd, anchor, cam);
	game_angles(cam, psi, theta);
	check(near_deg(psi, 30), "ψ com a cabeça 30° à direita", psi, 30);
	// R_cam ortonormal e destra (direita × cima = trás)
	float* c0 = cam; float* c1 = cam + 1; float* c2 = cam + 2;
	float cx = c0[4] * c1[8] - c0[8] * c1[4], cy = c0[8] * c1[0] - c0[0] * c1[8], cz = c0[0] * c1[4] - c0[4] * c1[0];
	check(fabsf(cx - c2[0]) + fabsf(cy - c2[4]) + fabsf(cz - c2[8]) < 1e-4f, "direita × cima = trás", cx, c2[0]);
	// recentrar com o jogador em ψ = 40: a cabeça vira 40 e a câmera fica acima da âncora
	pose_recenter(hmd, 40);
	pose_to_game(hmd, anchor, cam);
	game_angles(cam, psi, theta);
	check(near_deg(psi, 40), "ψ depois de recentrar", psi, 40);
	check(fabsf(cam[3] - 1000) + fabsf(cam[11] - 3000) < 1e-2f && fabsf(cam[7] - 2170) < 1e-2f, "posição depois de recentrar",
	      cam[7], 2170);
	// giro com pivô: cabeça 50 cm à frente do centro; girar 45° não move a câmera e soma 45 ao ψ
	setting_set("fake_z", -50);
	fake_hmd_pose(hmd);
	pose_to_game(hmd, anchor, cam);
	float before[3] = {cam[3], cam[7], cam[11]};
	game_angles(cam, psi, theta);
	float psi_before = psi;
	pose_turn(hmd, 45);
	pose_to_game(hmd, anchor, cam);
	game_angles(cam, psi, theta);
	check(fabsf(cam[3] - before[0]) + fabsf(cam[7] - before[1]) + fabsf(cam[11] - before[2]) < 1e-2f, "câmera parada no giro",
	      cam[3], before[0]);
	check(near_deg(psi, psi_before + 45), "ψ + 45 no giro", psi, psi_before + 45);
	// olhando quase para cima, o yaw continua estável
	setting_set("fake_z", 0), setting_set("fake_pitch", 89);
	fake_hmd_pose(hmd);
	pose_to_game(hmd, anchor, cam);
	game_angles(cam, psi, theta);
	check(near_deg(psi, g_ps.psi0 + 30) && theta > 88.9f, "yaw estável com θ = 89", psi, g_ps.psi0 + 30);
	// roll não mexe no yaw nem no pitch; a prumo, o yaw vem do vetor cima
	setting_set("fake_pitch", 20), setting_set("fake_roll", 15);
	fake_hmd_pose(hmd);
	pose_to_game(hmd, anchor, cam);
	game_angles(cam, psi, theta);
	check(near_deg(psi, g_ps.psi0 + 30) && fabsf(theta - 20) < 0.05f, "roll não altera ψ e θ", psi, g_ps.psi0 + 30);
	setting_set("fake_pitch", 90), setting_set("fake_roll", 0);
	fake_hmd_pose(hmd);
	pose_to_game(hmd, anchor, cam);
	game_angles(cam, psi, theta);
	check(near_deg(psi, g_ps.psi0 + 30), "yaw olhando a prumo", psi, g_ps.psi0 + 30);
	setting_set("fake_pitch", 0), setting_set("fake_yaw", 0);
	g_ps = saved;
	logf("jogador: auto-teste das conversões: %d falha(s)", fails);
	return fails;
}
