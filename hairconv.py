import bpy, os, glob, numpy as np

SRC  = r"C:/Users/mrcel/NyxEngine/projects/Sandbox/assets/models"
MAPS = SRC + "/Maps/HairCards/Belt"
OUT  = SRC + "/HairTest"
os.makedirs(OUT, exist_ok=True)
LOG  = open(r"C:/Users/mrcel/NyxEngine/hairconv.txt", "w")
def log(*a): print(*a); LOG.write(" ".join(str(x) for x in a) + "\n"); LOG.flush()

FBX     = SRC + "/uploads_files_6840509_SM_General_BeltFurHairCards.fbx"
OPACITY = MAPS + "/MT_Gladiator_BeltFurHairCards_Opacity.png"
BCA     = OUT + "/BeltFur_BaseColorAlpha.png"   # baked brown RGB + opacity in alpha

# ── Bake a baseColor texture whose ALPHA channel carries the hair mask ──────────
# The source "Opacity" map stores the strand mask as a grayscale; we don't know if
# it lives in RGB or A, so pick the channel with the most variance. glTF cutout
# needs the mask in baseColorTexture.a, so we composite: RGB = flat brown, A = mask.
op = bpy.data.images.load(OPACITY, check_existing=True)
try: op.colorspace_settings.name = 'Non-Color'   # read raw mask values, no gamma
except Exception: pass
w, h = op.size
px = np.array(op.pixels[:], dtype=np.float32).reshape(h, w, 4)
r, a = px[:, :, 0], px[:, :, 3]
mask = r if r.var() >= a.var() else a
log(f"opacity {w}x{h}  R.var={r.var():.4f} A.var={a.var():.4f}  -> mask={'R' if r.var()>=a.var() else 'A'}")
log(f"mask range [{mask.min():.3f}, {mask.max():.3f}] mean={mask.mean():.3f}")

out = np.empty((h, w, 4), dtype=np.float32)
out[:, :, 0] = 0.32; out[:, :, 1] = 0.20; out[:, :, 2] = 0.13   # warm brown (sRGB-stored)
out[:, :, 3] = mask
img = bpy.data.images.new("BeltFurBCA", w, h, alpha=True)
img.pixels = out.ravel()
img.filepath_raw = BCA
img.file_format = 'PNG'
img.save()
log("baked baseColor+alpha:", BCA)

# ── Material: alpha-clipped (glTF MASK) using the baked texture ─────────────────
def build_cutout(mat):
    mat.use_nodes = True
    nt = mat.node_tree; nt.nodes.clear()
    out_n = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf  = nt.nodes.new('ShaderNodeBsdfPrincipled')
    nt.links.new(bsdf.outputs['BSDF'], out_n.inputs['Surface'])
    t = nt.nodes.new('ShaderNodeTexImage')
    t.image = bpy.data.images.load(BCA, check_existing=True)
    try: t.image.colorspace_settings.name = 'sRGB'
    except Exception: pass
    nt.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    nt.links.new(t.outputs['Alpha'], bsdf.inputs['Alpha'])
    bsdf.inputs['Roughness'].default_value = 0.85
    bsdf.inputs['Metallic'].default_value  = 0.0
    mat.blend_method  = 'CLIP'        # -> glTF alphaMode=MASK
    mat.alpha_threshold = 0.5
    try: mat.shadow_method = 'CLIP'
    except Exception: pass

bpy.ops.wm.read_factory_settings(use_empty=True)
before = set(bpy.data.objects)
bpy.ops.import_scene.fbx(filepath=FBX)
new    = [o for o in bpy.data.objects if o not in before]
meshes = [o for o in new if o.type == 'MESH']
log("imported meshes:", [o.name for o in meshes])

for m in meshes:
    for mod in list(m.modifiers):
        if mod.type == 'ARMATURE': m.modifiers.remove(mod)
    bpy.ops.object.select_all(action='DESELECT')
    m.select_set(True); bpy.context.view_layer.objects.active = m
    if m.parent: bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
    if not m.data.materials:
        m.data.materials.append(bpy.data.materials.new("BeltFur_mat"))
    for slot in m.material_slots:
        if slot.material: build_cutout(slot.material)

for o in new:
    if o not in set(meshes):
        try: bpy.data.objects.remove(o, do_unlink=True)
        except Exception: pass

out_path = OUT + "/HairTest.gltf"
bpy.ops.export_scene.gltf(
    filepath=out_path, export_format='GLTF_SEPARATE',
    export_skins=False, export_animations=False, export_yup=True,
    use_selection=False, export_apply=True,
)
log("EXPORTED:", out_path)
LOG.close()
