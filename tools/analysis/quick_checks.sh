#!/usr/bin/env bash
# The analysis tools that need no special build, one result file per tool in <out dir> (default
# ${XDG_CACHE_HOME:-~/.cache}/lwe-analysis/<date>). Runs next to each other and never touches the repos' files; the semgrep, gitleaks,
# osv-scanner and scorecard steps need network access.
#
#   tools/analysis/quick_checks.sh [out dir]
set -u

ENGINE=$(cd "$(dirname "$0")/../.." && pwd)
MANAGER=${WE_MANAGER_DIR:-$ENGINE/../we_manager}
OUT=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/lwe-analysis/$(date +%F)}
GLOBAL_MODULES=$(npm root -g)
mkdir -p "$OUT"

run () {
    local name=$1
    shift
    echo "start $name"
    "$@" > "$OUT/$name.txt" 2>&1
    echo "$? $name" >> "$OUT/exit-codes.txt"
}

: > "$OUT/exit-codes.txt"

cat > "$OUT/eslint.config.mjs" <<CONFIG
import tseslint from '$GLOBAL_MODULES/typescript-eslint/dist/index.js';
export default tseslint.config(...tseslint.configs.strictTypeChecked, {
  languageOptions: { parserOptions: { projectService: true, tsconfigRootDir: '$MANAGER' } }
},
{ rules: { '@typescript-eslint/restrict-template-expressions': ['error', { allowNumber: true }], '@typescript-eslint/no-unused-vars': ['error', { varsIgnorePattern: '^_', argsIgnorePattern: '^_', ignoreRestSiblings: true }], '@typescript-eslint/no-dynamic-delete': 'off', '@typescript-eslint/no-invalid-void-type': 'off' } },
{ files: ['**/*.test.ts', '**/*.test.tsx'], rules: Object.fromEntries(['require-await', 'no-unsafe-return', 'no-unsafe-call', 'no-unsafe-argument', 'no-unsafe-assignment', 'no-unsafe-member-access', 'no-non-null-assertion', 'no-explicit-any', 'no-implied-eval', 'no-unnecessary-type-conversion'].map((r) => ['@typescript-eslint/' + r, 'off'])) });
CONFIG

(
    cd "$ENGINE" || exit
    run cppcheck cppcheck -j8 --std=c++20 --enable=warning,performance,portability --inconclusive --max-configs=2 \
        --suppress=missingIncludeSystem -isrc/External -Isrc src
    run gitleaks-engine gitleaks git --redact --no-banner .
    run semgrep-engine semgrep scan --metrics=off --config p/default --exclude src/External --exclude 'build*' src tools
    run osv-engine osv-scanner scan source -r .
    run scorecard-engine scorecard --repo=github.com/WallpaperEngineLover/linux-wallpaperengine-kde
    run checksec checksec --file=build/output/linux-wallpaperengine
) &

(
    cd "$MANAGER" || exit
    run gitleaks-manager gitleaks git --redact --no-banner .
    run semgrep-manager semgrep scan --metrics=off --config p/default --config p/typescript --exclude node_modules --exclude dist --exclude out src
    run osv-manager osv-scanner scan source -r .
    run typecheck npm run typecheck
    run eslint-strict eslint -c "$OUT/eslint.config.mjs" 'src/**/*.{ts,tsx}'
    run knip knip --no-progress
    run madge-circular madge --circular --extensions ts,tsx --ts-config tsconfig.web.json src
    run electronegativity electronegativity -i src
    run npm-audit npm audit
) &

wait
echo done
