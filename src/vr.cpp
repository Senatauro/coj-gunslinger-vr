// Integração com o OpenVR: olhos no HMD, a "tela" virtual (overlay com o backbuffer do jogo), HUD, overlays auxiliares
// e controles. As texturas chegam ao SteamVR pela interop oficial do DXVK, sem fork. O OpenVR é usado pela API C
// (FnTable, __stdcall) porque os métodos C++ que devolvem struct têm ABI diferente entre g++ e MSVC.
#include "mod.h"
#include "dxvk_interop.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#define bool openvr_c_bool  // openvr_capi.h é C e faz "typedef char bool"; o tamanho é o mesmo do bool do C++
#include "openvr_capi.h"
#undef bool
#pragma GCC diagnostic pop

bool g_vr;
int g_eye_w, g_eye_h;
volatile LONG g_vr_recenter = 1;
static VR_IVRSystem_FnTable* g_sys;
static VR_IVRCompositor_FnTable* g_comp;
static VR_IVRInput_FnTable* g_in;  // SteamVR Input (controles)
static VR_IVROverlay_FnTable* g_ovl;
static VROverlayHandle_t g_screen;

// Estado da submissão ao HMD (ver "Submissão ao HMD" mais abaixo). WGP = IVRCompositor::WaitGetPoses.
static CRITICAL_SECTION g_q;  // trava do mod em volta do Lock/Release da fila do DXVK (thread principal e de loading)
static bool g_explicit, g_cena, g_quit;
static bool g_handoff_pending, g_handoff_since_submit, g_wgp_since_submit;
static unsigned g_wgp_pc = ~0u;  // contador de Present do último WGP bem-sucedido
static int g_no_eyes;            // frames seguidos sem olhos em CENA
static int g_wgp_fail;           // WGPs seguidos com erro
static LONGLONG g_backoff_until; // sem WGP até aqui (QPC)
static LONGLONG g_last_wgp;
static LONG g_n_wgp, g_n_wgp_lock, g_n_submit, g_n_submit_err, g_n_handoff, g_n_already, g_last_err, g_n_cena_tela;
static TrackedDevicePose_t g_rpose[16], g_gpose[16];  // poses do último WGP: render e "game" (predita um frame além)
static unsigned g_pose_pc = ~0u;               // contador de Present desse WGP
static VROverlayHandle_t g_aim[4];             // miras: [0] direita, [1] esquerda, [2] alvo do "usar", [3] cabeça
static bool g_aim_shown[4];
static unsigned g_aim_pc[4];
static LONGLONG g_buzz_until[2];               // vibração por pulso legado em curso: fim (QPC) e força, por mão
static float g_buzz_amp[2];
static VROverlayHandle_t g_arc[16];            // pontos do arco (dinamite e teleporte)
static int g_arc_n;
static unsigned g_arc_pc;
static VROverlayHandle_t g_menu[6];            // menu de armas (botão Y)
static char g_menu_txt[6][40];                 // rótulo + destaque já desenhados em cada overlay
static VROverlayHandle_t g_wrist[2];            // munição no pulso
static char g_wrist_txt[2][32];
static unsigned g_wrist_pc[2];
static int g_menu_n;
static unsigned g_menu_pc;
static float g_hz = 90, g_v2p;                 // frequência do HMD e vsync -> fótons, para predizer a pose sem WGP
static inline LONGLONG now_qpc() {
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}
static inline unsigned present_count() { return *(volatile unsigned*)game::present_counter; }


static void* fntable(intptr_t(__cdecl* get)(const char*, EVRInitError*), const char* version) {
	char name[96];
	snprintf(name, sizeof name, "FnTable:%s", version);
	EVRInitError e = EVRInitError_VRInitError_None;
	void* p = (void*)get(name, &e);
	if (!p) logf("OpenVR: %s indisponível (erro %d)", name, e);
	return p;
}

// --- Controles: o SteamVR Input vira um pad XInput virtual. Uma ação por controle do pad; o que cada uma faz vem do
// layout do próprio jogo (inputs_pad.scr). Manifesto e bindings padrão (oculus_touch) ficam em cojvr/steamvr/. ---
static const struct {
	const char* name;
	WORD bit;
} k_vr_buttons[] = {{"a", XINPUT_GAMEPAD_A},           {"b", XINPUT_GAMEPAD_B},
                    {"x", XINPUT_GAMEPAD_X},           {"y", XINPUT_GAMEPAD_Y},
                    {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
                    {"ls", XINPUT_GAMEPAD_LEFT_THUMB},   {"rs", XINPUT_GAMEPAD_RIGHT_THUMB},
                    {"start", XINPUT_GAMEPAD_START},     {"back", XINPUT_GAMEPAD_BACK},
                    {"dpad_up", XINPUT_GAMEPAD_DPAD_UP}, {"dpad_down", XINPUT_GAMEPAD_DPAD_DOWN},
                    {"dpad_left", XINPUT_GAMEPAD_DPAD_LEFT}, {"dpad_right", XINPUT_GAMEPAD_DPAD_RIGHT}};
static const int kNB = sizeof k_vr_buttons / sizeof *k_vr_buttons;
static VRActionSetHandle_t g_set;
static VRActionHandle_t g_btn[kNB], g_move, g_look, g_lt, g_rt, g_haptic[2], g_grip[2];
static bool g_grip_on[2];  // grip apertado em cada mão (0 = direita)
static VRActionHandle_t g_skel[2];  // esqueleto da mão, para o curl dos dedos (0 = direita)
static float g_curl[2][5];
static bool g_curl_ok[2];

static void input_init(VR_IVRInput_FnTable* in) {
	if (!in) return logf("controles: IVRInput indisponível");
	char path[MAX_PATH], name[64];
	snprintf(path, MAX_PATH, "%ssteamvr\\actions.json", g_moddir);
	EVRInputError e = in->SetActionManifestPath(path);
	if (!e) e = in->GetActionSetHandle((char*)"/actions/main", &g_set);
	for (int i = 0; !e && i < kNB; i++)
		snprintf(name, sizeof name, "/actions/main/in/%s", k_vr_buttons[i].name), e = in->GetActionHandle(name, &g_btn[i]);
	VRActionHandle_t* an[] = {&g_move, &g_look, &g_lt, &g_rt};
	const char* nn[] = {"move", "look", "lt", "rt"};
	for (int i = 0; !e && i < 4; i++) snprintf(name, sizeof name, "/actions/main/in/%s", nn[i]), e = in->GetActionHandle(name, an[i]);
	for (int i = 0; !e && i < 2; i++) e = in->GetActionHandle((char*)(i ? "/actions/main/out/haptic_left" : "/actions/main/out/haptic_right"), &g_haptic[i]);
	for (int i = 0; !e && i < 2; i++) e = in->GetActionHandle((char*)(i ? "/actions/main/in/grip_l" : "/actions/main/in/grip_r"), &g_grip[i]);
	for (int i = 0; !e && i < 2; i++)  // esqueleto opcional: sem ele os dedos seguem gatilho e grip, e o resto continua
		if (EVRInputError se = in->GetActionHandle((char*)(i ? "/actions/main/in/skeleton_left" : "/actions/main/in/skeleton_right"), &g_skel[i]))
			logf("dedos: ação do esqueleto %d -> %d", i, se), g_skel[i] = 0;
	logf("controles: SteamVR Input: manifesto %s -> %d", path, e);
	if (!e) g_in = in;
}

void hud_init();
// Chamado antes de carregar o DXVK: no Windows o OpenVR tem de ser iniciado antes do device Vulkan.
void vr_init() {
	if (!cfg_int("vr", "enable", 1)) return;
	char path[MAX_PATH];
	snprintf(path, MAX_PATH, "%sopenvr_api.dll", g_moddir);
	HMODULE m = LoadLibraryA(path);
	if (!m) return logf("OpenVR: %s não carregou (%lu); jogo segue sem VR", path, GetLastError());
	auto init = (intptr_t(__cdecl*)(EVRInitError*, EVRApplicationType, const char*))(void*)GetProcAddress(m, "VR_InitInternal2");
	auto get = (intptr_t(__cdecl*)(const char*, EVRInitError*))(void*)GetProcAddress(m, "VR_GetGenericInterface");
	auto desc = (const char*(__cdecl*)(EVRInitError))(void*)GetProcAddress(m, "VR_GetVRInitErrorAsEnglishDescription");
	EVRApplicationType type = cfg_int("vr", "scene", 1) ? EVRApplicationType_VRApplication_Scene
	                                                    : EVRApplicationType_VRApplication_Overlay;
	EVRInitError e = EVRInitError_VRInitError_None;
	init(&e, type, NULL);
	if (e != EVRInitError_VRInitError_None) {
		if (setting("fake_hmd", 0)) hud_init();  // teste sem headset: a textura do HUD também, para o SHOT e a sonda
		return logf("OpenVR: VR_Init falhou: %d %s (o SteamVR estava aberto antes do jogo?)", e, desc(e));
	}
	g_sys = (VR_IVRSystem_FnTable*)fntable(get, IVRSystem_Version);
	g_comp = (VR_IVRCompositor_FnTable*)fntable(get, IVRCompositor_Version);
	g_ovl = (VR_IVROverlay_FnTable*)fntable(get, IVROverlay_Version);
	if (!g_sys || !g_comp || !g_ovl) return;
	uint32_t w = 0, h = 0;
	g_sys->GetRecommendedRenderTargetSize(&w, &h);
	EVROverlayError oe = g_ovl->CreateOverlay((char*)"cojvr.screen", (char*)"CoJ VR", &g_screen);
	if (oe) return logf("OpenVR: CreateOverlay falhou (%d)", oe);
	g_ovl->SetOverlayWidthInMeters(g_screen, cfg_float("vr", "screen_width", 3.0f));
	g_ovl->SetOverlayFlag(g_screen, VROverlayFlags_IgnoreTextureAlpha, 1);  // o 3D do jogo tem alfa 0
	g_ovl->ShowOverlay(g_screen);
	g_comp->FadeToColor(0, 0, 0, 0, setting("cinema", 1), false);  // começa na TELA (menus), com o ambiente escurecido
	logf("OpenVR iniciado (%s); olho recomendado %ux%u", type == EVRApplicationType_VRApplication_Scene ? "cena" : "overlay",
	     w, h);
	g_eye_w = w, g_eye_h = h;
	InitializeCriticalSection(&g_q);
	g_explicit = cfg_int("stereo", "timing", 1) != 0;
	g_comp->SetExplicitTimingMode(g_explicit ? EVRCompositorTimingMode_VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff
	                                         : EVRCompositorTimingMode_VRCompositorTimingMode_Implicit);
	ETrackedPropertyError pe;
	float hz = g_sys->GetFloatTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &pe);
	g_hz = hz > 1 ? hz : 90;
	g_v2p = g_sys->GetFloatTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_SecondsFromVsyncToPhotons_Float, &pe);
	logf("OpenVR: timing %s, frequência do HMD %.1f Hz, vsync -> fótons %.1f ms", g_explicit ? "explícito" : "implícito", hz,
	     g_v2p * 1000);
	input_init((VR_IVRInput_FnTable*)fntable(get, IVRInput_Version));
	g_vr = true;
	hud_init();  // hooks do HUD no headset
}

// Transformação olho -> cabeça (3x4 row-major, em metros); false sem VR.
bool vr_eye_to_head(int eye, float m[12]) {
	if (!g_vr) return false;
	HmdMatrix34_t t = g_sys->GetEyeToHeadTransform(eye ? EVREye_Eye_Right : EVREye_Eye_Left);
	memcpy(m, t.m, 48);
	return true;
}

// Pose de um dispositivo (0 = cabeça; controles < 16). Pose de update (lógica do jogo): a "game" do WGP do frame
// anterior, já predita um frame além. Pose de render: a do WGP deste frame. Sem pose válida do WGP, pede ao SteamVR a
// pose predita para os fótons do próximo frame. Cabeça e mãos usam a mesma predição, então uma não "nada" em relação
// à outra.
static bool device_pose(unsigned idx, bool update, float m[12]) {
	if (!g_vr || idx >= 16) return false;
	unsigned pc = present_count();
	const TrackedDevicePose_t* p = NULL;
	if (update && g_pose_pc == pc - 1 && g_gpose[idx].bPoseIsValid) p = &g_gpose[idx];
	if (!update && g_pose_pc == pc && g_rpose[idx].bPoseIsValid) p = &g_rpose[idx];
	TrackedDevicePose_t now[16];
	if (!p) {
		float since = 0;
		uint64_t frame;
		g_sys->GetTimeSinceLastVsync(&since, &frame);
		float ahead = 1 / g_hz - since + g_v2p;
		g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding, ahead > 0 ? ahead : 0, now, idx + 1);
		if (!now[idx].bPoseIsValid) return false;
		p = &now[idx];
	}
	memcpy(m, p->mDeviceToAbsoluteTracking.m, 48);
	return true;
}
bool vr_hmd_pose(bool update, float m[12]) { return device_pose(0, update, m); }
// Qual dispositivo é cada mão. O vrlink do Quest também cria dispositivos de rastreio de mão (VRLINKQ_Hand_Left/Right)
// com o mesmo papel, e a ordem dos índices muda a cada sessão do SteamVR; pegar o errado deixa a mão sem pose e as
// armas somem. Por isso a escolha é refeita 1×/s entre os dispositivos de classe Controller com o papel da mão:
// primeiro o que tem pose válida no último WGP, depois o de tipo com "touch".
static unsigned hand_device(int hand) {
	static unsigned idx[2] = {~0u, ~0u};
	static LONGLONG at[2];
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	if (now_qpc() - at[hand] < f.QuadPart && idx[hand] != ~0u) return idx[hand];
	at[hand] = now_qpc();
	int role = hand ? ETrackedControllerRole_TrackedControllerRole_LeftHand : ETrackedControllerRole_TrackedControllerRole_RightHand;
	unsigned pick = g_sys->GetTrackedDeviceIndexForControllerRole((ETrackedControllerRole)role);
	int best = -1;
	char type[64] = "";
	for (unsigned i = 1; i < 16; i++) {
		ETrackedPropertyError pe;
		char t[64] = "";
		if (g_sys->GetTrackedDeviceClass(i) != ETrackedDeviceClass_TrackedDeviceClass_Controller ||
		    g_sys->GetInt32TrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_ControllerRoleHint_Int32, &pe) != role)
			continue;
		g_sys->GetStringTrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_ControllerType_String, t, sizeof t, &pe);
		int score = (g_gpose[i].bPoseIsValid || g_rpose[i].bPoseIsValid) * 2 + (strstr(t, "touch") != NULL);
		if (score > best) best = score, pick = i, snprintf(type, sizeof type, "%s", t);
		static unsigned seen;  // cada candidato vai ao log uma vez (diagnóstico da escolha)
		if (!(seen & 1u << i)) {
			char ser[64] = "?";
			g_sys->GetStringTrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_SerialNumber_String, ser, sizeof ser, &pe);
			seen |= 1u << i, logf("armas: candidato mão %s: dispositivo %u (%s, tipo %s, pontos %d)", hand ? "E" : "D", i, ser, t, score);
		}
	}
	if (pick != idx[hand]) {
		char serial[64] = "?";
		ETrackedPropertyError pe;
		if (pick < 16) g_sys->GetStringTrackedDeviceProperty(pick, ETrackedDeviceProperty_Prop_SerialNumber_String, serial, sizeof serial, &pe);
		logf("armas: mão %s = dispositivo %u (%s, tipo %s, %s)", hand ? "E" : "D", pick, serial, type,
		     best >= 2 ? "com pose" : "sem pose");
	}
	return idx[hand] = pick;
}
bool vr_hand_pose(int hand, bool update, float m[12]) { return g_vr && device_pose(hand_device(hand & 1), update, m); }

// Tangentes do frustum do olho: l, r, b, t no plano z = -1, tiradas das linhas 0 e 1 de GetProjectionMatrix (não
// dependem de near/far).
bool vr_eye_tangents(int eye, float& l, float& r, float& b, float& t) {
	if (!g_vr) return false;
	EVREye e = eye ? EVREye_Eye_Right : EVREye_Eye_Left;
	HmdMatrix44_t M = g_sys->GetProjectionMatrix(e, 0.05f, 100.0f);
	l = (M.m[0][2] - 1) / M.m[0][0], r = (M.m[0][2] + 1) / M.m[0][0];
	b = (M.m[1][2] - 1) / M.m[1][1], t = (M.m[1][2] + 1) / M.m[1][1];
	static int logged;
	if (logged++ < 2) {
		float rl, rr, rt, rb;
		g_sys->GetProjectionRaw(e, &rl, &rr, &rt, &rb);
		logf("olho %d: tangentes da matriz l %.4f r %.4f b %.4f t %.4f | Raw left %.4f right %.4f top %.4f bottom %.4f", eye,
		     l, r, b, t, rl, rr, rt, rb);
	}
	return true;
}

// Tela de frente para a cabeça (só o yaw), na altura dos olhos.
static void place_screen() {
	TrackedDevicePose_t pose = {};
	g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding, 0, &pose, 1);
	const HmdMatrix34_t& m = pose.mDeviceToAbsoluteTracking;
	float fx = -m.m[0][2], fz = -m.m[2][2], n = sqrtf(fx * fx + fz * fz);
	if (!pose.bPoseIsValid || n < 1e-3f) fx = 0, fz = -1, n = 1;
	fx /= n, fz /= n;
	float d = cfg_float("vr", "screen_distance", 2.5f), y = pose.bPoseIsValid ? m.m[1][3] : 1.6f;
	HmdMatrix34_t t = {{{-fz, 0, -fx, m.m[0][3] + fx * d}, {0, 1, 0, y}, {fx, 0, -fz, m.m[2][3] + fz * d}}};
	g_ovl->SetOverlayTransformAbsolute(g_screen, ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
	logf("tela posicionada (pose %s)", pose.bPoseIsValid ? "válida" : "inválida");
}

// --- Texturas D3D9 com o VkImage exposto pela interop: a da tela (TELA), as dos olhos (CENA) e a do HUD. São criadas
// como compartilhadas para o DXVK não realocar a imagem: a desfragmentação de memória trocaria o VkImage que o
// SteamVR já guardou. ---
struct VkTex {
	IDirect3DTexture9* tex;
	IDirect3DSurface9* surf;
	ID3D9VkInteropTexture* itex;
	VkImageLayout layout;
	VRVulkanTextureData_t vk;
};
static VkTex g_scr, g_eye[2], g_hud;
static ID3D9VkInteropDevice* g_idev;

// Rótulos do menu de armas (0–5) e da munição no pulso (6–7): menu_texture desenha na CPU e label_flush, no Present,
// sobe para a textura do rótulo e entrega com SetOverlayTexture. Antes cada mudança ia por SetOverlayRaw, que o SteamVR
// pode passar a recusar depois de umas 200 chamadas (RequestFailed, openvr#1565): a causa provável da munição do pulso
// parada no último valor até fechar o jogo.
enum { LBL_W = 256, LBL_H = 96 };
static struct Label {
	VROverlayHandle_t o;
	VkTex t;
	IDirect3DSurface9* sys;
	volatile bool dirty;  // ponytail: o update escreve px e o Present lê sem trava; com FramePipelining 0 (padrão) é a mesma thread
	uint8_t px[LBL_W * LBL_H * 4];  // RGBA
} g_lbl[8];

static void vktex_release(VkTex& t) {
	if (t.itex) t.itex->Release();
	if (t.surf) t.surf->Release();
	if (t.tex) t.tex->Release();
	t = VkTex{};
}

static bool vktex_create(VkTex& t, UINT w, UINT h, const char* what) {
	auto dev = *(IDirect3DDevice9**)game::device;
	hook_reset();
	if (!g_idev && FAILED(dev->QueryInterface(IID_ID3D9VkInteropDevice, (void**)&g_idev)))
		return logf("interop: o device não é do DXVK"), g_vr = false;
	VkImageCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	VkImage image = 0;
	HANDLE shared = NULL;
	if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.tex, &shared)) ||
	    FAILED(t.tex->GetSurfaceLevel(0, &t.surf)) || FAILED(t.tex->QueryInterface(IID_ID3D9VkInteropTexture, (void**)&t.itex)) ||
	    FAILED(t.itex->GetVulkanImageInfo(&image, &t.layout, &ci))) {
		logf("%s: falha criando a textura %ux%u", what, w, h);
		vktex_release(t);
		return false;
	}
	VkInstance inst;
	VkPhysicalDevice phys;
	VkDevice vkdev;
	VkQueue queue;
	uint32_t qindex, qfamily;
	g_idev->GetVulkanHandles(&inst, &phys, &vkdev);
	g_idev->GetSubmissionQueue(&queue, &qindex, &qfamily);
	t.vk = {image, vkdev, phys, inst, queue, qfamily, ci.width, ci.height, (uint32_t)ci.format, (uint32_t)ci.samples};
	logf("%s: textura %ux%u compartilhada, formato vk %d layout %d, fila %u/%u", what, ci.width, ci.height, ci.format,
	     t.layout, qfamily, qindex);
	return true;
}

static void vktex_check_image(VkTex& t, const char* what) {
	VkImage image = 0;
	t.itex->GetVulkanImageInfo(&image, NULL, NULL);
	if (image != t.vk.m_nImage) logf("%s: VkImage trocado pelo DXVK (%llx -> %llx)", what, t.vk.m_nImage, image), t.vk.m_nImage = image;
}

// Textura do olho (RT A8R8G8B8 do tamanho da saída do olho), criada sob demanda pelo laço de olhos (hook do
// CLevel::vf162). Se o tamanho mudar, as duas são recriadas.
IDirect3DSurface9* vr_eye_surface(int eye, UINT w, UINT h) {
	VkTex& t = g_eye[eye];
	D3DSURFACE_DESC d;
	if (t.surf && (t.surf->GetDesc(&d), d.Width != w || d.Height != h)) vktex_release(g_eye[0]), vktex_release(g_eye[1]);
	if (!t.surf) {
		// sem VR só a RT importa; com VR também a interop (vktex_create desliga o VR se o device não for do DXVK)
		vktex_create(t, w, h, eye ? "olho D" : "olho E");
	}
	return t.surf;
}

// O DXVK recusa Reset (slot 16 da vtable do device) com recursos D3DPOOL_DEFAULT vivos: soltar todas as texturas do
// mod antes.
static void probe_release();
typedef HRESULT(STDMETHODCALLTYPE* reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static reset_t o_reset;
static HRESULT STDMETHODCALLTYPE hk_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
	vktex_release(g_scr), vktex_release(g_eye[0]), vktex_release(g_eye[1]), vktex_release(g_hud), probe_release();
	for (Label& l : g_lbl) vktex_release(l.t), l.dirty = l.o;  // sobe de novo na textura nova
	HRESULT hr = o_reset(dev, pp);
	logf("Reset %ux%u janela=%d -> %08lx", pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed, hr);
	return hr;
}

void hook_reset() {
	auto dev = *(IDirect3DDevice9**)game::device;
	if (!o_reset && dev) o_reset = (reset_t)patch_ptr((uintptr_t)(*(void***)dev + 16), (void*)hk_reset);
}

// --- Submissão ao HMD ---
// Timing explícito (padrão, [stereo] timing=1): WGP e SubmitExplicitTimingData no wrapper do RenderTask (antes do
// BeginFrame e fora do lock do renderer); Submit dos olhos e PostPresentHandoff logo antes do Present do jogo. Todo WGP
// tem exatamente um handoff. Timing implícito (timing=0): WGP sob Lock no fim do RenderTask, sem dados de timing nem
// handoff.
// Estados: CENA (olhos no HMD, overlay da tela escondido) e TELA (o jogo numa tela virtual). Sem olhos, a CENA só cai
// para a TELA depois de tela_hysteresis frames.
static inline bool on_loading_thread() {
	HANDLE h = *(HANDLE*)game::loading_thread;
	return h && GetThreadId(h) == GetCurrentThreadId();
}

static void q_lock() {
	EnterCriticalSection(&g_q);
	g_idev->LockSubmissionQueue();
}
static void q_unlock() {
	g_idev->ReleaseSubmissionQueue();
	LeaveCriticalSection(&g_q);
}

// Troca entre CENA e TELA. Na TELA o ambiente do SteamVR some num preto do compositor (ajuste "cinema"; o fade não
// apaga os overlays) e, com tela_follow, a tela vai para a frente do olhar a cada entrada (cutscene, menu, pausa,
// loading).
static void set_state(bool cena, const char* why) {
	if (cena == g_cena) return;
	g_cena = cena;
	float cinema = cena ? 0 : setting("cinema", 1);
	if (cena) {
		g_ovl->HideOverlay(g_screen);
	} else {
		if (g_idev) q_lock(), g_comp->ClearLastSubmittedFrame(), q_unlock();
		g_ovl->ShowOverlay(g_screen);
		g_n_cena_tela++;
		if (setting("tela_follow", 1)) InterlockedExchange(&g_vr_recenter, 1);
	}
	if (g_idev) q_lock(), g_comp->FadeToColor(0.3f, 0, 0, 0, cinema, false), q_unlock();
	logf("VR: %s (%s, tid %lu)", cena ? "CENA" : "TELA", why, GetCurrentThreadId());
}

static void handoff_locked() {
	if (!g_explicit) return;
	g_comp->PostPresentHandoff();
	g_n_handoff++;
}

// Começo do RenderTask, na thread principal: trata os eventos do SteamVR e, no timing explícito, faz o WGP. O WGP só
// roda em frame de gameplay (o update do nível do jogo rodou neste frame) ou com a CENA ainda em histerese.
void vr_render_task_pre(bool game_frame) {
	if (!g_vr || !g_idev) return;
	VREvent_t ev;
	while (g_sys->PollNextEvent(&ev, sizeof ev))
		if (ev.eventType == EVREventType_VREvent_ChaperoneUniverseHasChanged ||
		    ev.eventType == EVREventType_VREvent_StandingZeroPoseReset || ev.eventType == EVREventType_VREvent_SeatedZeroPoseReset) {
			// recentralização pelo SteamVR ou pelo headset (no Quest, segurar o botão Meta): o mod recentra junto e a
			// direção da cabeça vira a frente do jogador
			logf("VR: evento %u de recentralização; recentrando a pose do jogo", ev.eventType);
			pose_state().calibrated = false;
		} else if (ev.eventType == EVREventType_VREvent_Quit) {
			logf("VR: VREvent_Quit; caminho VR desligado (o jogo segue)");
			g_sys->AcknowledgeQuit_Exiting();
			set_state(false, "quit");
			g_vr = false, g_quit = true;
			return;
		}
	int mode = (int)setting("mode", 0);  // 0 AUTO, 1 CENA, 2 TELA
	if (!g_explicit || mode == 2 || !(game_frame || g_cena || mode == 1) || now_qpc() < g_backoff_until) return;
	if (g_handoff_pending) q_lock(), handoff_locked(), q_unlock(), g_handoff_pending = false, g_handoff_since_submit = true;
	bool lock = !g_handoff_since_submit;  // sem handoff desde o último Submit o WGP pode mexer na fila: precisa do Lock
	LONGLONG t0 = now_qpc();
	if (lock) q_lock(), g_n_wgp_lock++;
	EVRCompositorError e = g_comp->WaitGetPoses(g_rpose, 16, g_gpose, 16);
	if (!e) g_pose_pc = present_count();
	if (lock) q_unlock();
	LONGLONG t1 = now_qpc();
	stats_add("wgp", qpc_ms(t1 - t0));
	if (g_last_wgp) stats_add("wgp.intervalo", qpc_ms(t1 - g_last_wgp));
	g_last_wgp = t1;
	if (e) {
		g_last_err = e;
		if (++g_wgp_fail >= 10) {  // runtime sem foco ou com erro: TELA e nova tentativa com backoff (1, 2, 4… 10 s)
			static int backoff_s = 1;
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			g_backoff_until = t1 + backoff_s * f.QuadPart;
			logf("VR: WaitGetPoses falhou %d vezes (erro %d); TELA por %d s", g_wgp_fail, e, backoff_s);
			backoff_s = backoff_s * 2 > 10 ? 10 : backoff_s * 2;
			g_wgp_fail = 0;
			set_state(false, "WGP com erro");
		}
		return;
	}
	g_wgp_fail = 0;
	g_n_wgp++;
	g_wgp_pc = present_count();
	g_handoff_pending = true;
	q_lock(), g_comp->SubmitExplicitTimingData(), q_unlock();
}

// Fim do RenderTask (timing implícito): WGP sob Lock, logo depois do Present e do CRenderer::vf42 (que solta o lock
// do renderer).
void vr_render_task_post(bool game_frame) {
	if (!g_vr || !g_idev || g_explicit) return;
	int mode = (int)setting("mode", 0);
	if (mode == 2 || !(game_frame || g_cena || mode == 1)) return;
	LONGLONG t0 = now_qpc();
	q_lock();
	EVRCompositorError e = g_comp->WaitGetPoses(g_rpose, 16, g_gpose, 16);
	q_unlock();
	if (!e) g_pose_pc = present_count();
	stats_add("wgp", qpc_ms(now_qpc() - t0));
	g_n_wgp_lock++;
	if (e) g_last_err = e;
	else g_n_wgp++, g_wgp_since_submit = true;
}

// O laço de olhos (hook do CLevel::vf162) só roda se houve WGP neste frame (explícito) ou desde o último Submit
// (implícito).
bool vr_eyes_allowed() {
	if (!g_vr) return true;  // sem VR (fake_hmd) o laço roda sempre
	return g_explicit ? g_wgp_pc == present_count() : g_wgp_since_submit;
}

static void submit_eyes() {
	VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	for (int i = 0; i < 2; i++) {
		vktex_check_image(g_eye[i], i ? "olho D" : "olho E");
		g_idev->TransitionTextureLayout(g_eye[i].itex, &all, g_eye[i].layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	}
	g_idev->FlushRenderingCommands();
	q_lock();
	VRTextureBounds_t b = {0, 0, 1, 1};
	if (g_comp->CanRenderScene()) {
		for (int i = 0; i < 2; i++) {
			Texture_t tex = {&g_eye[i].vk, ETextureType_TextureType_Vulkan, EColorSpace_ColorSpace_Auto};
			EVRCompositorError e = g_comp->Submit(i ? EVREye_Eye_Right : EVREye_Eye_Left, &tex, &b, EVRSubmitFlags_Submit_Default);
			if (e == EVRCompositorError_VRCompositorError_AlreadySubmitted) g_n_already++;
			if (e) g_n_submit_err++, g_last_err = e;
			else g_n_submit++;
		}
	}
	if (g_handoff_pending) handoff_locked();
	q_unlock();
	for (int i = 0; i < 2; i++)
		g_idev->TransitionTextureLayout(g_eye[i].itex, &all, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_eye[i].layout);
	g_wgp_since_submit = false;
}

// --- HUD no headset. O passe de sprites (CRenderContext::vf44 com a fila global 0x0165CF64, chamado em 0x0040B507)
// roda depois dos olhos e desenharia a UI só no backbuffer, que é o espelho no monitor. Nos frames com olhos o RT 0
// vira a textura do HUD, limpa com alfa 0, e o overlay cojvr.hud a mostra presa à cabeça. ---
static VROverlayHandle_t g_hud_ovl;
static unsigned g_hud_pc;  // frame cuja UI está na textura do HUD
// Fade de tela cheia dentro da UI do nível (o preto de ~1 s ao começar ou recarregar uma fase): num painel preso à
// cabeça ele vira um quadrado preto na frente do 3D. A cada frame uma cópia 16x16 da textura do HUD vai para a memória,
// e a de 2 frames atrás (já pronta, sem esperar a GPU) diz quanto dela está opaca: acima de hud_full (0,85) o overlay
// fica escondido. Sem leitura recente (os 2 primeiros frames com HUD) ele também fica escondido. Se a cópia falhar, a
// detecção desliga e o HUD aparece sempre.
static bool g_pr_off;
static IDirect3DSurface9 *g_pr_rt[3], *g_pr_sys[3];
static unsigned g_pr_pc[3], g_full_pc = ~0u, g_clear_pc = ~0u;  // frame da cópia; da última leitura opaca e da limpa
static int g_pr_i;
static void probe_release() {
	for (int i = 0; i < 3; i++) {
		if (g_pr_rt[i]) g_pr_rt[i]->Release(), g_pr_rt[i] = NULL;
		if (g_pr_sys[i]) g_pr_sys[i]->Release(), g_pr_sys[i] = NULL;
		g_pr_pc[i] = 0;
	}
}
static void hud_probe(IDirect3DDevice9* dev) {
	int i = g_pr_i, j = (i + 1) % 3;
	if (!g_pr_rt[i] && (FAILED(dev->CreateRenderTarget(16, 16, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_pr_rt[i], NULL)) ||
	                    FAILED(dev->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_pr_sys[i], NULL))))
		return probe_release(), void(g_pr_off = true), logf("HUD: sem a cópia da textura; fade de tela cheia não detectado");
	if (FAILED(dev->StretchRect(g_hud.surf, NULL, g_pr_rt[i], NULL, D3DTEXF_POINT)) || FAILED(dev->GetRenderTargetData(g_pr_rt[i], g_pr_sys[i])))
		return void(g_pr_off = true), logf("HUD: cópia da textura falhou; fade de tela cheia não detectado");
	g_pr_pc[i] = present_count();
	D3DLOCKED_RECT lr;
	if (g_pr_pc[j] && SUCCEEDED(g_pr_sys[j]->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
		int n = 0;
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++) n += ((unsigned char*)lr.pBits)[y * lr.Pitch + x * 4 + 3] > 128;
		g_pr_sys[j]->UnlockRect();
		bool full = n > 256 * setting("hud_full", 0.85f), was = g_full_pc + 3 >= present_count();
		(full ? g_full_pc : g_clear_pc) = g_pr_pc[j];
		if (full && !was) logf("HUD: fade de tela cheia na UI (%d%% opaca): overlay escondido", n * 100 / 256);
	}
	g_pr_i = j;
}
typedef void(__thiscall* vf44_t)(void*, void*, int, void*, int);
static vf44_t o_vf44;
static void __fastcall hk_vf44(void* ctx, void*, void* queue, int a, void* vars, int b) {
	auto dev = *(IDirect3DDevice9**)game::device;
	IDirect3DSurface9* old = NULL;
	// fake_hmd (teste sem headset): a textura do HUD é preenchida igual, para o SHOT e a sonda, mas sem overlay
	bool cap = (g_vr || setting("fake_hmd", 0)) && setting("hud", 1) && queue == *(void**)0x0165CF64 && stereo_frame() &&
	           SUCCEEDED(dev->GetRenderTarget(0, &old));
	if (cap) {
		D3DSURFACE_DESC d, t;
		old->GetDesc(&d);
		if (g_hud.surf && (g_hud.surf->GetDesc(&t), t.Width != d.Width || t.Height != d.Height)) vktex_release(g_hud);
		cap = g_hud.surf || vktex_create(g_hud, d.Width, d.Height, "HUD");
	}
	D3DVIEWPORT9 vp;
	if (cap) {  // SetRenderTarget põe o viewport no RT inteiro: limpar tudo e devolver o do engine (ele cacheia o estado)
		dev->GetViewport(&vp), dev->SetRenderTarget(0, g_hud.surf), dev->Clear(0, NULL, D3DCLEAR_TARGET, 0, 1, 0), dev->SetViewport(&vp);
	}
	o_vf44(ctx, queue, a, vars, b);
	if (cap) {
		dev->GetViewport(&vp), dev->SetRenderTarget(0, old), dev->SetViewport(&vp);
		g_hud_pc = present_count();
		if (!g_pr_off) hud_probe(dev);
		if (g_shot_request == 2) dump_surface(g_hud.surf, "_hud");  // comando SHOT: o BMP guarda o alfa (conferir o fundo)
	}
	if (old) old->Release();
}

// O fade de tela cheia do FadeOutModule (*0x02E65FAC; o preto ao entrar no jogo) iria para o HUD preso à cabeça e
// cobriria a visão inteira. Nos frames com olhos ele fica fora da fila da UI (CLevel::vf164, thiscall com 2 args);
// hud_fade 1 volta a mostrá-lo. O vf164 recebe o nível do módulo ("FadeoutModule"), em FadeOutModule+4.
typedef void(__thiscall* vf164_t)(char*, void*, void*);
static vf164_t o_vf164;
static void __fastcall hk_vf164(char* lvl, void*, void* a, void* b) {
	char* fm = *(char**)0x02E65FAC;
	if ((g_vr || setting("fake_hmd", 0)) && fm && (lvl == fm || lvl == *(char**)(fm + 4)) && stereo_frame() && setting("hud", 1) && !setting("hud_fade", 0)) {
		static bool logged;
		if (!logged) logged = true, logf("HUD: fade do FadeOutModule %p fora do HUD", (void*)lvl);
		return;
	}
	o_vf164(lvl, a, b);
}

// Vídeos (.wmv): o jogo entrega o som do vídeo à voz do XAudio2 uma vez por frame (CWMFDecoder::Frame, vf2). Com o
// limite tela_fps da TELA o som fica sem dados entre frames e estala; por isso, enquanto um vídeo roda, a TELA fica
// sem limite.
typedef void(__thiscall* wmf_frame_t)(void*);
static wmf_frame_t o_wmf_frame;
static volatile LONGLONG g_movie_qpc;  // ponytail: escrita de 64 bits não atômica; no pior caso, um frame com o limite
static void __fastcall hk_wmf_frame(void* self, void*) {
	g_movie_qpc = now_qpc();
	o_wmf_frame(self);
}

void hud_init() {
	o_vf44 = (vf44_t)patch_ptr(0x012E78A4, (void*)hk_vf44);     // CRenderContext::vf44 (vtable 0x012E77F4, slot 44)
	o_vf164 = (vf164_t)patch_ptr(0x0129F6BC, (void*)hk_vf164);  // CLevel::vf164 (vtable 0x0129F42C, slot 164)
	o_wmf_frame = (wmf_frame_t)patch_ptr(0x012D1EE0, (void*)hk_wmf_frame);  // CWMFDecoder::vf2 (vtable 0x012D1ED8, slot 2)
}

static VROverlayHandle_t g_wheel_ovl;
static unsigned g_wheel_pc;
static float g_wheel_pos[3], g_wheel_w, g_wheel_uv[4];
// Roda de armas aberta neste frame: posição e largura no tracking (m); uv = u0 v0 u1 v1 do recorte na textura do HUD.
void vr_wheel(const float* pos, float width, const float* uv) {
	memcpy(g_wheel_pos, pos, 12), g_wheel_w = width, memcpy(g_wheel_uv, uv, 16), g_wheel_pc = present_count();
}
void vr_hud_band(int* b) {  // faixa 16:9 da UI na textura do HUD (w, h, x, y) e o tamanho da textura (W, H)
	char* g = *(char**)game::cgame;
	memcpy(b, g ? (int*)(g + 0x5B8) : (int[4]){0, 0, 0, 0}, 16), b[4] = g_hud.vk.m_nWidth, b[5] = g_hud.vk.m_nHeight;
}

// A textura do HUD deste frame (ou a de um rótulo) num overlay.
static EVROverlayError hud_texture(VROverlayHandle_t o, VkTex& t = g_hud, const char* what = "HUD") {
	vktex_check_image(t, what);
	VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	Texture_t tex = {&t.vk, ETextureType_TextureType_Vulkan, EColorSpace_ColorSpace_Auto};
	g_idev->TransitionTextureLayout(t.itex, &all, t.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	g_idev->FlushRenderingCommands();
	q_lock();
	EVROverlayError e = g_ovl->SetOverlayTexture(o, &tex);
	q_unlock();
	g_idev->TransitionTextureLayout(t.itex, &all, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.layout);
	return e;
}

// Rótulos desenhados desde o último Present: RGBA -> superfície na RAM (A8R8G8B8 = BGRA) -> textura -> overlay.
static void label_flush() {
	auto dev = *(IDirect3DDevice9**)game::device;
	for (Label& l : g_lbl) {
		if (!l.dirty) continue;
		l.dirty = false;
		D3DLOCKED_RECT r;
		if ((!l.t.surf && !vktex_create(l.t, LBL_W, LBL_H, "rótulo")) ||
		    (!l.sys && FAILED(dev->CreateOffscreenPlainSurface(LBL_W, LBL_H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &l.sys, NULL))) ||
		    FAILED(l.sys->LockRect(&r, NULL, 0))) {
			logf("HUD: rótulo sem textura; SetOverlayRaw -> %d", g_ovl->SetOverlayRaw(l.o, l.px, LBL_W, LBL_H, 4));
			continue;
		}
		for (int y = 0; y < LBL_H; y++)
			for (int x = 0; x < LBL_W; x++) {
				const uint8_t* s = l.px + (y * LBL_W + x) * 4;
				uint8_t* d = (uint8_t*)r.pBits + y * r.Pitch + x * 4;
				d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = s[3];
			}
		l.sys->UnlockRect();
		HRESULT hr = dev->UpdateSurface(l.sys, NULL, l.t.surf, NULL);
		EVROverlayError e = SUCCEEDED(hr) ? hud_texture(l.o, l.t, "rótulo") : EVROverlayError_VROverlayError_RequestFailed;
		static int logged;
		if (logged < 3 || e) logged++, logf("HUD: rótulo %d: UpdateSurface %08lx, SetOverlayTexture -> %d", (int)(&l - g_lbl), hr, e);
	}
}

// Overlay do HUD em CENA quando a UI deste frame foi para a textura; preso à cabeça (hud_dist, hud_width, hud_y em m).
static void hud_submit() {
	bool show = g_cena && g_hud.itex && g_hud_pc == present_count() && g_wheel_pc != present_count();  // roda aberta: só ela
	if (!g_pr_off) show = show && g_clear_pc != ~0u && g_clear_pc + 3 >= present_count() && !(g_full_pc != ~0u && g_full_pc > g_clear_pc);
	if (!show || (!g_hud_ovl && g_ovl->CreateOverlay((char*)"cojvr.hud", (char*)"CoJ VR HUD", &g_hud_ovl))) {
		if (g_hud_ovl) g_ovl->HideOverlay(g_hud_ovl);
		return;
	}
	// Usa a textura inteira (o resto é transparente): as legendas ficam no rodapé, fora da faixa 16:9 da UI.
	// Na pausa o menu vira um painel parado no mundo, na frente de onde se olhava ao pausar (pause_dist, pause_width).
	static bool paused;
	if (player_paused()) {
		static HmdMatrix34_t at;
		if (!paused) {
			const float* m = g_rpose[0].mDeviceToAbsoluteTracking.m[0];
			float fx = -m[2], fz = -m[10], n = sqrtf(fx * fx + fz * fz), d = setting("pause_dist", 2);
			if (n < 1e-3f) fx = 0, fz = -1, n = 1;
			fx /= n, fz /= n;
			at = {{{-fz, 0, -fx, m[3] + fx * d}, {0, 1, 0, m[7]}, {fx, 0, -fz, m[11] + fz * d}}};
			logf("HUD: pausa em 3D, menu a %.1f m", d);
		}
		g_ovl->SetOverlayWidthInMeters(g_hud_ovl, setting("pause_width", 2.4f));
		g_ovl->SetOverlayTransformAbsolute(g_hud_ovl, ETrackingUniverseOrigin_TrackingUniverseStanding, &at);
	} else {
		g_ovl->SetOverlayWidthInMeters(g_hud_ovl, setting("hud_width", 1.3f));
		HmdMatrix34_t t = {{{1, 0, 0, 0}, {0, 1, 0, setting("hud_y", -0.1f)}, {0, 0, 1, -setting("hud_dist", 1.5f)}}};
		g_ovl->SetOverlayTransformTrackedDeviceRelative(g_hud_ovl, k_unTrackedDeviceIndex_Hmd, &t);
	}
	paused = player_paused();
	EVROverlayError e = hud_texture(g_hud_ovl);
	g_ovl->ShowOverlay(g_hud_ovl);
	static int logged;
	if (logged < 3 || e) logged++, logf("HUD: HUD SetOverlayTexture -> %d", e);
}

// A roda de armas do jogo num painel virado para a cabeça, recortado da textura do HUD (vr_wheel a cada frame).
static void wheel_submit() {
	bool show = g_cena && g_hud.itex && g_hud_pc == present_count() && g_wheel_pc == present_count();
	if (!show || (!g_wheel_ovl && g_ovl->CreateOverlay((char*)"cojvr.wheel", (char*)"CoJ VR roda", &g_wheel_ovl))) {
		if (g_wheel_ovl) g_ovl->HideOverlay(g_wheel_ovl);
		return;
	}
	VRTextureBounds_t bd = {g_wheel_uv[0], g_wheel_uv[1], g_wheel_uv[2], g_wheel_uv[3]};
	g_ovl->SetOverlayTextureBounds(g_wheel_ovl, &bd);
	const float *hp = g_rpose[0].mDeviceToAbsoluteTracking.m[0], *p = g_wheel_pos;
	float z[3] = {hp[3] - p[0], 0, hp[11] - p[2]}, n = sqrtf(z[0] * z[0] + z[2] * z[2]);  // de pé, virado para a cabeça
	if (n < 1e-3f) z[2] = 1, n = 1;
	z[0] /= n, z[2] /= n;
	HmdMatrix34_t t = {{{z[2], 0, z[0], p[0]}, {0, 1, 0, p[1]}, {-z[0], 0, z[2], p[2]}}};
	g_ovl->SetOverlayWidthInMeters(g_wheel_ovl, g_wheel_w);
	g_ovl->SetOverlayTransformAbsolute(g_wheel_ovl, ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
	hud_texture(g_wheel_ovl);
	g_ovl->ShowOverlay(g_wheel_ovl);
}

// Antes do Present do jogo. CENA: Submit dos olhos (e handoff). TELA: copiar o backbuffer para a textura da tela.
void vr_frame_pre() {
	if (!g_vr) return;
	for (int h = 0; h < 2; h++)  // vibração legada em curso: um pulso por frame (até 3999 µs; a força vira a duração)
		if (now_qpc() < g_buzz_until[h] && hand_device(h) < 16)
			g_sys->TriggerHapticPulse(hand_device(h), 0, (unsigned short)(3999 * fminf(1, g_buzz_amp[h])));
	if (g_arc_n && g_arc_pc != present_count() && !on_loading_thread()) vr_arc(NULL, 0);  // arco sem atualização no frame some
	if (g_menu_n && g_menu_pc != present_count() && !on_loading_thread()) vr_menu(0, NULL, NULL, -1);  // idem o menu
	for (int h = 0; h < 2; h++)  // idem a munição no pulso
		if (g_wrist_txt[h][0] && g_wrist_pc[h] != present_count() && !on_loading_thread()) vr_wrist(h, NULL);
	for (int h = 0; h < 4; h++)  // idem as miras
		if (g_aim_shown[h] && g_aim_pc[h] != present_count() && !on_loading_thread()) g_ovl->HideOverlay(g_aim[h]), g_aim_shown[h] = false;
	bool loading = on_loading_thread();
	if (!loading) {
		label_flush();
		bool eyes = stereo_frame() && g_eye[0].itex && g_eye[1].itex && vr_eyes_allowed();
		if (eyes) {
			g_no_eyes = 0;
			set_state(true, "olhos neste frame");
			submit_eyes();
			hud_submit();
			wheel_submit();
		} else {
			if (g_handoff_pending) q_lock(), handoff_locked(), q_unlock();
			int hyst = (int)setting("tela_hysteresis", 10);
			const char* why = player_tela_reason();  // com motivo (menu, cutscene etc.), TELA na hora, sem histerese
			if (g_cena && (why || ++g_no_eyes >= hyst)) set_state(false, why ? why : "sem olhos");
			hud_submit();  // sem UI nova o HUD some
			wheel_submit();
		}
		if (g_handoff_pending) g_handoff_pending = false, g_handoff_since_submit = true;
	} else if (g_cena) {
		set_state(false, "loading");  // só troca o overlay; WGP e handoff ficam na thread principal
	}
	if (g_cena) return;
	auto dev = *(IDirect3DDevice9**)game::device;
	IDirect3DSurface9* bb;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
	D3DSURFACE_DESC d;
	bb->GetDesc(&d);
	if (g_scr.surf) {
		D3DSURFACE_DESC t;
		g_scr.surf->GetDesc(&t);
		if (t.Width != d.Width || t.Height != d.Height) vktex_release(g_scr);
	}
	if (g_scr.surf || vktex_create(g_scr, d.Width, d.Height, "tela")) dev->StretchRect(bb, NULL, g_scr.surf, NULL, D3DTEXF_NONE);
	bb->Release();
}

// Depois do Present. TELA: entregar a textura ao overlay (no máximo ~90 vezes por segundo) e limitar o jogo a
// tela_fps (o menu a ~1650 fps disputaria a GPU com o compositor).
void vr_frame_post() {
	if (!g_vr || g_cena || !g_scr.tex) return;
	static LONGLONG last, last_frame;
	LONGLONG now = now_qpc();
	float fps = setting("tela_fps", 90);
	if (fps > 0 && last_frame && qpc_ms(now - g_movie_qpc) > 500) {  // sem limite durante vídeos (som por frame)
		double left = 1000.0 / fps - qpc_ms(now - last_frame);
		if (left > 1) Sleep((DWORD)left);
		now = now_qpc();
	}
	last_frame = now;
	if (qpc_ms(now - last) < 1000.0 / 90) return;
	last = now;
	if (InterlockedExchange(&g_vr_recenter, 0)) place_screen();
	vktex_check_image(g_scr, "tela");

	// No modo de vídeo do olho a UI fica numa faixa 16:9 do backbuffer (CGame+0x5B8: w, h, x, y); a TELA mostra só ela.
	static int band_y = -1, band_h = -1;
	char* g = *(char**)game::cgame;
	int* ui = g ? (int*)(g + 0x5B8) : NULL;
	if (video_eye_mode() && ui && ui[1] > 0 && (ui[3] != band_y || ui[1] != band_h)) {
		band_y = ui[3], band_h = ui[1];
		VRTextureBounds_t b = {0, (float)band_y / g_scr.vk.m_nHeight, 1, (float)(band_y + band_h) / g_scr.vk.m_nHeight};
		g_ovl->SetOverlayTextureBounds(g_screen, &b);
		logf("tela: recorte da UI v %.3f..%.3f", b.vMin, b.vMax);
	}

	VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	Texture_t tex = {&g_scr.vk, ETextureType_TextureType_Vulkan, EColorSpace_ColorSpace_Auto};
	g_idev->TransitionTextureLayout(g_scr.itex, &all, g_scr.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	g_idev->FlushRenderingCommands();
	q_lock();
	EVROverlayError e = g_ovl->SetOverlayTexture(g_screen, &tex);
	q_unlock();
	g_idev->TransitionTextureLayout(g_scr.itex, &all, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_scr.layout);

	static int logged;
	if (logged < 3) logged++, logf("SetOverlayTexture -> %d", e);
	// Repetir após um erro derruba o processo: com o cliente SteamVR de 32 bits a 2ª chamada falha no driver e a ponte
	// do Proton aborta. Por isso a tela é desligada no primeiro erro.
	if (e) logf("tela desligada após erro %d (cliente SteamVR de 32 bits? rode com PROTON_USE_WOW64=1)", e), g_vr = false;
}

// A cada 5 s: contagens do Submit e do handoff, WGPs (com Lock) e o GetFrameTiming do compositor.
void vr_report() {
	if (!g_vr && !g_quit) return;
	logf("VR: %s | WGP %ld (com Lock %ld) | Submit ok %ld, erro %ld (AlreadySubmitted %ld, último erro %ld) | handoff %ld |"
	     " CENA->TELA %ld",
	     g_cena ? "CENA" : "TELA", InterlockedExchange(&g_n_wgp, 0), InterlockedExchange(&g_n_wgp_lock, 0),
	     InterlockedExchange(&g_n_submit, 0), InterlockedExchange(&g_n_submit_err, 0), InterlockedExchange(&g_n_already, 0),
	     g_last_err, InterlockedExchange(&g_n_handoff, 0), InterlockedExchange(&g_n_cena_tela, 0));
	if (!g_vr) return;
	// Pelo wow64 do Proton o m_nSize passa sem ajuste, e a struct tem 192 B no Windows 32-bit e 188 B no Linux 64-bit:
	// tenta 188 num buffer de 192 e, se falhar, 192.
	union {
		Compositor_FrameTiming t;
		char raw[192];
	} ft = {};
	ft.t.m_nSize = 188;
	bool ok = g_comp->GetFrameTiming(&ft.t, 0);
	static bool tried192;
	if (!ok && !tried192) {
		tried192 = true;
		ft.t.m_nSize = 192;
		ok = g_comp->GetFrameTiming(&ft.t, 0);
		logf("VR: GetFrameTiming com 188 falhou; com 192 -> %d", ok);
	}
	if (ok)
		logf("VR: timing: intervalo do cliente %.2f ms, GPU total %.2f ms, quadros perdidos %u, mal apresentados %u,"
		     " reprojeção %#x", ft.t.m_flClientFrameIntervalMs, ft.t.m_flTotalRenderGpuMs, ft.t.m_nNumDroppedFrames,
		     ft.t.m_nNumMisPresented, ft.t.m_nReprojectionFlags);
}

// Vibração no controle da mão (0 = direita, 1 = esquerda).
// haptic_mode: 1 = ação do SteamVR Input; 2 = pulso legado (TriggerHapticPulse) direto no controle, repetido a cada
// frame enquanto durar; 3 = os dois. O padrão é 2 porque o vrlink do Quest cria dispositivos de rastreio de mão no
// mesmo /user/hand e a ação pode ir para eles: devolve sucesso e nada vibra.
void vr_haptic(int hand, float sec, float hz, float amp) {
	if (!g_vr) return;
	int mode = (int)setting("haptic_mode", 2), h = hand & 1;
	EVRInputError e = EVRInputError_VRInputError_None;
	if ((mode & 1) && g_in) q_lock(), e = g_in->TriggerHapticVibrationAction(g_haptic[h], 0, sec, hz, amp, k_ulInvalidInputValueHandle), q_unlock();
	if (mode & 2) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_buzz_until[h] = now_qpc() + (LONGLONG)(sec * f.QuadPart), g_buzz_amp[h] = amp;
	}
	static int n;
	if (n++ < 10 || e) logf("vibração: vibração mão %d (modo %d) %.0f ms %.0f Hz %.2f -> %d", h, mode, sec * 1000, hz, amp, e);
}

// Vinheta de conforto: overlay preso ao HMD, borda preta e centro livre, que aparece em ~0,15 s enquanto o jogador
// anda ou gira (target = intensidade 0..1; 0 esconde). Textura 128×128 gerada aqui.
static VROverlayHandle_t g_vig;
void vr_vignette(float target) {
	if (!g_vr || !g_ovl) return;
	static float cur;
	static bool shown;
	static LONGLONG last;
	LONGLONG now = now_qpc();
	float dt = last ? fminf(0.1f, (float)qpc_ms(now - last) / 1000) : 0;
	last = now, cur += (target > cur ? 1 : -1) * fminf(fabsf(target - cur), dt / 0.15f);
	if (!g_vig) {
		if (g_ovl->CreateOverlay((char*)"cojvr.vignette", (char*)"CoJ VR vinheta", &g_vig)) return;
		static uint8_t px[128 * 128 * 4];
		for (int y = 0; y < 128; y++)
			for (int x = 0; x < 128; x++) {  // raio no plano (meia largura = 1 m, a 0,5 m): livre até ~31°, preto a ~56°
				float u = hypotf(x - 63.5f, y - 63.5f) / 64, a = fminf(1, fmaxf(0, (u - 0.30f) / 0.45f));
				px[(y * 128 + x) * 4 + 3] = (uint8_t)(255 * a * a * (3 - 2 * a));
			}
		g_ovl->SetOverlayRaw(g_vig, px, 128, 128, 4);
		HmdMatrix34_t t = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, -0.5f}}};
		g_ovl->SetOverlayTransformTrackedDeviceRelative(g_vig, k_unTrackedDeviceIndex_Hmd, &t);
		g_ovl->SetOverlayWidthInMeters(g_vig, 2.0f);
		g_ovl->SetOverlaySortOrder(g_vig, 10);  // por cima do HUD e das miras
		logf("vinheta: criada");
	}
	if (cur < 0.01f) {
		if (shown) g_ovl->HideOverlay(g_vig), shown = false;
		return;
	}
	g_ovl->SetOverlayAlpha(g_vig, cur);
	if (!shown) g_ovl->ShowOverlay(g_vig), shown = true;
}

// Ponto de mira de cada mão (overlay pequeno, sempre virado para a cabeça). Retículo RGBA 64×64 gerado aqui, branco
// com borda escura e colorido pelo SetOverlayColor (que multiplica a textura). O overlay some se a mão não mandou o
// ponto neste frame (TELA etc.). Formato (crosshair_shape: 0 anel com ponto, 1 ponto, 2 anel, 3 cruz) e cor por mão
// (crosshair_color_r/_l, paleta abaixo; padrão direita vermelho, esquerda azul) mudam ao vivo.
// hand 2 = marcador do alvo do "usar" (anel com ponto, cor pelo tint); hand 3 = mira da cabeça (ponto).
static int g_aim_shape[4] = {-1, -1, -1, -1};
void vr_reticle(int hand, const float* p, float width, const float* tint) {
	if (!g_vr || !g_ovl) return;
	int h = hand == 2 || hand == 3 ? hand : hand & 1;
	if (!g_aim[h]) {
		static const char* keys[4] = {"cojvr.aim0", "cojvr.aim1", "cojvr.use", "cojvr.gaze"};
		if (g_ovl->CreateOverlay((char*)keys[h], (char*)"CoJ VR mira", &g_aim[h])) return;
	}
	int shape = h == 2 ? 0 : h == 3 ? 1 : (int)setting("crosshair_shape", 0) & 3;
	if (shape != g_aim_shape[h]) {
		static uint8_t px[64 * 64 * 4];
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++) {
				float dx = fabsf(x - 31.5f), dy = fabsf(y - 31.5f), r = sqrtf(dx * dx + dy * dy);
				auto cross = [&](float w, float a, float b) { return (dx < w && dy > a && dy < b) || (dy < w && dx > a && dx < b); };
				bool lit = shape == 0 ? (r > 22 && r < 27) || r < 4 : shape == 1 ? r < 6 : shape == 2 ? r > 22 && r < 27 : cross(2.5f, 6, 28);
				bool edge = shape == 0 ? (r > 20 && r < 29) || r < 6 : shape == 1 ? r < 8.5f : shape == 2 ? r > 20 && r < 29 : cross(4.5f, 4, 30);
				uint8_t* q = px + (y * 64 + x) * 4;
				q[0] = q[1] = q[2] = lit ? 255 : 15, q[3] = lit ? 235 : edge ? 140 : 0;
			}
		EVROverlayError e = g_ovl->SetOverlayRaw(g_aim[h], px, 64, 64, 4);
		logf("HUD: retículo %d no formato %d (SetOverlayRaw -> %d)", h, shape, e);
		g_aim_shape[h] = shape;
	}
	static const float pal[8][3] = {{1, .16f, .16f}, {.24f, .55f, 1}, {.3f, 1, .3f}, {1, .9f, .2f},  // vermelho, azul, verde, amarelo,
	                                {1, 1, 1}, {1, .55f, .1f}, {.7f, .35f, 1}, {.2f, 1, 1}};      // branco, laranja, roxo, ciano
	if (!tint && h < 2) tint = pal[(int)setting(h ? "crosshair_color_l" : "crosshair_color_r", h) & 7];
	if (!p) {
		if (g_aim_shown[h]) g_ovl->HideOverlay(g_aim[h]), g_aim_shown[h] = false;
		return;
	}
	// virado para a cabeça: z = ponto -> cabeça, x = cima × z, y = z × x
	const float* hp = g_rpose[0].mDeviceToAbsoluteTracking.m[0];
	float z[3] = {hp[3] - p[0], hp[7] - p[1], hp[11] - p[2]}, n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
	if (n < 1e-3f) return;
	for (float& c : z) c /= n;
	float x[3] = {z[2], 0, -z[0]}, nx = sqrtf(x[0] * x[0] + x[2] * x[2]);
	if (nx < 1e-3f) x[0] = 1, x[2] = 0, nx = 1;
	x[0] /= nx, x[2] /= nx;
	float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
	HmdMatrix34_t t = {{{x[0], y[0], z[0], p[0]}, {x[1], y[1], z[1], p[1]}, {x[2], y[2], z[2], p[2]}}};
	g_ovl->SetOverlayWidthInMeters(g_aim[h], width);
	if (tint) g_ovl->SetOverlayColor(g_aim[h], tint[0], tint[1], tint[2]);
	g_ovl->SetOverlayTransformAbsolute(g_aim[h], ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
	if (!g_aim_shown[h]) g_ovl->ShowOverlay(g_aim[h]), g_aim_shown[h] = true;
	g_aim_pc[h] = present_count();
}

// Curl de cada dedo (polegar, indicador, médio, anelar, mínimo; 0 aberto, 1 fechado) do esqueleto do SteamVR.
bool vr_finger_curl(int hand, float out[5]) {
	if (!g_vr || !g_curl_ok[hand & 1]) return false;
	memcpy(out, g_curl[hand & 1], sizeof g_curl[0]);
	return true;
}

// Grip apertado na mão. SET fake_grip_r/fake_grip_l 1 simula o grip sem headset (testes do menu de armas pelo pad.txt).
bool vr_grip(int hand) { return (g_vr && g_grip_on[hand & 1]) || setting(hand & 1 ? "fake_grip_l" : "fake_grip_r", 0); }

// Pontos do arco da dinamite (até 16 overlays pequenos, laranja, com tamanho angular fixo). No arco do teleporte o
// tint dá a cor (verde = destino válido, vermelho = sem destino); a textura é branca e a cor vem do SetOverlayColor.
void vr_arc(const float* pts, int n, const float* tint) {
	if (!g_vr || !g_ovl) return;
	static uint8_t px[16 * 16 * 4];
	static bool made;
	if (!made && (made = true))
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++) {
				float r = sqrtf((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f));
				uint8_t* q = px + (y * 16 + x) * 4;
				q[0] = q[1] = q[2] = 255, q[3] = r < 6 ? 230 : r < 7.5f ? 90 : 0;
			}
	const float* hp = g_rpose[0].mDeviceToAbsoluteTracking.m[0];
	for (int i = 0; i < 16; i++) {
		if (i >= n) {
			if (i < g_arc_n && g_arc[i]) g_ovl->HideOverlay(g_arc[i]);
			continue;
		}
		if (!g_arc[i]) {
			char key[32];
			snprintf(key, sizeof key, "cojvr.arc%d", i);
			if (g_ovl->CreateOverlay(key, (char*)"CoJ VR arco", &g_arc[i])) continue;
			g_ovl->SetOverlayRaw(g_arc[i], px, 16, 16, 4);
		}
		static const float orange[3] = {1, 0.67f, 0.16f};
		const float* c = tint ? tint : orange;
		g_ovl->SetOverlayColor(g_arc[i], c[0], c[1], c[2]);
		const float* p = pts + i * 3;
		float z[3] = {hp[3] - p[0], hp[7] - p[1], hp[11] - p[2]}, d = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
		if (d < 1e-3f) continue;
		for (float& c : z) c /= d;
		float x[3] = {z[2], 0, -z[0]}, nx = sqrtf(x[0] * x[0] + x[2] * x[2]);
		if (nx < 1e-3f) x[0] = 1, x[2] = 0, nx = 1;
		x[0] /= nx, x[2] /= nx;
		float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
		HmdMatrix34_t t = {{{x[0], y[0], z[0], p[0]}, {x[1], y[1], z[1], p[1]}, {x[2], y[2], z[2], p[2]}}};
		g_ovl->SetOverlayWidthInMeters(g_arc[i], d * (i == n - 1 ? 0.03f : 0.012f));  // o último ponto (impacto) maior
		g_ovl->SetOverlayTransformAbsolute(g_arc[i], ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
		if (i >= g_arc_n) g_ovl->ShowOverlay(g_arc[i]);
	}
	g_arc_n = n, g_arc_pc = present_count();
}

// Menu de armas, no estilo do seletor do HL2VR. Cada item é um overlay com o rótulo desenhado pelo GDI (fundo escuro;
// o item apontado fica laranja e maior), virado para a cabeça, na posição pedida (tracking, m).
static void menu_texture(int slot, VROverlayHandle_t o, const char* text, bool hl) {
	enum { W = LBL_W, H = LBL_H };
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = W, bi.bmiHeader.biHeight = -H;
	bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
	void* bits = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!bm || !bits) return (void)DeleteDC(dc);
	HGDIOBJ ob = SelectObject(dc, bm);
	HFONT f = CreateFontA(46, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
	HGDIOBJ of = SelectObject(dc, f);
	memset(bits, 0, W * H * 4);
	SetBkMode(dc, TRANSPARENT), SetTextColor(dc, RGB(255, 255, 255));
	wchar_t w[64];  // rótulos em UTF-8 ("Revólver"): DrawTextA usaria a página de código ANSI
	MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 64);
	wchar_t* nl = wcschr(w, L'\n');  // "nome\nmunição": nome em cima, munição menor embaixo
	RECT r = {0, 0, W, nl ? H * 6 / 10 : H}, r2 = {0, H * 55 / 100, W, H - 4};
	if (nl) *nl = 0;
	DrawTextW(dc, w, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	if (nl) {
		HFONT f2 = CreateFontA(32, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
		SelectObject(dc, f2), DrawTextW(dc, nl + 1, -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE), SelectObject(dc, f), DeleteObject(f2);
	}
	GdiFlush();
	Label& l = g_lbl[slot];
	uint8_t* px = l.px;
	const uint8_t *b = (const uint8_t*)bits, bg[3] = {(uint8_t)(hl ? 230 : 25), (uint8_t)(hl ? 120 : 22), (uint8_t)(hl ? 20 : 18)};
	for (int i = 0; i < W * H; i++) {
		int x = i % W, y = i / W, t = b[i * 4 + 2];  // texto branco: intensidade em qualquer canal
		bool edge = x < 3 || y < 3 || x >= W - 3 || y >= H - 3;
		for (int c = 0; c < 3; c++) px[i * 4 + c] = (uint8_t)(edge ? 255 : bg[c] + (255 - bg[c]) * t / 255);
		px[i * 4 + 3] = edge ? 200 : 215;
	}
	l.o = o, l.dirty = true;  // label_flush entrega no Present
	SelectObject(dc, of), DeleteObject(f), SelectObject(dc, ob), DeleteObject(bm), DeleteDC(dc);
}

void vr_menu(int n, const char* const* labels, const float* pos, int hl) {
	if (!g_vr || !g_ovl) return;
	const float* hp = g_rpose[0].mDeviceToAbsoluteTracking.m[0];
	for (int i = 0; i < 6; i++) {
		if (i >= n) {
			if (i < g_menu_n && g_menu[i]) g_ovl->HideOverlay(g_menu[i]);
			continue;
		}
		if (!g_menu[i]) {
			char key[32];
			snprintf(key, sizeof key, "cojvr.menu%d", i);
			if (g_ovl->CreateOverlay(key, (char*)"CoJ VR menu", &g_menu[i])) continue;
			g_menu_txt[i][0] = 0;
		}
		char want[40];
		snprintf(want, sizeof want, "%c%s", i == hl ? '*' : ' ', labels[i]);
		if (strcmp(want, g_menu_txt[i])) menu_texture(i, g_menu[i], labels[i], i == hl), strcpy(g_menu_txt[i], want);
		const float* p = pos + i * 3;
		float z[3] = {hp[3] - p[0], hp[7] - p[1], hp[11] - p[2]}, d = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
		if (d < 1e-3f) continue;
		for (float& c : z) c /= d;
		float x[3] = {z[2], 0, -z[0]}, nx = sqrtf(x[0] * x[0] + x[2] * x[2]);
		if (nx < 1e-3f) x[0] = 1, x[2] = 0, nx = 1;
		x[0] /= nx, x[2] /= nx;
		float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
		HmdMatrix34_t t = {{{x[0], y[0], z[0], p[0]}, {x[1], y[1], z[1], p[1]}, {x[2], y[2], z[2], p[2]}}};
		g_ovl->SetOverlayWidthInMeters(g_menu[i], i == hl ? 0.13f : 0.10f);
		g_ovl->SetOverlayTransformAbsolute(g_menu[i], ETrackingUniverseOrigin_TrackingUniverseStanding, &t);
		if (i >= g_menu_n) g_ovl->ShowOverlay(g_menu[i]);
	}
	g_menu_n = n, g_menu_pc = present_count();
}

// Munição no pulso: rótulo no estilo do menu de armas, preso ao controle da mão, nas costas do punho e virado para
// cima (ammo_x/y/z em cm no espaço do controle: x direita, y cima, z para trás; ammo_pitch inclina para os olhos).
// NULL esconde; se não for atualizado no frame, some (vr_frame_pre).
void vr_wrist(int hand, const char* text) {
	if (!g_vr || !g_ovl) return;
	int h = hand & 1;
	if (!text) {
		if (g_wrist[h] && g_wrist_txt[h][0]) g_ovl->HideOverlay(g_wrist[h]), g_wrist_txt[h][0] = 0;
		return;
	}
	if (!g_wrist[h] && g_ovl->CreateOverlay((char*)(h ? "cojvr.ammo1" : "cojvr.ammo0"), (char*)"CoJ VR munição", &g_wrist[h])) return;
	if (strcmp(text, g_wrist_txt[h])) {
		menu_texture(6 + h, g_wrist[h], text, false);
		if (!g_wrist_txt[h][0]) g_ovl->ShowOverlay(g_wrist[h]);
		snprintf(g_wrist_txt[h], sizeof g_wrist_txt[h], "%s", text);
	}
	float a = setting("ammo_pitch", 30) * 0.01745329f, c = cosf(a), sn = sinf(a);
	HmdMatrix34_t t = {{{1, 0, 0, setting("ammo_x", 0) / 100}, {0, sn, c, setting("ammo_y", 3) / 100}, {0, -c, sn, setting("ammo_z", 12) / 100}}};
	g_ovl->SetOverlayWidthInMeters(g_wrist[h], setting("ammo_width", 7) / 100);
	g_ovl->SetOverlayTransformTrackedDeviceRelative(g_wrist[h], hand_device(h), &t);
	g_wrist_pc[h] = present_count();
}

// Escurece a visão pelo compositor (0 = nada, 1 = preto); usado pela coleira da cabeça, pelo teleporte e pela escada.
// Só chama o compositor quando o valor muda.
void vr_fade(float a) {
	static float last;
	if (!g_vr || !g_comp || (fabsf(a - last) < 0.02f && (a > 0 || last == 0))) return;
	last = a;
	q_lock(), g_comp->FadeToColor(0.05f, 0, 0, 0, a, false), q_unlock();
}

// Pad virtual do frame (1 UpdateActionState por Present). Devolve false enquanto nenhum controle VR apareceu; depois,
// sempre true (com tudo solto quando o dashboard tira o foco), para o jogo nunca ver o pad "desconectado".
bool vr_gamepad(XINPUT_GAMEPAD* g) {
	static XINPUT_GAMEPAD cur;
	static bool seen;
	static unsigned pc = ~0u;
	if (!g_in) return false;
	if (present_count() != pc) {
		pc = present_count();
		VRActiveActionSet_t set = {g_set, k_ulInvalidInputValueHandle, 0, 0, 0};
		q_lock();
		EVRInputError e = g_in->UpdateActionState(&set, sizeof set, 1);
		memset(&cur, 0, sizeof cur);
		bool active = false;
		for (int i = 0; !e && i < kNB; i++) {
			InputDigitalActionData_t d;
			if (!g_in->GetDigitalActionData(g_btn[i], &d, sizeof d, k_ulInvalidInputValueHandle) && d.bActive)
				active = true, cur.wButtons |= d.bState ? k_vr_buttons[i].bit : 0;
		}
		for (int i = 0; i < 2; i++) {  // grips: não vão para o pad, o mod decide o que fazem
			InputDigitalActionData_t d;
			g_grip_on[i] = !e && !g_in->GetDigitalActionData(g_grip[i], &d, sizeof d, k_ulInvalidInputValueHandle) && d.bActive && d.bState;
		}
		for (int i = 0; i < 2; i++) {  // curl dos 5 dedos pelo esqueleto do SteamVR (FromAnimation, como no HL2VR)
			VRSkeletalSummaryData_t sd;
			g_curl_ok[i] = !e && g_skel[i] && !g_in->GetSkeletalSummaryData(g_skel[i], EVRSummaryType_VRSummaryType_FromAnimation, &sd);
			if (g_curl_ok[i]) memcpy(g_curl[i], sd.flFingerCurl, sizeof g_curl[i]);
		}
		auto axis = [](float v) { return (SHORT)((v < -1 ? -1 : v > 1 ? 1 : v) * 32767); };
		InputAnalogActionData_t a;
		if (!e && !g_in->GetAnalogActionData(g_move, &a, sizeof a, k_ulInvalidInputValueHandle) && a.bActive)
			active = true, cur.sThumbLX = axis(a.x), cur.sThumbLY = axis(a.y);
		if (!e && !g_in->GetAnalogActionData(g_look, &a, sizeof a, k_ulInvalidInputValueHandle) && a.bActive)
			active = true, cur.sThumbRX = axis(a.x), cur.sThumbRY = axis(a.y);
		if (!e && !g_in->GetAnalogActionData(g_lt, &a, sizeof a, k_ulInvalidInputValueHandle) && a.bActive)
			active = true, cur.bLeftTrigger = (BYTE)(a.x * 255);
		if (!e && !g_in->GetAnalogActionData(g_rt, &a, sizeof a, k_ulInvalidInputValueHandle) && a.bActive)
			active = true, cur.bRightTrigger = (BYTE)(a.x * 255);
		q_unlock();
		static int nerr;
		if (e && nerr++ < 5) logf("controles: UpdateActionState -> %d", e);
		if (active != seen && active) logf("controles: controles VR ativos; o pad virtual passa a vir do SteamVR Input");
		seen |= active;
		static WORD last_buttons;
		if (cur.wButtons != last_buttons) {
			static int n;
			if (n++ < 400) logf("controles: botões %#06x LT %u RT %u", cur.wButtons, cur.bLeftTrigger, cur.bRightTrigger);
			last_buttons = cur.wButtons;
		}
	}
	*g = cur;
	return seen;
}
