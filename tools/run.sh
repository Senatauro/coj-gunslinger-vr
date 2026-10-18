#!/bin/sh
# Lança o jogo pelo Proton sem passar pelo "Jogar" da Steam. Com o SteamVR ativo, o lançamento pela Steam liga o
# Desktop Game Theatre (a Steam atual não tem mais opção para desligá-lo), que derruba a Steam ou encerra o jogo.
# A Steam ainda precisa estar aberta (DRM e steam_api).
# WoW64: o cliente SteamVR de 32 bits (bin/vrclient.so) não inicializa o renderer Vulkan em ext4 (SteamVR-for-Linux #963);
# no modo WoW64 a ponte do Proton usa o cliente de 64 bits. PROTON_USE_WOW64=0 tools/run.sh volta ao modo clássico.
export PROTON_USE_WOW64="${PROTON_USE_WOW64:-1}"
STEAM="${STEAM_DIR:-$HOME/.local/share/Steam}"
GAME="${COJ_DIR:-$STEAM/steamapps/common/CoJ Gunslinger}"
PROTON="${PROTON_DIR:-$STEAM/steamapps/common/Proton - Experimental}"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM" STEAM_COMPAT_DATA_PATH="$STEAM/steamapps/compatdata/204450" \
	STEAM_COMPAT_INSTALL_PATH="$GAME" STEAM_COMPAT_LIBRARY_PATHS="$STEAM" STEAM_COMPAT_APP_ID=204450 \
	SteamAppId=204450 SteamGameId=204450
# Cache de shaders e de vídeos convertidos (transcoded_video.foz): os mesmos caminhos que o "Jogar" da Steam passa.
CACHE="$STEAM/steamapps/shadercache/204450"
export STEAM_COMPAT_SHADER_PATH="$CACHE" STEAM_COMPAT_TRANSCODED_MEDIA_PATH="$CACHE" STEAM_COMPAT_MEDIA_PATH="$CACHE/fozmediav1"
cd "$GAME" || exit 1
# Runtime: o que o toolmanifest.vdf do Proton pede (require_tool_appid 4183110 = SteamLinuxRuntime_4, igual ao "Jogar").
# Com o runtime sniper, o winegstreamer falha ao abrir os .wmv e o jogo pula todos os vídeos.
exec "$STEAM/steamapps/common/${STEAM_RUNTIME_DIR:-SteamLinuxRuntime_4}/_v2-entry-point" --verb=waitforexitandrun -- \
	"$PROTON/proton" waitforexitandrun "$GAME/CoJGunslinger.exe" "$@"
