"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds three GLSL stages and so does `source/shaders/`. That is
two copies of the same text, and two copies drift -- quietly, because a demo
that renders a *plausible* picture looks exactly like a demo that renders the
right one. The whole claim of these pages is that they run the plugin's own
shader rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `sstest --render` drives the real plugin class through a real
FFGL sequence and has no idea this page exists, and `tools/check-shaders.sh`
compiles the C++ copies through glslc and never looks at the JS one.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ and each matching backtick literal
out of `plugin.js`, and compares them exactly -- no whitespace normalisation, no
comment stripping. A comment updated on one side and not the other is exactly
the drift worth catching.

There is no decoding to do. None of the three C++ bodies contains a backtick, a
backslash or a `${`, so each goes into its template literal as it stands, and
this REJECTS any backslash on the JS side: a template literal would silently
turn an escape into a different character, so a backslash there could only be a
difference being hidden. If a shader ever gains a backtick in a comment, escape
it as \\` in plugin.js and teach this file to decode that one escape, as galvo's
does -- nothing else.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `demo/sstv.js` is a hand translation of
`source/sstv/` and the conversions in `plugin.js` are a hand translation of
`source/Controls.h` and `Slowscan::resolve()`, and only a reader can tell
whether they still agree. When you change one of those, change it here too --
a wrong constant shows up on the page as a picture that is subtly the wrong
slant, or the wrong noise, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol.
SHADERS = [
    ("VERTEX", "source/shaders/Vertex.cpp", "kVertex"),
    ("READBACK", "source/shaders/Readback.cpp", "kReadbackFragment"),
    ("COMPOSE", "source/shaders/Compose.cpp", "kComposeFragment"),
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # The C++ carries no backslash at all, so a stray one here is either a typo
    # or a difference being smuggled through the template literal.
    stray = body.find("\\")
    if stray >= 0:
        return None, f"backslash at line {body[:stray].count(chr(10)) + 1}"
    if "${" in body:
        return None, "`${` substitution"

    return body, None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<10} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
