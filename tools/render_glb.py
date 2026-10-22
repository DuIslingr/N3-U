"""Render a .glb to PNG with Blender, for checking converted models.

Usage: blender -b --factory-startup --python tools/render_glb.py -- model.glb out.png
       [--pose-test] [--only REGEX] [--azimuth DEGREES]

Frames the model from a 3/4 front view with a soft key light. --pose-test
rotates the first skinned armature's bones a little to show the skinning works.
--only keeps just the meshes whose name matches REGEX (e.g. one LOD).
--azimuth turns the camera around the model (default 30); --elevation sets how high
it looks down from (default 0.3; large values give a top-down view).
--action NAME plays the glTF animation whose name starts with NAME and renders
--frames N evenly spaced moments of it to out_0.png ... out_{N-1}.png.
"""

import math
import re
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
pose_test = "--pose-test" in argv
only = re.compile(argv[argv.index("--only") + 1]) if "--only" in argv else None
azimuth = math.radians(float(argv[argv.index("--azimuth") + 1])) if "--azimuth" in argv else math.radians(30)
# --focus F: aim at height fraction F of the model (0 = feet, 1 = top); --distance D scales camera distance
focus = float(argv[argv.index("--focus") + 1]) if "--focus" in argv else None
distance = float(argv[argv.index("--distance") + 1]) if "--distance" in argv else 1.0
elevation = float(argv[argv.index("--elevation") + 1]) if "--elevation" in argv else 0.3   # camera height factor
# --eye X,Y,Z,YAW: first-person camera at a game position (game Z-up coordinates match Blender's)
eye = [float(v) for v in argv[argv.index("--eye") + 1].split(",")] if "--eye" in argv else None
action_name = argv[argv.index("--action") + 1] if "--action" in argv else None
nframes = int(argv[argv.index("--frames") + 1]) if "--frames" in argv else 4

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)


def activate_action(prefix):
    """Make the imported animation `prefix` active on every object it drives; return its frame range."""
    rng = None
    for obj in bpy.context.scene.objects:
        ad = obj.animation_data
        if ad is None:
            continue
        for track in ad.nla_tracks:
            track.mute = True
        found = None
        for track in ad.nla_tracks:
            for strip in track.strips:
                if strip.action and strip.action.name.startswith(prefix):
                    found = strip.action
        if found is None:
            continue
        ad.action = found
        if hasattr(ad, "action_slot") and found.slots:
            ad.action_slot = found.slots[0]
        lo_f, hi_f = found.frame_range
        rng = (lo_f, hi_f) if rng is None else (min(rng[0], lo_f), max(rng[1], hi_f))
    return rng


frames = [None]
if action_name:
    rng = activate_action(action_name)
    if rng is None:
        sys.exit(f"animation {action_name!r} not found")
    step = (rng[1] - rng[0]) / nframes
    frames = [rng[0] + step * i for i in range(nframes)]
    print(f"ACTION {action_name} frames {rng}")

meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
if only:
    for o in meshes:
        if not (only.search(o.name) or only.search(o.data.name)):
            bpy.data.objects.remove(o, do_unlink=True)
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
if pose_test:
    for arm in (o for o in bpy.context.scene.objects if o.type == "ARMATURE"):
        for i, pb in enumerate(arm.pose.bones):
            if i % 3 == 1:
                pb.rotation_mode = "XYZ"
                pb.rotation_euler = (0.0, 0.0, 0.35)
        break
bpy.context.view_layer.update()

def set_frame(f):
    if f is not None:
        bpy.context.scene.frame_set(int(f), subframe=f - int(f))
    bpy.context.view_layer.update()


lo = Vector((1e9, 1e9, 1e9))
hi = Vector((-1e9, -1e9, -1e9))
for f in frames:   # frame the camera on the union of all rendered poses
    set_frame(f)
    deps = bpy.context.evaluated_depsgraph_get()
    for o in meshes:
        ev = o.evaluated_get(deps)
        for v in ev.to_mesh().vertices:
            w = o.matrix_world @ v.co
            lo = Vector(map(min, lo, w))
            hi = Vector(map(max, hi, w))
        ev.to_mesh_clear()
center = (lo + hi) / 2
size = max((hi - lo).length, 1e-3)
if focus is not None:
    center.z = lo.z + (hi.z - lo.z) * focus
size *= distance
print(f"BOUNDS min={tuple(round(x, 3) for x in lo)} max={tuple(round(x, 3) for x in hi)}")

scene = bpy.context.scene
cam_data = bpy.data.cameras.new("cam")
cam_data.lens = 50
cam_data.clip_end = size * 20
cam = bpy.data.objects.new("cam", cam_data)
scene.collection.objects.link(cam)
direction = Vector((math.sin(azimuth), -math.cos(azimuth), elevation)).normalized()
if eye:
    cam_data.lens = 18
    cam_data.clip_start = 0.05
    cam.location = Vector(eye[:3])
    cam.rotation_euler = (math.radians(88), 0, math.radians(eye[3]))
else:
    cam.location = center + direction * size * 1.6
    cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
scene.camera = cam

sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
sun.data.energy = 3.0
sun.rotation_euler = (math.radians(50), 0, math.radians(30))
scene.collection.objects.link(sun)
world = bpy.data.worlds.new("w")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.35, 0.37, 0.42, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 1.0
scene.world = world

scene.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items} else "BLENDER_EEVEE"
scene.render.resolution_x = 1280 if eye else 900
scene.render.resolution_y = 720 if eye else 900
for i, f in enumerate(frames):
    set_frame(f)
    scene.render.filepath = out if f is None else out.replace(".png", f"_{i}.png")
    bpy.ops.render.render(write_still=True)
    print("RENDERED", scene.render.filepath)
