#!/bin/sh
# Instala ou remove o mod direto na pasta do jogo, para desenvolvimento ("tools/install.sh uninstall" remove).
# Remover = apagar d3d9.dll e cojvr/, e o jogo volta ao original. COJ_DIR, DXVK_DLL e OPENVR_DLL trocam os caminhos
# padrão do jogo e das DLLs do DXVK (x32) e do OpenVR (win32).
set -e
GAME="${COJ_DIR:-$HOME/.local/share/Steam/steamapps/common/CoJ Gunslinger}"
DXVK="${DXVK_DLL:-$HOME/Projetos/Mods/_decomp/downloads/dxvk_3.1.1/dxvk-3.1.1/x32/d3d9.dll}"
OPENVR="${OPENVR_DLL:-$HOME/Projetos/Mods/_decomp/downloads/openvr_2.15.6/openvr-2.15.6/bin/win32/openvr_api.dll}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$GAME/CoJGunslinger.exe" ] || { echo "jogo não encontrado em $GAME"; exit 1; }
# Sobrescrever a DLL com o jogo aberto troca o código mapeado do processo em execução. Só o processo do jogo dentro do
# Wine tem cmdline "S:\..."; pgrep -f sozinho também pegaria shells e scripts cuja linha de comando cita o jogo.
for p in $(pgrep -f CoJGunslinger); do
	case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in S:*) echo "o CoJ está aberto: feche antes de instalar"; exit 1;; esac
done

if [ "$1" = uninstall ]; then
	rm -f "$GAME/d3d9.dll"
	rm -rf "$GAME/cojvr"
	echo "removido de $GAME"
	exit 0
fi

cp "$ROOT/build/d3d9.dll" "$GAME/d3d9.dll"
mkdir -p "$GAME/cojvr"
cp "$DXVK" "$GAME/cojvr/dxvk_d3d9.dll"
cp "$OPENVR" "$GAME/cojvr/openvr_api.dll"
mkdir -p "$GAME/cojvr/steamvr"
cp "$ROOT"/cojvr/steamvr/*.json "$GAME/cojvr/steamvr/"
cp "$ROOT/cojvr/vr_texts.txt" "$GAME/cojvr/vr_texts.txt"  # sempre sobrescreve: vale a do repositório
[ -f "$GAME/cojvr/cojvr.ini" ] || cp "$ROOT/cojvr/cojvr.ini" "$GAME/cojvr/cojvr.ini"  # só na 1ª vez: mantém os ajustes
echo "instalado em $GAME"
