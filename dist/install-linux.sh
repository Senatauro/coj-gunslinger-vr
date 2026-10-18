#!/bin/bash
# CoJ Gunslinger VR installer for Linux (Steam + Proton).
# Makes a copy of the game folder next to it ("CoJ Gunslinger VR", hard links: almost no extra disk), puts the mod in
# the copy and adds "Call of Juarez: Gunslinger VR" to the Steam library. The original game is never modified.
#   ./install-linux.sh              install or update
#   ./install-linux.sh --uninstall  remove the copy and the Steam entry
set -euo pipefail

APP_ID=204450
APP_NAME="Call of Juarez: Gunslinger VR"
COPY_DIR="CoJ Gunslinger VR"
HERE="$(cd "$(dirname "$0")" && pwd)"

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

steam_root() {
	[ -n "${COJVR_STEAM_ROOT:-}" ] && { echo "$COJVR_STEAM_ROOT"; return; }  # set by tools/test_install_linux.py
	local p
	for p in ~/.local/share/Steam ~/.steam/steam ~/.steam/root ~/.var/app/com.valvesoftware.Steam/.local/share/Steam; do
		[ -f "$p/steamapps/libraryfolders.vdf" ] && { realpath "$p"; return; }
	done
	die "Steam not found (looked in ~/.local/share/Steam, ~/.steam and the Flatpak folder)."
}

libraries() {  # the Steam folder itself, then every "path" in libraryfolders.vdf
	{ echo "$ROOT"; sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$ROOT/steamapps/libraryfolders.vdf"; } |
		awk '!seen[$0]++' | while IFS= read -r l; do [ -d "$l/steamapps" ] && echo "$l"; done
}

find_tool() {  # find_tool <folder name in steamapps/common>
	local l
	while IFS= read -r l; do [ -d "$l/steamapps/common/$1" ] && { echo "$l/steamapps/common/$1"; return; }; done < <(libraries)
}

wait_steam_closed() {
	[ -n "${COJVR_TEST:-}" ] && return
	while pgrep -x steam >/dev/null; do
		read -rp $'\nSteam is running. Close it (Steam menu > Exit), then press Enter... '
		sleep 2
	done
}

# --- shortcuts.vdf: Steam's binary VDF (0x00 map, 0x01 string, 0x02 int32, 0x08 end), handled as hex bytes ---
hex() { printf '%s' "$1" | od -An -v -tx1 | tr -d ' \n'; }
le32() { local v=$(( $1 & 0xFFFFFFFF )); printf '%02x%02x%02x%02x' $((v & 255)) $((v >> 8 & 255)) $((v >> 16 & 255)) $((v >> 24 & 255)); }
str_field() { echo "01$(hex "$1")00$(hex "$2")00"; }
int_field() { echo "02$(hex "$1")00$(le32 "$2")"; }

# skip_node <i>: B[i] is the first byte inside a map; prints the index right after its closing 0x08
skip_node() {
	local i=$1 t
	while :; do
		t=${B[i]}; i=$((i + 1))
		[ "$t" = 08 ] && { echo "$i"; return; }
		while [ "${B[i]}" != 00 ]; do i=$((i + 1)); done; i=$((i + 1))  # key
		case $t in
		00) i=$(skip_node "$i") ;;
		01) while [ "${B[i]}" != 00 ]; do i=$((i + 1)); done; i=$((i + 1)) ;;
		02) i=$((i + 4)) ;;
		*) die "unknown entry type $t in shortcuts.vdf; nothing was changed" ;;
		esac
	done
}

# set_shortcut <launcher or ""> <start dir>: removes our entry from every Steam user and adds it again if a launcher is given
set_shortcut() {
	local launcher=$1 start=$2 users=0 cfg f name1 name2 keep entry i s e k body n
	name1="01$(hex appname)00$(hex "$APP_NAME")00" name2="01$(hex AppName)00$(hex "$APP_NAME")00"
	for cfg in "$ROOT"/userdata/*/config; do
		[[ $(basename "$(dirname "$cfg")") =~ ^[1-9][0-9]*$ ]] || continue
		f="$cfg/shortcuts.vdf"
		B=()
		[ -f "$f" ] && { read -ra B < <(od -An -v -tx1 "$f" | tr '\n' ' ') || true; }  # sem newline no fim o read devolve 1; o || true segura o set -e
		keep=()
		if [ ${#B[@]} -gt 0 ]; then
			[ "${B[0]}" = 00 ] || die "could not read $f; nothing was changed"
			i=1; while [ "${B[i]}" != 00 ]; do i=$((i + 1)); done; i=$((i + 1))  # skips the "shortcuts" key
			while [ "${B[i]}" = 00 ]; do  # each entry: 00 <index> 00 <fields> 08
				s=$((i + 1)); while [ "${B[s]}" != 00 ]; do s=$((s + 1)); done; s=$((s + 1))
				e=$(skip_node "$s")
				body=$(IFS=; echo "${B[*]:s:e-s}")
				[[ $body == *"$name1"* || $body == *"$name2"* ]] || keep+=("$body")
				i=$e
			done
		fi
		if [ -n "$launcher" ]; then
			local exe="\"$launcher\"" crc
			crc=$(printf '%s' "$exe$APP_NAME" | gzip -c | tail -c8 | od -An -tu4 -N4 | tr -d ' ')
			entry=$(int_field appid $((crc | 0x80000000)))$(str_field appname "$APP_NAME")$(str_field Exe "$exe")
			entry+=$(str_field StartDir "\"$start\"")$(str_field icon "")$(str_field ShortcutPath "")$(str_field LaunchOptions "")
			entry+=$(int_field IsHidden 0)$(int_field AllowDesktopConfig 1)$(int_field AllowOverlay 0)$(int_field OpenVR 1)
			entry+=$(int_field Devkit 0)$(str_field DevkitGameID "")$(int_field DevkitOverrideAppID 0)$(int_field LastPlayTime 0)
			entry+=$(str_field FlatpakAppID "")$(str_field sortas "")00$(hex tags)000808
			keep+=("$entry")
		fi
		out="00$(hex shortcuts)00"
		n=0
		for k in "${keep[@]}"; do out+="00$(hex $n)00$k"; n=$((n + 1)); done
		out+=0808
		[ -f "$f" ] && cp -p "$f" "$f.cojvr-backup"
		printf "$(sed 's/../\\x&/g' <<<"$out")" > "$f.tmp" && mv "$f.tmp" "$f"  # only \xHH in the format: no NUL in bash strings
		users=$((users + 1))
	done
	echo "$users"
}

put() {  # replaces dst with a fresh file: never writes into an existing file, which could be a hard link to the game
	rm -f "$2"
	cp "$1" "$2"
}

install() {
	local f proton runtime linked
	for f in d3d9.dll cojvr/dxvk_d3d9.dll cojvr/openvr_api.dll; do
		[ -f "$HERE/$f" ] || die "$f is missing next to the installer. Extract the whole zip and run it from there."
	done
	proton=$(find_tool "Proton - Experimental")
	runtime=$(find_tool "SteamLinuxRuntime_4")
	[ -n "$proton" ] && [ -n "$runtime" ] || die "Proton Experimental and the Steam Linux Runtime 4 are needed. Install \"Proton Experimental\" from the Steam library (Tools), start the normal game once with it, then run this again."
	[ -d "$LIB/steamapps/compatdata/$APP_ID/pfx" ] || [ -n "${COJVR_TEST:-}" ] ||
		die "Start the normal game once from Steam (with Proton Experimental), quit it, then run this again."
	[ ! -e "$DEST" ] || [ -f "$DEST/cojvr/.vr_copy" ] || die "$DEST already exists and was not made by this installer; move it away first."

	echo "Copying the game to $DEST (hard links, almost no extra disk)..."
	mkdir -p "$DEST"
	linked=1
	# ponytail: copia item a item do topo da pasta e pula o que já existe, e é isso que faz a atualização. Se a primeira
	# instalação parar no meio, a pasta fica sem o arquivo de marca (cojvr/.vr_copy) e a próxima execução pede para apagá-la.
	for f in "$GAME"/* "$GAME"/.[!.]*; do
		[ -e "$f" ] || continue
		case "$(basename "$f")" in d3d9.dll | cojvr | steam_appid.txt) continue ;; esac
		[ -e "$DEST/$(basename "$f")" ] && continue
		cp -al "$f" "$DEST/" 2>/dev/null || { linked=0; rm -rf "${DEST:?}/$(basename "$f")"; cp -a "$f" "$DEST/"; }
	done
	[ $linked = 1 ] && echo "  done with hard links" || echo "  this disk does not allow hard links: made a normal copy"

	mkdir -p "$DEST/cojvr"
	put "$HERE/d3d9.dll" "$DEST/d3d9.dll"
	(cd "$HERE" && find cojvr -type f) | while IFS= read -r f; do
		mkdir -p "$DEST/$(dirname "$f")"
		[ "$(basename "$f")" = cojvr.ini ] && [ -f "$DEST/$f" ] && continue  # keeps the player's VR options on update
		put "$HERE/$f" "$DEST/$f"
	done
	rm -f "$DEST/steam_appid.txt"
	printf '%s' $APP_ID > "$DEST/steam_appid.txt"
	echo "CoJ Gunslinger VR copy made by install-linux.sh" > "$DEST/cojvr/.vr_copy"

	rm -f "$DEST/cojvr-launch.sh"
	cat > "$DEST/cojvr-launch.sh" <<EOF
#!/bin/sh
# Call of Juarez: Gunslinger VR launcher (made by install-linux.sh). Start SteamVR before the game.
# PROTON_USE_WOW64=1: the 32-bit SteamVR client fails on ext4 (SteamVR-for-Linux #963).
export PROTON_USE_WOW64="\${PROTON_USE_WOW64:-1}"
# No Steam overlay: with it the game hangs at the end of a level load.
export LD_PRELOAD="\$(printf '%s' "\${LD_PRELOAD:-}" | tr ': ' '\\n\\n' | grep -v gameoverlayrenderer | paste -sd: -)"
STEAM="$ROOT"
LIB="$LIB"
GAME="$DEST"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="\$STEAM" STEAM_COMPAT_DATA_PATH="\$LIB/steamapps/compatdata/$APP_ID" \\
	STEAM_COMPAT_INSTALL_PATH="\$GAME" STEAM_COMPAT_LIBRARY_PATHS="\$LIB" STEAM_COMPAT_APP_ID=$APP_ID \\
	SteamAppId=$APP_ID SteamGameId=$APP_ID
CACHE="\$LIB/steamapps/shadercache/$APP_ID"
export STEAM_COMPAT_SHADER_PATH="\$CACHE" STEAM_COMPAT_TRANSCODED_MEDIA_PATH="\$CACHE" STEAM_COMPAT_MEDIA_PATH="\$CACHE/fozmediav1"
cd "\$GAME" || exit 1
exec "$runtime/_v2-entry-point" --verb=waitforexitandrun -- "$proton/proton" waitforexitandrun "\$GAME/CoJGunslinger.exe" "\$@"
EOF
	chmod +x "$DEST/cojvr-launch.sh"

	wait_steam_closed
	local n
	n=$(set_shortcut "$DEST/cojvr-launch.sh" "$DEST")
	printf '\nDone. "%s" was added to Steam (%s user(s)).\n' "$APP_NAME" "$n"
	echo "Start SteamVR first, then the game from Steam. The normal game is unchanged."
}

uninstall() {
	wait_steam_closed
	set_shortcut "" "" >/dev/null
	if [ -f "$DEST/cojvr/.vr_copy" ]; then
		rm -rf "$DEST"  # hard links: removing the copy leaves the original files alone
		echo "Removed $DEST."
	fi
	printf '"%s" was removed from Steam. The normal game is unchanged.\n' "$APP_NAME"
}

ROOT=$(steam_root)
LIB="" GAME=""
while IFS= read -r l; do
	acf="$l/steamapps/appmanifest_$APP_ID.acf"
	[ -f "$acf" ] || continue
	dir=$(sed -n 's/^[[:space:]]*"installdir"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$acf")
	[ -f "$l/steamapps/common/${dir:-CoJ Gunslinger}/CoJGunslinger.exe" ] && { LIB=$l; GAME="$l/steamapps/common/${dir:-CoJ Gunslinger}"; break; }
done < <(libraries)
[ -n "$GAME" ] || die "Call of Juarez: Gunslinger is not installed in any Steam library."
DEST="$LIB/steamapps/common/$COPY_DIR"
[ "$(realpath -m "$DEST")" != "$(realpath "$GAME")" ] || die "the copy would be the game folder itself; nothing was changed."
printf 'Steam: %s\nGame:  %s\n' "$ROOT" "$GAME"
if [ "${1:-}" = --uninstall ]; then uninstall; else install; fi
