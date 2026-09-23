#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the effect does nothing".
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   physics       every harness check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. A check that holds at one raster
#                 was fitted to it. The chain's checks are raster-free by
#                 construction (the decoder's picture is the mode's own 320xN)
#                 and run at both anyway; the rendering ones are not:
#                   --timing      every line, segment and pixel boundary is its
#                                 constant, to the sample, over a whole frame
#                   --slant       the lean is e * T_line / T_pixel, both signs,
#                                 whole and fractional drifts apart
#                   --levels      flat fields exact, a ramp within a step, an
#                                 edge's tail at the one-pole's tau
#                   --threshold   the variance is the linearised closed form
#                                 above the knee; the knee where Rice says
#                   --progressive floor( t s / T_line ) lines; Speed is inert
#                                 per sample
#                   --vis         the header decodes to the mode it was sent in
#                   --sync        Line Sync holds the edge within a pixel
#                   --clock       a six-day host clock runs the same signal
#                   --names       nothing the host will silently truncate
#                   --render      the frame is the decoder's picture, byte for
#                                 byte; a resize mid-run keeps it
#                   --raster      the lean, fitted again in the rendered frame
#                   --negative    the checks can FAIL: twelve broken models,
#                                 each caught by the bound that should catch it
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word.
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "shaders"
if tools/check-shaders.sh; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

SSTEST="$BUILD/sstest"

for size in 320x180 1280x720; do
	step "physics at $size"
	for check in timing slant levels threshold progressive vis sync clock names render raster negative; do
		if out=$("$SSTEST" --$check --size $size 2>&1); then
			pass "sstest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "sstest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

step "sweep"
if out=$(python3 tools/sweep.py --binary "$SSTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$SSTEST" --bench --bench-frames 60 2>&1 | sed -n '4,9p' | sed 's/^/   /'
"$SSTEST" --engine 2>&1 | sed -n '3,8p' | sed 's/^/   /'

BUNDLE="$BUILD/Slowscan.bundle"
BIN="$BUNDLE/Contents/MacOS/Slowscan"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.slowscan" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Slowscan.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Slowscan" "id:          SS01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
