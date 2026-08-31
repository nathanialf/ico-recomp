#!/usr/bin/env bash
# Install repo git hooks (currently: the no-ROM pre-commit gate).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
mkdir -p .git/hooks
cat > .git/hooks/pre-commit <<'EOF'
#!/usr/bin/env bash
exec bash tools/check_no_rom.sh
EOF
chmod +x .git/hooks/pre-commit
echo "pre-commit hook installed (tools/check_no_rom.sh)"
