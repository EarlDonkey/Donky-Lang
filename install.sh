#!/usr/bin/env bash
# install.sh — build and install Donky from source on Linux and macOS.
#
# Usage:
#   ./install.sh                 install to /usr/local (requires root or sudo)
#   ./install.sh --prefix DIR    install to DIR (e.g. ~/.local)
#   ./install.sh --root          allow running as root
#   ./install.sh --uninstall     remove files installed by a previous run
#   ./install.sh --help
#
# Layout (matches Windows installer, and the path main.rs's Windows branch
# expects if a POSIX branch is ever added: {exe_dir}/../lib/donky/runtime.o):
#   $PREFIX/bin/donky
#   $PREFIX/lib/donky/runtime.o              (reference copy; the binary has the C source embedded)
#   $PREFIX/lib/donky/runtime_template.c     (same source, for debugging)
#   $PREFIX/lib/donky/lib/*.donk             (standard library)
#   $PREFIX/lib/donky/examples/              (skipped if > 50 MB, matching build.ps1)
#
# IMPORTANT — `donky build-exe` writes runtime.c / *.obj into the CWD.
# Run it from a directory you own (never from /usr or /usr/local).
#
# Requires: bash, cargo (Rust), gcc (or clang), git, libffi dev headers.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
PREFIX="/usr/local"
ALLOW_ROOT=0
UNINSTALL=0
SKIP_SMOKE=0
EXAMPLES_MAX_BYTES=$((50 * 1024 * 1024))

# ---------------------------------------------------------------------------
# Colours (disabled if not a TTY)
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_CYN=$'\033[36m'; C_RST=$'\033[0m'
else
    C_RED=''; C_GRN=''; C_YEL=''; C_CYN=''; C_RST=''
fi

say()  { printf '%s\n' "$*"; }
step() { printf '\n%s==> %s%s\n' "$C_CYN" "$*" "$C_RST"; }
warn() { printf '%sWARNING: %s%s\n' "$C_YEL" "$*" "$C_RST" >&2; }
die()  { printf '%sERROR: %s%s\n' "$C_RED" "$*" "$C_RST" >&2; exit 1; }

usage() {
    sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)    [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift 2 ;;
        --prefix=*)  PREFIX="${1#--prefix=}"; shift ;;
        --root)      ALLOW_ROOT=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        --skip-smoke) SKIP_SMOKE=1; shift ;;
        --help|-h)   usage ;;
        *)           die "unknown argument: $1 (try --help)" ;;
    esac
done

# Normalise PREFIX: strip trailing slashes, expand a leading ~/, refuse dangerous values.
case "$PREFIX" in
    "~")   PREFIX="$HOME" ;;
    "~/"*) PREFIX="$HOME/${PREFIX#\~/}" ;;
esac
PREFIX="${PREFIX%/}"
[ -n "$PREFIX" ] || die "refusing to install to empty prefix"
case "$PREFIX" in
    /|/bin|/lib|/usr/bin|/usr/lib)
        die "refusing to install to $PREFIX — pick /usr/local or a directory under \$HOME" ;;
esac

# ---------------------------------------------------------------------------
# Locate repo root (directory that contains Cargo.toml)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=""
for cand in "$SCRIPT_DIR" "$SCRIPT_DIR/donky-native" "$(pwd)" "$(pwd)/donky-native"; do
    if [ -f "$cand/Cargo.toml" ]; then ROOT="$cand"; break; fi
done
[ -n "$ROOT" ] || die "could not find Cargo.toml (looked in $SCRIPT_DIR and $(pwd))"
cd "$ROOT"
PARENT_DIR="$(dirname "$ROOT")"

# ---------------------------------------------------------------------------
# OS / arch detection
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s)"
case "$UNAME_S" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    *)      die "unsupported OS: $UNAME_S (this script supports Linux and macOS; use build.ps1 on Windows)" ;;
esac
ARCH="$(uname -m)"

# ---------------------------------------------------------------------------
# Preflight: root
# ---------------------------------------------------------------------------
if [ "$(id -u)" -eq 0 ]; then
    if [ "$ALLOW_ROOT" -ne 1 ] && [ "$UNINSTALL" -eq 0 ]; then
        die "refusing to run as root. Pass --root to override, or use --prefix \"\$HOME/.local\"."
    fi
fi

# ---------------------------------------------------------------------------
# Preflight: required tools
# ---------------------------------------------------------------------------
need_tool() {
    command -v "$1" >/dev/null 2>&1
}

step "Preflight: required tools"

MISSING=0
for t in cargo gcc git; do
    if ! need_tool "$t"; then
        warn "$t not found on PATH"
        MISSING=1
    fi
done

if [ "$MISSING" -eq 1 ]; then
    say ""
    case "$OS" in
        linux)
            if command -v apt-get >/dev/null 2>&1; then
                say "Install on Debian/Ubuntu:"
                say "  sudo apt-get update"
                say "  sudo apt-get install -y build-essential pkg-config libffi-dev git curl"
                say "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
                say "  source \"\$HOME/.cargo/env\""
            elif command -v dnf >/dev/null 2>&1; then
                say "Install on Fedora/RHEL:"
                say "  sudo dnf install -y gcc make git libffi-devel"
                say "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
            elif command -v pacman >/dev/null 2>&1; then
                say "Install on Arch:"
                say "  sudo pacman -S --needed base-devel git libffi rust"
            elif command -v zypper >/dev/null 2>&1; then
                say "Install on openSUSE:"
                say "  sudo zypper install -y gcc make git libffi-devel rust cargo"
            else
                say "Install cargo (Rust), gcc, git and libffi-devel with your package manager."
            fi
            ;;
        macos)
            say "Install on macOS (Homebrew):"
            say "  xcode-select --install                       # provides gcc/clang and git"
            say "  brew install libffi pkg-config rust"
            say "  # add libffi to the compiler search path:"
            say "  export PKG_CONFIG_PATH=\"\$(brew --prefix libffi)/lib/pkgconfig:\$PKG_CONFIG_PATH\""
            say "  export LDFLAGS=\"-L\$(brew --prefix libffi)/lib \$LDFLAGS\""
            say "  export CPPFLAGS=\"-I\$(brew --prefix libffi)/include \$CPPFLAGS\""
            ;;
    esac
    die "missing required tools (see above)"
fi

# ---------------------------------------------------------------------------
# Preflight: libffi headers
# ---------------------------------------------------------------------------
step "Preflight: libffi headers (ffi.h)"

# Try pkg-config first, then well-known locations, then ask gcc to find it.
FFI_OK=0
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libffi 2>/dev/null; then
    FFI_OK=1
else
    for d in /usr/include /usr/local/include /usr/include/*/ffi.h /opt/homebrew/include /opt/homebrew/opt/libffi/include; do
        if [ -f "$d/ffi.h" ] || [ "$(basename "$d")" = "ffi.h" ]; then FFI_OK=1; break; fi
    done
fi
if [ "$FFI_OK" -eq 0 ]; then
    # Fall back: ask the compiler to preprocess a one-line include.
    TMP_C="$(mktemp "${TMPDIR:-/tmp}/donky-ffi-XXXXXX.c")"
    printf '#include <ffi.h>\nint main(void){return 0;}\n' > "$TMP_C"
    if gcc -E "$TMP_C" >/dev/null 2>&1; then FFI_OK=1; fi
    rm -f "$TMP_C"
fi
if [ "$FFI_OK" -eq 0 ]; then
    say ""
    case "$OS" in
        linux)
            if command -v apt-get >/dev/null 2>&1; then
                say "Install libffi development headers:"
                say "  sudo apt-get install -y libffi-dev"
            elif command -v dnf >/dev/null 2>&1; then
                say "  sudo dnf install -y libffi-devel"
            elif command -v pacman >/dev/null 2>&1; then
                say "  sudo pacman -S libffi"
            elif command -v zypper >/dev/null 2>&1; then
                say "  sudo zypper install libffi-devel"
            else
                say "Install the libffi development package (libffi-dev / libffi-devel)."
            fi
            ;;
        macos)
            say "Install libffi and point the toolchain at it:"
            say "  brew install libffi pkg-config"
            say "  export PKG_CONFIG_PATH=\"\$(brew --prefix libffi)/lib/pkgconfig:\$PKG_CONFIG_PATH\""
            say "  export LDFLAGS=\"-L\$(brew --prefix libffi)/lib \$LDFLAGS\""
            say "  export CPPFLAGS=\"-I\$(brew --prefix libffi)/include \$CPPFLAGS\""
            ;;
    esac
    die "libffi headers not found"
fi
say "  libffi: OK"

# ---------------------------------------------------------------------------
# Uninstall path
# ---------------------------------------------------------------------------
if [ "$UNINSTALL" -eq 1 ]; then
    step "Uninstalling from $PREFIX"
    for p in "$PREFIX/bin/donky" "$PREFIX/lib/donky"; do
        if [ -e "$p" ] || [ -L "$p" ]; then
            rm -rf "$p"
            say "  removed $p"
        else
            say "  not present: $p"
        fi
    done
    say ""
    say "Done." 
    exit 0
fi

# ---------------------------------------------------------------------------
# Build the Rust compiler
# ---------------------------------------------------------------------------
step "Building donky-native (cargo build --release)"
if ! (cd "$ROOT" && cargo build --release); then
    die "cargo build failed"
fi
CARGO_EXE="$ROOT/target/release/donky-native"
[ -x "$CARGO_EXE" ] || die "cargo succeeded but $CARGO_EXE was not produced (did the package name change?)"

# ---------------------------------------------------------------------------
# Compile the C runtime (reference copy; the shipped binary embeds the source)
# ---------------------------------------------------------------------------
step "Compiling C runtime (runtime.o)"
RUNTIME_SRC=""
for cand in "$ROOT/src/runtime_template.c" "$ROOT/runtime_template.c"; do
    if [ -f "$cand" ]; then RUNTIME_SRC="$cand"; break; fi
done
[ -n "$RUNTIME_SRC" ] || die "runtime_template.c not found (looked in src/ and repo root)"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/donky-install-XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

RUNTIME_OBJ="$WORK_DIR/runtime.o"
EXTRA_CFLAGS=""
if [ "$OS" = "macos" ]; then
    # Homebrew libffi lives outside the default search path.
    FFI_PREFIX="$(brew --prefix libffi 2>/dev/null || true)"
    if [ -n "$FFI_PREFIX" ]; then
        EXTRA_CFLAGS="-I$FFI_PREFIX/include"
    fi
fi

if ! gcc -O2 $EXTRA_CFLAGS -c "$RUNTIME_SRC" -o "$RUNTIME_OBJ"; then
    die "gcc failed to compile $RUNTIME_SRC"
fi

# ---------------------------------------------------------------------------
# Stage the install tree
# ---------------------------------------------------------------------------
step "Staging install tree under $PREFIX"

# If installing to a system prefix and not root, ask for sudo up front.
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    PARENT_OF_PREFIX="$(dirname "$PREFIX")"
    if [ ! -w "$PARENT_OF_PREFIX" ] 2>/dev/null && [ ! -w "$PREFIX" ] 2>/dev/null; then
        if command -v sudo >/dev/null 2>&1; then
            say "  $PREFIX is not writable; will use sudo for file copies"
            SUDO="sudo"
        else
            die "$PREFIX is not writable and sudo is not available. Re-run with --prefix \"\$HOME/.local\"."
        fi
    fi
fi

run_mkdir() { $SUDO mkdir -p "$1"; }
run_cp()    { $SUDO cp "$@"; }

PREFIX_BIN="$PREFIX/bin"
PREFIX_LIB="$PREFIX/lib/donky"
PREFIX_LIB_SRC="$PREFIX_LIB/lib"

run_mkdir "$PREFIX_BIN"
run_mkdir "$PREFIX_LIB"
run_mkdir "$PREFIX_LIB_SRC"

run_cp "$CARGO_EXE"    "$PREFIX_BIN/donky"
$SUDO chmod 0755 "$PREFIX_BIN/donky"
say "  installed $PREFIX_BIN/donky"

run_cp "$RUNTIME_OBJ"        "$PREFIX_LIB/runtime.o"
run_cp "$RUNTIME_SRC"        "$PREFIX_LIB/runtime_template.c"
say "  installed $PREFIX_LIB/runtime.o (+ runtime_template.c)"

# Standard library: *.donk under $PARENT_DIR/lib (mirrors build.ps1)
LIB_SRC="$PARENT_DIR/lib"
if [ -d "$LIB_SRC" ]; then
    COUNT=0
    while IFS= read -r -d '' f; do
        rel="${f#"$LIB_SRC"/}"
        dest="$PREFIX_LIB_SRC/$rel"
        run_mkdir "$(dirname "$dest")"
        run_cp "$f" "$dest"
        COUNT=$((COUNT + 1))
    done < <(find "$LIB_SRC" -type f -name '*.donk' -print0)
    say "  installed $COUNT .donk file(s) into $PREFIX_LIB_SRC"
else
    warn "no lib/ directory at $LIB_SRC — standard library not installed"
fi

# Examples (optional; skipped if > 50 MB, matching build.ps1).
EX_SRC="$PARENT_DIR/examples"
if [ -d "$EX_SRC" ]; then
    EX_BYTES="$(find "$EX_SRC" -type f -exec du -b {} + 2>/dev/null | awk '{s+=$1} END {print s+0}')"
    if [ "$EX_BYTES" -lt "$EXAMPLES_MAX_BYTES" ]; then
        run_mkdir "$PREFIX_LIB/examples"
        # Preserve tree structure; cp -R on both BSD and GNU cp.
        if [ -n "$SUDO" ]; then
            $SUDO cp -R "$EX_SRC/." "$PREFIX_LIB/examples/"
        else
            cp -R "$EX_SRC/." "$PREFIX_LIB/examples/"
        fi
        say "  installed examples ($((EX_BYTES / 1024)) KB)"
    else
        warn "examples/ exceeds 50 MB — not installed"
    fi
fi

# ---------------------------------------------------------------------------
# Smoke test: run the just-installed binary to build + run a tiny program
# ---------------------------------------------------------------------------
if [ "$SKIP_SMOKE" -eq 0 ]; then
    step "Smoke test (build-exe + run hello world in a temp dir)"
    SMOKE="$WORK_DIR/smoke"
    mkdir -p "$SMOKE"
    printf 'print("donky-install-ok");\n' > "$SMOKE/hello.donk"
    (
        cd "$SMOKE"
        "$PREFIX_BIN/donky" build-exe hello.donk >/dev/null
        OUT="$("$SMOKE/donky_app" 2>&1)"
        [ "$OUT" = "donky-install-ok" ] || { printf 'smoke output: %s\n' "$OUT" >&2; exit 1; }
    ) || die "smoke test failed (see output above). Re-run with --skip-smoke to bypass."
    say "  self-test OK"
fi

# ---------------------------------------------------------------------------
# Post-install: PATH check
# ---------------------------------------------------------------------------
step "Post-install"

case ":$PATH:" in
    *":$PREFIX_BIN:"*) PATH_OK=1 ;;
    *)                 PATH_OK=0 ;;
esac

if [ "$PATH_OK" -eq 0 ]; then
    warn "$PREFIX_BIN is not in your PATH."
    RC=""
    case "${SHELL:-}" in
        */zsh)  RC="$HOME/.zshrc" ;;
        */bash) RC="$HOME/.bashrc" ;;
        *)      RC="$HOME/.profile" ;;
    esac
    say ""
    say "Add this line to $RC:"
    say ""
    say "    export PATH=\"$PREFIX_BIN:\$PATH\""
    say ""
    say "Then reload:  source $RC"
fi

say ""
say "${C_GRN}Donky installed successfully.${C_RST}"
say ""
say "  binary   : $PREFIX_BIN/donky"
say "  runtime  : $PREFIX_LIB/runtime.o (embedded in binary; reference copy for debugging)"
say "  stdlib   : $PREFIX_LIB_SRC"
say ""
say "Try:"
say "  donky --version"
say "  donky examples/hello.donk       # tree-walker"
say "  donky build-exe hello.donk      # standalone binary (needs gcc on PATH; writes into CWD)"
say ""
warn "build-exe writes runtime.c / *.obj into the CURRENT directory. Never run it from /usr or /usr/local."
say ""