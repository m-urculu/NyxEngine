import bpy, os, glob

SRC  = r"C:/Users/mrcel/NyxEngine/projects/Sandbox/assets/models"
MAPS = SRC + "/Maps"
OUT  = SRC + "/Gladiator"
os.makedirs(OUT, exist_ok=True)
LOG  = open(r"C:/Users/mrcel/NyxEngine/gladconv.txt", "w")
def log(*a): print(*a); LOG.write(" ".join(str(x) for x in a) + "\n"); LOG.flush()

# part -> (fbx filename, Maps subfolder)
PARTS = [
    ("Body",       "uploads_files_6840509_SK_Gladiator_Body.fbx",       "Body"),
    ("Arms",       "uploads_files_6840509_SK_Gladiator_Arms.fbx",       "Arms"),
    ("Belt",       "uploads_files_6840509_SK_Gladiator_Belt.fbx",       "Belt"),
    ("Cape",       "uploads_files_6840509_SK_Gladiator_Cape.fbx",       "Cape"),
    ("LowerParts", "uploads_files_6840509_SK_Gladiator_LowerParts.fbx", "LowerParts"),
    ("Neck",       "uploads_files_6840509_SK_Gladiator_Neck.fbx",       "Neck"),
    # Head (SK_Gladiator_Head) skipped: the MetaHuman head FBX is ~370 facial-groom
    # strand meshes — unusable. The helmet below is the gladiator's headgear instead.
    ("Helmet",     "uploads_files_6840509_SM_Gladiator_Helmet.fbx",     "Helmet"),
]

import re
def is_top_lod(name):
    m = re.search(r'lod(\d+)', name.lower())   # MetaHuman head ships LOD0..LOD7
    return (m is None) or (m.group(1) == '0')

def find_tex(folder, kind):
    # prefer .jpg over .tga; kind is a substring like 'BaseColor'
    for ext in ("jpg", "jpeg", "png", "tga"):
        hits = sorted(glob.glob(os.path.join(MAPS, folder, f"*{kind}*.{ext}")))
        if hits:
            return hits[0].replace("\\", "/")
    return None

def sep_rgb_node(nt):
    # Blender 4.x: SeparateColor (Red/Green/Blue); 3.x: SeparateRGB (R/G/B)
    try:
        n = nt.nodes.new('ShaderNodeSeparateColor'); return n, 'Green', 'Blue'
    except RuntimeError:
        n = nt.nodes.new('ShaderNodeSeparateRGB');   return n, 'G', 'B'

def build_mat(mat, base, nrm, orm):
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out  = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    if base:
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = bpy.data.images.load(base, check_existing=True)
        try: t.image.colorspace_settings.name = 'sRGB'
        except Exception: pass
        nt.links.new(t.outputs['Color'], bsdf.inputs['Base Color'])
    if nrm:
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = bpy.data.images.load(nrm, check_existing=True)
        try: t.image.colorspace_settings.name = 'Non-Color'
        except Exception: pass
        nm = nt.nodes.new('ShaderNodeNormalMap')
        nt.links.new(t.outputs['Color'], nm.inputs['Color'])
        nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
    if orm:
        t = nt.nodes.new('ShaderNodeTexImage'); t.image = bpy.data.images.load(orm, check_existing=True)
        try: t.image.colorspace_settings.name = 'Non-Color'
        except Exception: pass
        sep, gch, bch = sep_rgb_node(nt)
        nt.links.new(t.outputs['Color'], sep.inputs[0])
        nt.links.new(sep.outputs[gch], bsdf.inputs['Roughness'])
        nt.links.new(sep.outputs[bch], bsdf.inputs['Metallic'])

def flat_mat(mat, rgba, rough=0.5, metal=0.0):
    # untextured material driven only by factors (eyes/teeth have no PBR maps)
    mat.use_nodes = True
    nt = mat.node_tree; nt.nodes.clear()
    out  = nt.nodes.new('ShaderNodeOutputMaterial')
    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    bsdf.inputs['Base Color'].default_value = rgba
    bsdf.inputs['Roughness'].default_value  = rough
    bsdf.inputs['Metallic'].default_value   = metal

# fresh empty scene
bpy.ops.wm.read_factory_settings(use_empty=True)

import mathutils
def world_bbox(objs):
    mn = mathutils.Vector(( 1e18, 1e18, 1e18))
    mx = mathutils.Vector((-1e18,-1e18,-1e18))
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ mathutils.Vector(c)
            for i in range(3):
                if w[i] < mn[i]: mn[i] = w[i]
                if w[i] > mx[i]: mx[i] = w[i]
    return mn, mx

helmet_objs = []
armor_objs  = []
total_meshes = 0
for part, fbx, mapdir in PARTS:
    fbx_path = os.path.join(SRC, fbx).replace("\\", "/")
    if not os.path.exists(fbx_path):
        log("MISSING fbx:", fbx_path); continue
    base = find_tex(mapdir, "BaseColor")
    nrm  = find_tex(mapdir, "Normal")
    orm  = find_tex(mapdir, "OcclusionRoughnessMetallic")
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=fbx_path)
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == 'MESH']
    if any(re.search(r'lod\d', o.name.lower()) for o in meshes):   # drop LOD1..7, keep LOD0
        meshes = [o for o in meshes if is_top_lod(o.name)]
    for m in meshes:
        # static bind pose: drop armature deformation, keep world position
        for mod in list(m.modifiers):
            if mod.type == 'ARMATURE': m.modifiers.remove(mod)
        bpy.ops.object.select_all(action='DESELECT')
        m.select_set(True); bpy.context.view_layer.objects.active = m
        if m.parent: bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
        if not m.data.materials:
            m.data.materials.append(bpy.data.materials.new(part + "_mat"))
        for slot in m.material_slots:
            if slot.material: build_mat(slot.material, base, nrm, orm)
    # remove everything not kept: armatures, empties, AND dropped LOD meshes
    keep = set(meshes)
    for o in new:
        if o not in keep:
            try: bpy.data.objects.remove(o, do_unlink=True)
            except Exception: pass
    if part == "Helmet": helmet_objs += meshes
    else:                armor_objs  += meshes
    total_meshes += len(meshes)
    log(f"{part}: imported {len(meshes)} mesh(es); base={bool(base)} nrm={bool(nrm)} orm={bool(orm)}")

log("total meshes:", total_meshes)

# ── Head: the FBX is a MetaHuman rig — 370 objects, but only a few are real anatomy.
# Keep the head skin (+ eyes/teeth) and drop all the CTRL_*/FRM_*/GUI rig gizmos.
# The head is exported at its native world transform (translation 0,0,0), so it seats
# directly on the neck — NO repositioning needed (unlike the helmet prop below).
HEAD_FBX = "uploads_files_6840509_SK_Gladiator_Head.fbx"
KEEP_HEAD = {"head_lod0_mesh", "eyeLeft_lod0_mesh", "eyeRight_lod0_mesh", "teeth_lod0_mesh"}
head_objs  = []
head_skin  = []
head_path = os.path.join(SRC, HEAD_FBX).replace("\\", "/")
if os.path.exists(head_path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=head_path)
    new = [o for o in bpy.data.objects if o not in before]
    keep = [o for o in new if o.type == 'MESH' and o.name in KEEP_HEAD]
    hbase = find_tex("Head", "BaseColor")
    hnrm  = find_tex("Head", "Normal")
    horm  = find_tex("Head", "OcclusionRoughnessMetallic")
    for m in keep:
        for mod in list(m.modifiers):
            if mod.type == 'ARMATURE': m.modifiers.remove(mod)
        bpy.ops.object.select_all(action='DESELECT')
        m.select_set(True); bpy.context.view_layer.objects.active = m
        if m.parent: bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
        if not m.data.materials:
            m.data.materials.append(bpy.data.materials.new(m.name + "_mat"))
        for slot in m.material_slots:
            if not slot.material: continue
            if   m.name == "head_lod0_mesh":  build_mat(slot.material, hbase, hnrm, horm)
            elif m.name == "teeth_lod0_mesh": flat_mat(slot.material, (0.82, 0.79, 0.74, 1), rough=0.35)
            else:                             flat_mat(slot.material, (0.20, 0.15, 0.11, 1), rough=0.10)  # dark wet eye
    keepset = set(keep)
    for o in new:
        if o not in keepset:
            try: bpy.data.objects.remove(o, do_unlink=True)
            except Exception: pass
    head_objs = keep
    head_skin = [o for o in keep if o.name == "head_lod0_mesh"]
    log(f"Head: kept {[o.name for o in keep]}  base={bool(hbase)} nrm={bool(hnrm)} orm={bool(horm)}")

# The helmet is a socket-attached prop centered at the origin. Seat it on the head:
# centre it on the head skin's bounding box (X/Y), then lift it so it caps the crown.
if helmet_objs and (head_skin or armor_objs):
    hmn, hmx = world_bbox(helmet_objs)
    if head_skin:
        rmn, rmx = world_bbox(head_skin)
        up = 2                                              # Blender Z-up (export converts to Y-up)
        Hh = rmx[up] - rmn[up]
        target = (rmn + rmx) * 0.5
        target[up] = (rmn[up] + rmx[up]) * 0.5 + 0.12 * Hh  # lift to sit on the crown
        ref = "head"
    else:
        amn, amx = world_bbox(armor_objs)
        up = max(range(3), key=lambda i: amx[i] - amn[i])
        target = mathutils.Vector(((amn[0]+amx[0])*0.5, (amn[1]+amx[1])*0.5, (amn[2]+amx[2])*0.5))
        target[up] = amn[up] + 0.92 * (amx[up] - amn[up])
        ref = "body"
    delta = target - (hmn + hmx) * 0.5
    for o in helmet_objs: o.location += delta
    log(f"helmet repositioned by {tuple(round(v,2) for v in delta)} (ref={ref})")

out_path = OUT + "/Gladiator.gltf"
bpy.ops.export_scene.gltf(
    filepath=out_path,
    export_format='GLTF_SEPARATE',
    export_skins=False,
    export_animations=False,
    export_yup=True,
    use_selection=False,
    export_apply=True,
)
log("EXPORTED:", out_path)
LOG.close()
