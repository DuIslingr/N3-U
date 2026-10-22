"""Glitch effect files (effects/*.bdae): techniques, passes, render states, parameters.

An effect file is a ZIP holding a BRES image (little_endian_not_quantized.bdae)
whose root lists effects at +0x70/+0x74 (0x104 bytes each):

    effect +0x00 id, +0x04 name
           +0x38/+0x3C techniques (0x0C each: name, pass count, first pass)
           +0x40/+0x44 parameters (0x18 each, see below)

A pass is 0x74 bytes followed directly by its uniform bindings (0x10 each); the
next pass of the technique follows the bindings:

    pass +0x04 vertex shader file, +0x08 entry, +0x0C defines ('\\n'-separated)
         +0x10 fragment shader file, +0x14 entry, +0x18 defines
         +0x1C..+0x2C packed render state (5 words, see RenderState)
         +0x6C/+0x70 bindings: u32 0xFFFF, uniform name, u16 kind, u16, u16 index, u16
                     kind 0: effect parameter `index`; kind 1: engine global

    parameter +0x00 name, +0x04 type, +0x08 sampler/flags, +0x0C count,
              +0x10 ?, +0x14 default value (floats; textures have none)

Parameter types: 4 float, 5 vec2, 6 vec3, 7 vec4, 0x0B mat4, 0x0C-0x10 samplers (0x0D 2D,
0x10 cube), 22 technique. Materials select a technique by name through their
"<effect>-profile_GLES2/CurrentTechnique" parameter ({u32 2, char* name}).

Blend factor nibbles index the driver's table at 0xc79e30 in libNOVA3_neon.so.
"""
import io
import struct
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import bdae  # noqa: E402

ROOT_EFFECTS = (0x70, 0x74, 0x104)
TECHNIQUE_SIZE = 0x0C
PASS_SIZE = 0x74
BINDING_SIZE = 0x10
PARAM_SIZE = 0x18

BLEND_FACTORS = ["zero", "one", "src_color", "one_minus_src_color", "src_alpha", "one_minus_src_alpha",
                 "dst_alpha", "one_minus_dst_alpha", "dst_color", "one_minus_dst_color", "constant_color",
                 "one_minus_constant_color", "constant_alpha", "one_minus_constant_alpha", "src_alpha_saturate"]

TYPE_COMPONENTS = {4: 1, 5: 2, 6: 3, 7: 4}


@dataclass
class RenderState:
    words: tuple

    @property
    def blend(self):
        return bool(self.words[3] & 0x00080000)

    @property
    def cull(self):
        return bool(self.words[3] & 0x00100000)

    @property
    def depth_test(self):
        return bool(self.words[3] & 0x00400000)

    @property
    def depth_write(self):
        return bool(self.words[3] & 0x00800000)

    @property
    def blend_factors(self):
        """(src rgb, dst rgb, src alpha, dst alpha) as BLEND_FACTORS names."""
        w = self.words[0]
        return tuple(BLEND_FACTORS[(w >> s) & 0xF] for s in (0, 4, 8, 12))

    def describe(self):
        parts = []
        if self.blend:
            parts.append("blend(%s,%s)" % self.blend_factors[:2])
        if self.cull:
            parts.append("cull")
        parts.append("ztest" if self.depth_test else "no-ztest")
        if self.depth_write:
            parts.append("zwrite")
        return " ".join(parts)


@dataclass
class Binding:
    uniform: str
    kind: int        # 0 effect parameter, 1 engine global
    index: int


@dataclass
class Pass:
    vertex_shader: str
    vertex_defines: list
    fragment_shader: str
    fragment_defines: list
    state: RenderState
    bindings: list


@dataclass
class Technique:
    name: str
    passes: list


@dataclass
class Parameter:
    name: str
    type: int
    count: int
    default: tuple = ()


@dataclass
class Effect:
    id: str
    name: str
    techniques: list = field(default_factory=list)
    parameters: list = field(default_factory=list)

    def technique(self, name):
        for t in self.techniques:
            if t.name == name:
                return t
        return None


def _defines(text):
    out = []
    for line in (text or "").split("\n"):
        line = line.strip()
        if line.startswith("#define "):
            out.append(line[len("#define "):].strip())
    return out


def bres_from_file(data):
    if data[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = z.namelist()
            name = "little_endian_not_quantized.bdae"
            data = z.read(name if name in names else [n for n in names if n.endswith(".bdae")][0])
    return bdae.Bres(data)


def load(path_or_bytes):
    data = Path(path_or_bytes).read_bytes() if isinstance(path_or_bytes, (str, Path)) else path_or_bytes
    b = bres_from_file(data)
    effects = []
    for e in b.array(b.root, ROOT_EFFECTS):
        fx = Effect(b.cstr(e), b.cstr(e + 4))
        for p in b.array(e, (0x40, 0x44, PARAM_SIZE)):
            ptype, count = b.u32(p + 4), b.u32(p + 0xC)
            default = ()
            n = TYPE_COMPONENTS.get(ptype, 16 if ptype == 0x0B else 0)
            value = b.ptr(p + 0x14)
            if n and value is not None and value + 4 * n * max(count, 1) <= len(b.data):
                default = b.f32(value, n * max(count, 1))
                default = default if isinstance(default, tuple) else (default,)
            fx.parameters.append(Parameter(b.cstr(p), ptype, count, default))
        for t in b.array(e, (0x38, 0x3C, TECHNIQUE_SIZE)):
            npass, p = b.u32(t + 4), b.ptr(t + 8)
            passes = []
            for _ in range(npass):
                nb, bp = b.u32(p + 0x6C), b.ptr(p + 0x70)
                if nb and bp != p + PASS_SIZE:
                    raise bdae.BdaeError(f"{fx.id}/{b.cstr(t)}: bindings not after pass")
                bindings = []
                for k in range(nb):
                    o = bp + k * BINDING_SIZE
                    bindings.append(Binding(b.cstr(o + 4), b.u32(o + 8) & 0xFFFF, b.u32(o + 0xC) & 0xFFFF))
                passes.append(Pass(b.cstr(p + 4), _defines(b.cstr(p + 0xC)), b.cstr(p + 0x10), _defines(b.cstr(p + 0x18)),
                                   RenderState(tuple(b.u32(p + 0x1C + 4 * i) for i in range(5))), bindings))
                p = p + PASS_SIZE + nb * BINDING_SIZE
            fx.techniques.append(Technique(b.cstr(t), passes))
        effects.append(fx)
    return effects


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    failed = 0
    for f in args.files:
        try:
            effects = load(f)
        except Exception as ex:  # noqa: BLE001
            print(f"FAIL {f}: {ex}")
            failed += 1
            continue
        for fx in effects:
            npass = sum(len(t.passes) for t in fx.techniques)
            print(f"OK   {Path(f).name}: {fx.id} {len(fx.techniques)} techniques, {npass} passes, {len(fx.parameters)} params")
            if args.verbose:
                for t in fx.techniques:
                    for i, p in enumerate(t.passes):
                        print(f"       {t.name}[{i}] {p.vertex_shader} {'+'.join(p.vertex_defines)} | "
                              f"{p.fragment_shader} {'+'.join(p.fragment_defines)} | {p.state.describe()}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
