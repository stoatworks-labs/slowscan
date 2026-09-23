#!/usr/bin/env bash
#
# Every shader this repo ships, through a real GLSL compiler, before a host
# has to find out. Called by tools/verify.sh AND by CI, so the two cannot
# drift: a GitHub macOS runner cannot create an accelerated GL context, so
# in CI this is the only thing that looks at the GLSL at all. It is not a
# substitute for a driver -- only a real driver catches the class of bug
# where Apple's Metal-backed GL disagrees with the compiler, and that is
# checked on the dev Mac by `sstest --render`, and nowhere else.
#
#   tools/check-shaders.sh
#
set -uo pipefail
cd "$(dirname "$0")/.."

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/shaders/Vertex.cpp",
	"source/shaders/Readback.cpp",
	"source/shaders/Compose.cpp",
]

# A shader may be several adjacent raw strings (MSVC caps one literal at
# about 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$n" -ne 3 ]; then
		# The count is asserted, not counted up to: a check that silently
		# looks at fewer shaders than exist is worse than no check.
		printf '   %d shaders extracted, expected 3 -- the extraction has gone stale\n' "$n"
		bad=$(( bad + 1 ))
	fi
	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}


shaders_compile
