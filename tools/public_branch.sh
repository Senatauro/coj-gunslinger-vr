#!/bin/bash
# Gera (ou atualiza) a branch "public", a que vai para o GitHub, a partir do HEAD sem tocar na pasta de trabalho:
# o mesmo conteúdo menos a documentação interna (docs/, knowledge/, MODLOG.md, STATUS.md), com o README do pacote na
# raiz e um .gitignore que mantém essas pastas fora. A 1ª vez cria a branch sem histórico; as seguintes empilham um
# commit novo sobre a anterior.
#   tools/public_branch.sh "mensagem do commit"
set -euo pipefail
cd "$(dirname "$0")/.."
msg=${1:?mensagem do commit}
export GIT_INDEX_FILE=$(mktemp)
trap 'rm -f "$GIT_INDEX_FILE"' EXIT
git read-tree HEAD
git rm --cached -r -q --ignore-unmatch docs knowledge MODLOG.md STATUS.md
readme=$(git hash-object -w dist/README.md)
ignore=$(printf 'build/\n.neuralmind/\ndocs/\nknowledge/\nMODLOG.md\nSTATUS.md\n' | git hash-object -w --stdin)
git update-index --add --cacheinfo "100644,$readme,README.md" --cacheinfo "100644,$ignore,.gitignore"
tree=$(git write-tree)
parent=()
git rev-parse -q --verify refs/heads/public >/dev/null && parent=(-p public)
[ ${#parent[@]} -gt 0 ] && [ "$(git rev-parse public^{tree})" = "$tree" ] && { echo "public já está igual ao HEAD"; exit 0; }
commit=$(git commit-tree "$tree" "${parent[@]}" -m "$msg")
git update-ref refs/heads/public "$commit"
echo "public -> $(git log -1 --format='%h %s' public)"
git ls-tree -r --name-only public | sed 's|/.*||' | sort | uniq -c
