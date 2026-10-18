#!/bin/sh
# Teste sem interação: lança o jogo (tools/run.sh), passa da tela inicial e carrega o save pelo pad virtual
# (A na tela inicial → Play Game → Story → Continue → espera o loading → START na tela READY). Requer [dev] pad_script=1
# no cojvr.ini. Não usa "New Game", que sobrescreveria o save.
#   tools/play.sh [extra.txt]   extra.txt (opcional) vai para o pad.txt 10 s depois de entrar no gameplay
#   tools/play.sh menu          para no menu principal
#   tools/play.sh stop          encerra o jogo
G="${COJ_DIR:-$HOME/.local/share/Steam/steamapps/common/CoJ Gunslinger}/cojvr"
LOG="$G/cojvr.log"
if [ "$1" = stop ]; then  # pkill -f mataria também o próprio shell; só o processo do jogo no Wine tem cmdline "S:\..."
	for p in $(pgrep -f CoJGunslinger); do
		case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in S:*) kill "$p" && echo "kill $p";; esac
	done
	exit 0
fi
running() {  # o jogo já está aberto? (mesmo filtro do stop)
	for p in $(pgrep -f CoJGunslinger); do
		case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in S:*) return 0;; esac
	done
	return 1
}
if running; then echo "o CoJ já está aberto: feche antes (tools/play.sh stop)"; exit 1; fi
wait_log() {  # wait_log <padrão> <timeout em s>: espera o padrão aparecer no cojvr.log
	i=0
	until grep -q "$1" "$LOG" 2>/dev/null; do
		i=$((i + 1)); [ $i -gt "$2" ] && { echo "timeout esperando '$1'"; exit 1; }
		sleep 1
	done
}
rm -f "$LOG" "$G/pad.txt"
"$(dirname "$0")/run.sh" > /dev/null 2>&1 &
wait_log 'XInputGetState: 1ª chamada' 120
sleep 20  # vídeos de abertura (a tela inicial roda a ~48 fps e só sai com A)
# A na tela inicial até o menu principal existir; o intervalo longo evita que um A a mais caia no "Play Game".
n=0
until grep -q "nível novo 'MainMenuModule'" "$LOG"; do
	n=$((n + 1)); [ $n -gt 15 ] && { echo "timeout esperando o menu principal"; exit 1; }
	echo 'A 150' > "$G/pad.txt"
	sleep 6
done
echo "menu principal após $n A"
[ "$1" = menu ] && exit 0  # para no menu principal (ex.: ir ao modo Duelo pelo pad.txt)
sleep 2
printf 'A 150\nWAIT 2500\nA 150\nWAIT 2500\nA 150\n' > "$G/pad.txt"
wait_log "nível novo 'BackgroundModuleCJ'" 30  # tela de loading
sleep 20
# START até o nível do jogo existir: o LevelDI só nasce com o START na tela READY. Um START a mais no gameplay
# abriria a pausa, por isso o intervalo de 6 s (o LevelDI aparece < 1 s depois do START).
n=0
until grep -q "nível novo 'LevelDI'" "$LOG"; do
	n=$((n + 1)); [ $n -gt 30 ] && { echo "timeout esperando o gameplay"; exit 1; }
	echo 'START 200' > "$G/pad.txt"
	sleep 6
done
echo "gameplay após $n START"
[ -n "$1" ] && { sleep 10; cp "$1" "$G/pad.txt"; echo "pad extra: $1"; }
exit 0
