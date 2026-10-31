"""Run with UE 5.6 PythonScript commandlet. Refuses to overwrite saved assets."""
import json
import struct
import zlib
from pathlib import Path
import unreal as u

ROOT = Path(u.Paths.project_dir()).resolve()
OUT = ROOT / 'Artifacts'
OUT.mkdir(parents=True, exist_ok=True)
M = u.MaterialEditingLibrary
TOOLS = u.AssetToolsHelpers.get_asset_tools()
PATHS = ['/Game/Mods/MiniMap/Materials/M_BuildingCompose',
         '/Game/Mods/MainMap/Materials/M_BuildingPresent',
         '/Game/Mods/MiniMap/Materials/T_ComposeDefault',
         '/Game/Mods/MainMap/Materials/T_PresentDefault']
assert not any(u.EditorAssetLibrary.does_asset_exist(p) for p in PATHS[:2]), 'Existing material: refusing overwrite'

def default_texture(path, rgba, srgb):
    if u.EditorAssetLibrary.does_asset_exist(path):
        tex = u.load_asset(path)
        assert isinstance(tex, u.Texture2D) and tex.get_editor_property('srgb') == srgb
        return tex  # Resume after graph-authoring failure without overwriting saved dependencies.
    src = ROOT / 'Sources' / (path.rsplit('/', 1)[1] + '.png')
    src.parent.mkdir(exist_ok=True)
    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b'\x00' + bytes(rgba))) + chunk(b'IEND', b'')
    src.write_bytes(png)
    task = u.AssetImportTask()
    task.filename = str(src)
    task.destination_path, task.destination_name = path.rsplit('/', 1)
    task.automated = True
    task.save = False
    task.replace_existing = False
    TOOLS.import_asset_tasks([task])
    tex = u.load_asset(path)
    assert isinstance(tex, u.Texture2D)
    tex.set_editor_property('srgb', srgb)
    tex.set_editor_property('compression_settings', u.TextureCompressionSettings.TC_DEFAULT)
    tex.set_editor_property('mip_gen_settings', u.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    tex.set_editor_property('filter', u.TextureFilter.TF_BILINEAR)
    tex.set_editor_property('address_x', u.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', u.TextureAddress.TA_CLAMP)
    assert u.EditorAssetLibrary.save_loaded_asset(tex)
    return tex

white = default_texture(PATHS[2], (255,255,255,255), True)
clear = default_texture(PATHS[3], (0,0,0,255), False)
graphs = []

class Graph:
    def __init__(self, path, domain):
        folder, name = path.rsplit('/', 1)
        self.mat = TOOLS.create_asset(name, folder, u.Material, u.MaterialFactoryNew())
        assert self.mat
        self.mat.set_editor_property('material_domain', domain)
        self.mat.set_editor_property('blend_mode', u.BlendMode.BLEND_ALPHA_COMPOSITE)
        self.nodes = {}
        self.record = {'path': path, 'nodes': {}, 'edges': [], 'outputs': {}}
        graphs.append(self)

    def node(self, name, cls, **props):
        obj = M.create_material_expression(self.mat, getattr(u, 'MaterialExpression' + cls), -1200 + len(self.nodes)*75, len(self.nodes)*45)
        assert obj
        for key, value in props.items():
            obj.set_editor_property(key, value)
        obj.set_editor_property('desc', name)
        self.nodes[name] = obj
        self.record['nodes'][name] = {'class': cls, 'properties': {k: str(v) for k,v in props.items()}}
        return name

    def wire(self, source, target, pin, output=''):
        assert M.connect_material_expressions(self.nodes[source], output, self.nodes[target], pin), (source,target,pin,output)
        self.record['edges'].append([source, output, target, pin])

    def output(self, node, prop, output=''):
        assert M.connect_material_property(self.nodes[node], output, prop)
        self.record['outputs'][str(prop)] = [node,output]

    def unary(self, name, cls, source, output='', **props):
        self.node(name, cls, **props)
        self.wire(source, name, '', output)
        return name

    def binary(self, name, cls, a, b=None, aout='', bout='', **props):
        self.node(name, cls, **props)
        self.wire(a, name, 'A', aout)
        if b:
            self.wire(b, name, 'B', bout)
        return name

g = Graph(PATHS[0], u.MaterialDomain.MD_SURFACE)
for key, value in {'shading_model': u.MaterialShadingModel.MSM_UNLIT, 'two_sided': True,
                   'disable_depth_test': True, 'use_translucency_vertex_fog': False,
                   'apply_cloud_fogging': False, 'compute_fog_per_pixel': False,
                   'output_translucent_velocity': False}.items():
    g.mat.set_editor_property(key, value)
g.node('UV0', 'TextureCoordinate', coordinate_index=0)
g.node('SpriteAtlas', 'TextureSampleParameter2D', parameter_name='SpriteAtlas', texture=white,
       sampler_type=u.MaterialSamplerType.SAMPLERTYPE_COLOR, automatic_view_mip_bias=False)
g.wire('UV0', 'SpriteAtlas', 'UVs')
g.node('VertexColor', 'VertexColor')
g.unary('BoundTint', 'Saturate', 'VertexColor')
g.node('Threshold', 'Constant', r=0.04045)
decoded = []
for channel in 'RGB':
    v = g.unary('Encoded'+channel, 'ComponentMask', 'BoundTint', r=channel=='R', g=channel=='G', b=channel=='B', a=False)
    low = g.binary('Low'+channel, 'Divide', v, const_b=12.92)
    add = g.binary('Offset'+channel, 'Add', v, const_b=0.055)
    div = g.binary('Scale'+channel, 'Divide', add, const_b=1.055)
    high = g.node('High'+channel, 'Power', const_exponent=2.4)
    g.wire(div, high, 'Base')
    name = g.node('Decode'+channel, 'If', equals_threshold=0.0)
    for src, pin in [(v,'A'), ('Threshold','B'), (high,'A > B'), (low,'A == B'), (low,'A < B')]:
        g.wire(src, name, pin)
    decoded.append(name)
rg = g.binary('LinearRG', 'AppendVector', decoded[0], decoded[1])
tint = g.binary('LinearTint', 'AppendVector', rg, decoded[2])
aq = g.binary('AlphaTimesVertexAlpha', 'Multiply', 'SpriteAtlas', 'VertexColor', aout='A', bout='A')
s = g.unary('Coverage', 'Saturate', aq)
ct = g.binary('TintedLinearColor', 'Multiply', 'SpriteAtlas', tint, aout='RGB')
em = g.binary('AssociatedColor', 'Multiply', ct, s)
g.output(em, u.MaterialProperty.MP_EMISSIVE_COLOR)
g.output(s, u.MaterialProperty.MP_OPACITY)

g = Graph(PATHS[1], u.MaterialDomain.MD_UI)
g.node('UV0', 'TextureCoordinate', coordinate_index=0)
g.node('BuildingTarget', 'TextureSampleParameter2D', parameter_name='BuildingTarget', texture=clear,
       sampler_type=u.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, automatic_view_mip_bias=False)
g.wire('UV0', 'BuildingTarget', 'UVs')
inv = g.unary('OneMinusTransmittance', 'OneMinus', 'BuildingTarget', output='A')
opacity = g.unary('PresentationCoverage', 'Saturate', inv)
g.output('BuildingTarget', u.MaterialProperty.MP_EMISSIVE_COLOR, output='RGB')
g.output(opacity, u.MaterialProperty.MP_OPACITY)

for g in graphs:
    M.layout_material_expressions(g.mat)
    M.recompile_material(g.mat)
    assert u.EditorAssetLibrary.save_loaded_asset(g.mat)
    assert M.get_num_material_expressions(g.mat) == len(g.nodes)
    names = [str(x) for x in M.get_texture_parameter_names(g.mat)]
    expected = 'SpriteAtlas' if 'Compose' in g.mat.get_name() else 'BuildingTarget'
    assert names == [expected], names
    g.record['verified_parameters'] = names
    g.record['expression_count'] = M.get_num_material_expressions(g.mat)
    task = u.AssetExportTask()
    task.object = g.mat
    task.filename = str(OUT / (g.mat.get_name() + '.t3d'))
    task.exporter = u.ObjectExporterT3D()
    task.automated = True
    task.prompt = False
    assert u.Exporter.run_asset_export_task(task), 'T3D export failed'
(OUT / 'authored-graphs.json').write_text(json.dumps([g.record for g in graphs], indent=2))
u.log('A2.2: two parent materials authored and saved; graph exports completed.')
