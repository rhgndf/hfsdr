import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def parse_args():
    try:
        start = sys.argv.index("--") + 1
    except ValueError:
        start = len(sys.argv)
    args = sys.argv[start:]
    if len(args) != 3:
        raise SystemExit("usage: blender --background --python convert_vrml_to_glb.py -- input.wrl output.glb preview.png")
    return Path(args[0]), Path(args[1]), Path(args[2])


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def import_vrml(path):
    try:
        bpy.ops.preferences.addon_enable(module="io_scene_x3d")
    except Exception:
        pass
    if not hasattr(bpy.ops.import_scene, "x3d"):
        raise RuntimeError("Blender X3D/VRML importer is unavailable")
    bpy.ops.import_scene.x3d(filepath=str(path))


def scene_bounds(objects):
    points = []
    for obj in objects:
        if obj.type not in {"MESH", "CURVE", "SURFACE", "FONT"}:
            continue
        for corner in obj.bound_box:
            points.append(obj.matrix_world @ Vector(corner))
    if not points:
        raise RuntimeError("No renderable geometry was imported")
    min_corner = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    max_corner = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    return min_corner, max_corner


def center_scene(objects):
    min_corner, max_corner = scene_bounds(objects)
    center = (min_corner + max_corner) / 2
    for obj in objects:
        obj.location -= center
    min_corner, max_corner = scene_bounds(objects)
    size = max_corner - min_corner
    return max(size.x, size.y, size.z, 1.0)


def add_camera_and_light(max_dim):
    distance = max_dim * 2.2
    bpy.ops.object.light_add(type="AREA", location=(0, -distance * 0.4, distance))
    light = bpy.context.object
    light.name = "Review softbox"
    light.data.energy = 450
    light.data.size = max_dim * 1.5

    bpy.ops.object.camera_add(location=(distance, -distance, distance * 0.75), rotation=(math.radians(60), 0, math.radians(42)))
    camera = bpy.context.object
    direction = Vector((0, 0, 0)) - camera.location
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = max_dim * 1.6
    bpy.context.scene.camera = camera


def render_preview(path):
    scene = bpy.context.scene
    scene.render.resolution_x = 1400
    scene.render.resolution_y = 900
    scene.render.film_transparent = False
    scene.world.color = (0.94, 0.96, 0.98)
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(path)
    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE"
    bpy.ops.render.render(write_still=True)


def export_glb(path):
    bpy.ops.export_scene.gltf(
        filepath=str(path),
        export_format="GLB",
        export_apply=True,
        export_yup=True,
    )


def main():
    input_path, glb_path, preview_path = parse_args()
    glb_path.parent.mkdir(parents=True, exist_ok=True)
    preview_path.parent.mkdir(parents=True, exist_ok=True)

    clear_scene()
    import_vrml(input_path)
    objects = list(bpy.context.scene.objects)
    max_dim = center_scene(objects)
    add_camera_and_light(max_dim)
    export_glb(glb_path)
    render_preview(preview_path)


if __name__ == "__main__":
    main()
