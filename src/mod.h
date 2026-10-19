#pragma once
#include <windows.h>
#include <stdint.h>
#include <xinput.h>

// Versão do mod. O build do GitHub passa a versão da release (make VERSION=0.1.N); este é o padrão do build local.
#ifndef COJVR_VERSION
#define COJVR_VERSION "0.1.0"
#endif

// Endereços do CoJGunslinger.exe da Steam, versão 1.0.5.0, sempre carregado na base 0x400000. São absolutos:
// o mod_init (mod.cpp) confere a base, o cabeçalho e o hash do .text e desliga o mod se algo não bater.
namespace game {
constexpr uintptr_t base = 0x00400000;
constexpr DWORD timestamp = 0x52C6D131, size_of_image = 0x02FA3000, text_vsize = 0x00E56D6A;
constexpr uint64_t text_fnv1a64 = 0x9f86b36332ed6286ULL;  // FNV-1a 64 do .text inteiro (VirtualSize bytes)

constexpr uintptr_t CRenderer_vf9_slot = 0x012E15D8;  // vtable 0x012E15B4 + 9*4 -> 0x007EA080 (Present do engine)
constexpr uintptr_t iat_XInputGetState = 0x01258750;
constexpr uintptr_t present_counter = 0x02E658D8;     // u32, incrementado a cada Present
constexpr uintptr_t d3d9_module = 0x02E65FEC;         // HMODULE gravado por RCreateRenderer
constexpr uintptr_t device = 0x02E6461C;              // IDirect3DDevice9*

// Estéreo (stereo.cpp)
constexpr uintptr_t CLevel_vf161_slot = 0x0129F6B0;   // -> 0x004D5F20, roda o frame_script (thiscall, sem args)
constexpr uintptr_t CLevel_vf162_slot = 0x0129F6B4;   // -> 0x004D5F50, render das views (thiscall, sem args)
constexpr uintptr_t CRenderer_vf41_slot = 0x012E1658; // -> 0x007EAEA0, lock do renderer (thiscall, 1 arg, ret 4)
constexpr uintptr_t render_task_imm = 0x0040BD33;     // imm32 do "push 0x0040B0E0" que agenda o RenderTask
constexpr uintptr_t render_task = 0x0040B0E0;         // S_CGame__RenderTask, void __cdecl(void*)
constexpr uintptr_t view_list_head = 0x004C2F10;      // char* __stdcall(CLevel+0x600, char onlyEnabled); TLS 2 (render) = lista da cópia de render
constexpr uintptr_t cgame = 0x0165D380;               // CGame*: +0x98 editor, +0xAC frame, +0x9A4/+0x9A5 FramePipelining (config/efetivo)
constexpr uintptr_t crenderer = 0x02E64100;           // CRenderer*: +0x178 lock ativo
constexpr uintptr_t loading_thread = 0x02E666FC;      // HANDLE da BackgroundRenderProc (thread de loading)
constexpr uintptr_t cull_freeze = 0x02E6575D;         // byte: congela o culling (esperado 0)
constexpr uintptr_t nv_stereo = 0x02E6600C;           // objeto do estéreo da NVIDIA (esperado nulo)
}

// log.cpp
void log_open(const char* path);
void logf(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

// proxy.cpp
extern HMODULE g_self;
extern char g_moddir[MAX_PATH];  // "<jogo>\cojvr\"
int cfg_int(const char* sec, const char* key, int def);
float cfg_float(const char* sec, const char* key, float def);
void cfg_str(const char* sec, const char* key, const char* def, char* out, int n);

// vr.cpp
// Convenções: "tracking" é o espaço do SteamVR (metros, −Z = frente); poses são 3x4 row-major.
extern bool g_vr;                   // OpenVR ativo
extern int g_eye_w, g_eye_h;        // tamanho recomendado do olho (0 sem VR)
void hook_reset();                  // log do IDirect3DDevice9::Reset (solta as texturas do mod antes)
struct IDirect3DSurface9* vr_eye_surface(int eye, UINT w, UINT h);  // textura do olho (cria/recria no tamanho)
bool vr_eye_to_head(int eye, float m[12]);  // GetEyeToHeadTransform (3x4 row-major, metros); false sem VR
bool vr_hmd_pose(bool update, float m[12]);  // pose do HMD de update ("game" do WaitGetPoses anterior) ou de render; false sem VR
bool vr_hand_pose(int hand, bool update, float m[12]);  // pose do controle (0 = direito, 1 = esquerdo), crua, no tracking
void vr_haptic(int hand, float sec, float hz, float amp);  // vibração no controle da mão
void vr_reticle(int hand, const float* trk, float width, const float* tint = NULL);  // mira no tracking (m); NULL esconde; hand 2 = alvo do "usar", 3 = mira pela cabeça (duelo)
void vr_menu(int n, const char* const* labels, const float* pos, int hl);  // menu de armas (tracking, m), n ≤ 6
void vr_wheel(const float* pos, float width, const float* uv);  // roda de armas do jogo (recorte do HUD) no tracking
void vr_hud_band(int* b);  // faixa da UI (w, h, x, y) e tamanho da textura do HUD (W, H)
void vr_vignette(float target);  // vinheta de conforto (0..1; 0 esconde), suavizada em ~0,15 s
void vr_wrist(int hand, const char* text);  // munição no pulso da mão (0 = direita); NULL esconde
void vr_arc(const float* pts, int n, const float* tint = NULL);         // pontos do arco (dinamite, teleporte) no tracking (m), n ≤ 16; 0 esconde
bool vr_finger_curl(int hand, float out[5]);  // curvatura dos dedos pelo esqueleto do SteamVR (0 = mão direita)
bool vr_grip(int hand);                       // grip do controle (0 = direito) apertado
bool vr_eye_tangents(int eye, float& l, float& r, float& b, float& t);  // frustum do olho no plano z = -1
extern volatile LONG g_vr_recenter; // 1 = reposicionar a tela no próximo frame
void vr_init();                     // antes do DXVK
void vr_frame_pre();                // no Present do jogo, antes do Present do D3D9
void vr_frame_post();               // depois do Present do D3D9
void vr_render_task_pre(bool game_frame);   // começo do RenderTask: WaitGetPoses + SubmitExplicitTimingData (timing explícito)
void vr_render_task_post(bool game_frame);  // fim do RenderTask: WaitGetPoses com a fila do DXVK travada (timing implícito)
bool vr_eyes_allowed();             // o laço de olhos pode rodar neste frame (houve WaitGetPoses)
void vr_report();                   // a cada 5 s

// stats.cpp
double qpc_ms(LONGLONG ticks);                // ticks do QPC -> ms
void stats_add(const char* name, double ms);  // name: literal
void stats_report();                          // a cada 5 s, do Present

// video.cpp
bool video_eye_mode();                        // VR ativo ou [stereo] fake_hmd=1: o jogo roda no tamanho do olho
void video_init();                            // hooks do Apply do modo de vídeo e do gravador do Video.scr; chamado no mod_init
void video_report();                          // a cada 5 s

// pose.cpp: pose do HMD no mundo do jogo
struct PoseState {
	float psi0;        // yaw de calibração (graus, convenção do jogo)
	float cx, cz;      // centro de calibração no chão do tracking (m)
	float height;      // altura da cabeça em pé no recentrar (cm)
	bool calibrated;
};
PoseState& pose_state();
void pose_to_game(const float hmd[12], const float anchor[3], float cam[12]);  // HMD (tracking) -> câmera do jogo
void game_angles(const float cam[12], float& psi, float& theta);              // ψ, θ (graus) da câmera
void pose_recenter(const float hmd[12], float psi_player);
void pose_turn(const float hmd[12], float delta);                              // giro com pivô na cabeça
void fake_hmd_pose(float m[12]);                                               // pose sintética sem HMD (SET fake_*)
int pose_selftest();

// player.cpp: jogador, cabeça e modos CENA (3D com a cabeça) e TELA (tela plana no headset)
void player_init();                           // hooks do jogador e da câmera de update
void focus_install(HWND hwnd);                // subclasse da WndProc: o jogo não perde o foco com o VR
void focus_frame();                           // a cada Present
bool vr_gamepad(XINPUT_GAMEPAD* g);           // vr.cpp: pad virtual dos controles VR (false = nenhum controle VR ainda)
void vr_fade(float a);                        // vr.cpp: escurece a visão (FadeToColor do compositor); usado pela coleira
void player_update_end(char* lvl);            // fim do update do LevelDI (depois do frame_script)
bool player_scene();                          // CENA liberada (laço de olhos e cabeça)
const char* player_tela_reason();             // motivo para estar em TELA (NULL = nenhum)
bool player_rstick(float x);                  // stick direito para o giro do mod (true = o jogo não recebe)
float player_dodge();                         // Sense of Death: −1/0/+1, lado da esquiva pela cabeça
bool player_paused();                         // pausa (estados 7–9) com o 3D atrás
bool head_render(float cam[12]);              // câmera da cabeça para o render (pose de render do WaitGetPoses + âncora)
char* player_local();                         // PlayerDI do jogador local (NULL sem os hooks do jogador)
bool player_head_frame();                     // a cabeça foi aplicada neste frame ou no anterior
const float* player_head_cam();               // câmera da cabeça no update (3x4, mundo do jogo)
const float* player_hmd();                    // pose do HMD no update (3x4, tracking) que gerou essa câmera
const float* player_anchor();                 // âncora (pé) lida depois da câmera FPP (CameraFPPDI::vf8)
void player_mark(char c);                     // diagnóstico: anota uma letra na ordem de eventos do frame
void player_face(float psi);                  // vira a frente da cabeça para o yaw psi (graus) no próximo frame
void player_to_game(const float trk[12], float out[12]);  // pose do tracking -> mundo do jogo (como a cabeça)
const char* player_order();                   // diagnóstico: ordem de eventos do último frame completo

// weapon.cpp: armas e mãos
void weapon_init();                           // hook do PlayerFppVis::vf110; só com os hooks do jogador
void weapon_frame();                          // antes do OnFrame, com a cabeça aplicada: pose das mãos do frame
void weapon_camera(bool paused = false);      // depois da câmera FPP, com a cabeça aplicada (âncora final do frame)
void weapon_paused();                         // mesmo ponto, na pausa em 3D (tela do fim do duelo)
bool weapon_muzzle(float out[3]);             // cano da mão do Shoot em curso (false fora do Shoot do jogador)
bool weapon_barrel(int k, float pos[3], float dir[3]);  // cano e eixo da arma da mão lógica k no frame
float weapon_trigger(int k);                  // gatilho físico da arma da mão lógica k (0 sem arma)
bool weapon_use_dir(float out[3]);            // cabeça → mão livre estendida (false: fica o olhar)
void weapon_probe();                          // diagnóstico da arma no coldre do duelo no frame do SHOT
bool weapon_hand_body(int h, float o[3]);     // mão física h no frame do corpo (cm), false sem pose
bool weapon_hand_trk(int h, float o[3]);      // mão física h no tracking (cm), false sem pose
const float* weapon_hand_game(int h);         // mão física h no mundo do jogo (3x4, cm), NULL sem pose
void weapon_pad(LONG* cur);                   // grip, gatilho e especial da dinamite no pad virtual

// duel.cpp: duelo (e algumas funções de outros arquivos usadas por ele)
void duel_init();                             // hooks do duelo (só com os hooks do jogador)
void duel_frame(char* p);                     // log dos estados do duelo, no fim do update
void duel_tutorial_log(char* p, int path);    // log da dica do duelo depois da câmera FPP (caminho das mãos)

// gatling.cpp: gatling (StationaryCannon) em VR
bool gatling_on(char* p);                     // montado na gatling (modos 14/15) com a cabeça aplicada
void gatling_anchor(char* p, const float* cam, float a[3]);  // âncora pela câmera do jogo montado na gatling
void gatling_lookdir(char* p, float out[3]);  // mira lida pelo controller da gatling (a da mão de apoio)
float gatling_fire(char* p);                  // gatilho (ação 161) pela manivela; −1 fora da gatling
bool gatling_muzzle(char* p, float o[3], float d[3]);  // boca e direção da gatling para o Shoot; false fora dela
void gatling_frame(char* p);                  // mira, manivela, saída e ponto de mira, no fim do update
void qte_frame(char* p);                      // qte.cpp: QTE em VR (o botão certo vai sozinho), no fim do update
void gatling_camera(char* p);                 // depois da câmera FPP: mãos visíveis montado; a manivela segue a mão
bool gatling_grip(int side, const float* T, float out[12]);  // Holder da mão física side presa no cabo da manivela
bool ladder_busy();                          // player.cpp: escada automática em curso (bloqueia o usar)
float ladder_fade();                         // player.cpp: escuro da escada automática (0..1)
float ladder_action(int a);                  // player.cpp: valor das ações 80/81 (subir/descer) na escada automática; −1 = o jogo decide
bool teleport_pad(LONG* cur);                // weapon.cpp: modo teleporte (stick esquerdo mira o arco); true = o mod cuida do stick
void teleport_frame(char* p);                // weapon.cpp: antes do OnFrame, executa o teleporte pedido (SetPosition do ator)
float teleport_fade();                       // weapon.cpp: piscada preta do teleporte (0..1), somada à coleira
void teleport_request(const float pos[3]);   // weapon.cpp: teste, teleporte direto para um ponto do chão (pad.txt TP)
bool duel_active(char* p);                    // duelo em VR em curso (controlador ativo e vis do duelo)
bool duel_body(char* p);                      // vis do duelo no jogador (também na morte e na tela do fim)
bool duel_bone_pos(int idx, float o[3]);      // weapon.cpp: posição atual de um osso do corpo do duelo
bool actor_bone_pos(char* actor, const char* name, float o[3]);  // weapon.cpp: posição de um osso de um ator, pelo nome
void game_to_trk(const float* P, float* t);  // weapon.cpp: mundo do jogo (cm) -> tracking (m)
bool duel_gun_holstered(char* p);             // arma do duelo ainda no coldre (antes do saque)
int duel_phase(char* p);                      // fase do DuelController no duelo em VR (0 fora dele)

// tutorial.cpp
void tutorial_init();                         // hooks das dicas (mostrar, esconder, ação) e dos textos por chave
bool tut_paused();                            // dica com pausa na tela
bool tut_expects(int action);                 // alguma dica na tela espera a ação (_ACTION_*)
void tut_action(int action);                  // o mod fez a ação por chamada direta: conclui a dica que a espera

// stereo.cpp
float setting(const char* key, float def);    // [stereo] do INI, sobrescrito em tempo real por SET (pad.txt)
void setting_set(const char* key, float val);
void stereo_init();
void stereo_present();                        // começo do Present do engine
void stereo_report();                         // a cada 5 s
bool stereo_frame();                          // o laço de olhos rodou neste frame

// mod.cpp
extern bool g_version_ok;
extern volatile LONG g_shot_request;  // SHOT: 1 = pedido, 2 = olhos gravados, falta o backbuffer (Present)
void dump_surface(struct IDirect3DSurface9* src, const char* tag);  // cojvr\shot_N<tag>.bmp (N avança no Present)
void mod_init();
void* patch_ptr(uintptr_t addr, void* val);  // troca um ponteiro (vtable/IAT) e devolve o antigo
bool patch_call(uintptr_t site, uintptr_t expect, void* to);  // weapon.cpp: troca o destino de um call E8 (só se ele ainda for expect)

// options.cpp
void options_init();  // opções de VR na lista de Opções → Jogo do próprio jogo

// release.cpp
extern char g_update_tag[32];        // tag da release mais nova que a instalada ("" = nenhuma)
void release_init();                 // placar online desligado em VR e checagem de versão em segundo plano
void open_page(const char* which);   // "report" ou "update": abre a página no navegador do computador
