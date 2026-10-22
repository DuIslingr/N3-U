"""Read keyframe animation from N.O.V.A. 3 .bdae files.

Layout (from CColladaDatabase, SAnimationSegment::getData, SAnimationAccessor and
the animation_track interpreters in the engine):

* Root +0x24/+0x28: SAnimation[count], 0x20 bytes each:
    +0x00 id ("Bip01-node-translation")
    +0x08 SSampler*      +0x10 SChannel*      +0x18 SDefaults* (optional)
* SChannel: +0x00 target node id, +0x04 channel type (see CHANNEL_*).
  Other channel types: 114 material parameter (+0x00 material id, +0x08 parameter
  name, +0x0C mask of the components written, e.g. 8 = w), 19 light colour (RGB
  bytes, target = light id), 24 node visibility, 115/116 UV offset U/V,
  117/118 texture matrix rotation/scale, 18 morph weights, 14/16 camera fov/near.
* SSampler (u32s): +0x04 key time type (1 = u8 frames, 3 = u16 frames at 30 fps,
  4 = i32 ms), +0x0c time buffer index, +0x10 value type (6 = float),
  +0x14 components, +0x18 value buffer index, +0x1c stride << 16 | offset.
* SDefaults: +0x00 flags, +0x04 count, +0x08 float* - the full vector a
  single-axis track writes into, or (for axis-angle rotation) the axis.
* Root +0x30 -> {count, SAnimationSegment*}: 0x1c bytes each
  {start ms, end ms, state, reloc count, data offset, data size, runtime ptr}.
  Each segment's data block is SAnimationData: u32 count, then count x
  {u32 size, i32 offset relative to the offset field}. Samplers index these
  buffers. Key times are absolute on the file's timeline, and each segment
  repeats the previous segment's last key.
* Root +0x38/+0x3c: clips, 12 bytes each {name, start ms, end ms}, all on one
  timeline shared by every track in the file.
"""

import struct
from dataclasses import dataclass

import numpy as np

from bdae import Bres, ROOT_ANIMATIONS

FPS = 30.0

CHANNEL_TRANSLATION = 1
CHANNEL_TRANSLATION_X, CHANNEL_TRANSLATION_Y, CHANNEL_TRANSLATION_Z = 2, 3, 4
CHANNEL_ROTATION = 5            # quaternion (x, y, z, w)
CHANNEL_ROTATION_ANGLE = 9      # angle in radians about the axis in SDefaults
CHANNEL_SCALE = 10
CHANNEL_SCALE_X, CHANNEL_SCALE_Y, CHANNEL_SCALE_Z = 11, 12, 13

PROPERTY_OF = {
    CHANNEL_TRANSLATION: ("translation", None),
    CHANNEL_TRANSLATION_X: ("translation", 0),
    CHANNEL_TRANSLATION_Y: ("translation", 1),
    CHANNEL_TRANSLATION_Z: ("translation", 2),
    CHANNEL_ROTATION: ("rotation", None),
    CHANNEL_ROTATION_ANGLE: ("rotation", "angle"),
    CHANNEL_SCALE: ("scale", None),
    CHANNEL_SCALE_X: ("scale", 0),
    CHANNEL_SCALE_Y: ("scale", 1),
    CHANNEL_SCALE_Z: ("scale", 2),
}

TIME_FORMATS = {1: ("B", 1, 1000.0 / FPS), 3: ("H", 2, 1000.0 / FPS), 4: ("i", 4, 1.0)}


@dataclass
class Track:
    target: str             # node id
    channel: int
    times: np.ndarray       # ms on the file's timeline
    values: np.ndarray      # (n, components)
    defaults: tuple         # full vector / axis from SDefaults, or None

    @property
    def property(self):
        return PROPERTY_OF.get(self.channel, (None, None))


@dataclass
class Clip:
    name: str
    start: float            # ms
    end: float              # ms


class AnimationLibrary:
    def __init__(self, data):
        b = self.bres = Bres(data) if isinstance(data, (bytes, bytearray)) else data
        r = b.root
        self.clips = [Clip(b.cstr(o), b.i32(o + 4), b.i32(o + 8)) for o in b.array(r, (0x38, 0x3C, 0x0C))]
        self.segments = self._segments()
        self.tracks = [t for t in (self._track(a) for a in b.array(r, ROOT_ANIMATIONS)) if t is not None]

    def _segments(self):
        b = self.bres
        table = b.ptr(b.root + 0x30)
        if table is None:
            return []
        segs = []
        base = b.ptr(table + 4)
        for i in range(b.i32(table)):
            start, end, _state, nrel, off, size, _rt = struct.unpack_from("<7i", b.data, base + 0x1C * i)
            if nrel:
                raise NotImplementedError("animation segment with relocations")
            count = b.u32(off)
            bufs = []
            for k in range(count):
                field = off + 8 + 8 * k
                bufs.append((field + struct.unpack_from("<i", b.data, field)[0], b.u32(off + 4 + 8 * k)))
            segs.append((start, end, bufs))
        return segs

    def _track(self, a):
        b = self.bres
        d = b.data
        channel = b.ptr(a + 0x10)
        sampler = b.ptr(a + 0x08)
        if channel is None or sampler is None:
            return None
        ctype = b.u32(channel + 4)
        s = struct.unpack_from("<8I", d, sampler)
        if s[1] not in TIME_FORMATS or s[4] != 6:   # only float values occur in the game data
            return None
        tfmt, tsize, tscale = TIME_FORMATS[s[1]]
        comps, stride, voff = s[5], s[7] >> 16, s[7] & 0xFFFF
        times, values = [], []
        for _start, _end, bufs in self.segments:
            if max(s[3], s[6]) >= len(bufs):
                continue
            tb, tlen = bufs[s[3]]
            vb, _ = bufs[s[6]]
            n = tlen // tsize
            if n == 0:
                continue
            t = np.frombuffer(d, "<" + tfmt, n, tb).astype(np.float64) * tscale
            v = np.ndarray((n, comps), "<f4", d, vb + voff, (stride, 4)).astype(np.float64)
            if times and t[0] <= times[-1][-1]:
                keep = t > times[-1][-1]          # segments share their boundary key
                t, v = t[keep], v[keep]
            if not len(t):
                continue
            times.append(t)
            values.append(v)
        if not times:
            return None
        defaults = None
        dp = b.ptr(a + 0x18)
        if dp is not None and b.ptr(dp + 8) is not None:
            defaults = b.f32(b.ptr(dp + 8), 3)
        return Track(b.cstr(channel), ctype, np.concatenate(times), np.concatenate(values), defaults)


# ---------------------------------------------------------------------- sampling

def _interp(track, t):
    """Linearly interpolate every component of `track` at times `t` (clamped)."""
    return np.stack([np.interp(t, track.times, track.values[:, c]) for c in range(track.values.shape[1])], 1)


def _quat_interp(track, t):
    """Normalized lerp of quaternion keys at times `t` (hemisphere-corrected)."""
    q = getattr(track, "_aligned", None)
    if q is None:
        q = track.values / np.linalg.norm(track.values, axis=1, keepdims=True)
        # Keep neighbouring keys in the same hemisphere so lerping takes the short way.
        flips = np.einsum("ij,ij->i", q[:-1], q[1:]) < 0
        sign = np.concatenate([[1.0], np.where(np.cumsum(flips) % 2, -1.0, 1.0)])
        q = q * sign[:, None]
        track._aligned = q
    idx = np.clip(np.searchsorted(track.times, t, side="right") - 1, 0, len(q) - 1)
    nxt = np.minimum(idx + 1, len(q) - 1)
    span = track.times[nxt] - track.times[idx]
    f = np.where(span > 0, (t - track.times[idx]) / np.where(span > 0, span, 1), 0.0)
    f = np.clip(f, 0, 1)[:, None]
    out = q[idx] * (1 - f) + q[nxt] * f
    return out / np.linalg.norm(out, axis=1, keepdims=True)


def _axis_angle(axis, angles):
    axis = np.asarray(axis, float)
    axis = axis / max(np.linalg.norm(axis), 1e-12)
    h = angles / 2.0
    return np.concatenate([axis[None, :] * np.sin(h)[:, None], np.cos(h)[:, None]], 1)


def sample_property(tracks, prop, rest, t):
    """Values of node property `prop` ("translation"/"rotation"/"scale") at times `t`.

    `tracks` are this node's tracks; `rest` is the node's rest value, used for
    components no track animates.
    """
    if prop == "rotation":
        full = [tr for tr in tracks if tr.property == ("rotation", None)]
        if full:
            return _quat_interp(full[0], t)
        angle = next(tr for tr in tracks if tr.property == ("rotation", "angle"))
        return _axis_angle(angle.defaults, _interp(angle, t)[:, 0])
    out = np.tile(np.asarray(rest, float), (len(t), 1))
    for tr in tracks:
        p, comp = tr.property
        if p != prop:
            continue
        if comp is None:
            out = _interp(tr, t)
        else:
            if tr.defaults is not None:
                others = [c for c in range(3) if c != comp]
                out[:, others] = np.asarray(tr.defaults, float)[others]
            out[:, comp] = _interp(tr, t)[:, 0]
    return out


def clip_times(tracks, clip):
    """Key times (ms) of `tracks` inside `clip`, always including both ends."""
    ts = [clip.start, clip.end]
    for tr in tracks:
        ts.extend(tr.times[(tr.times > clip.start) & (tr.times < clip.end)])
    return np.unique(np.asarray(ts, float))
