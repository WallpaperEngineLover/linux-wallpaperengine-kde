#!/usr/bin/env python3
"""Scans the workshop library and writes a feature inventory.

    scan.py [ids...]        (no ids = whole library)

Writes features.json (per item: info + feature counter) and FEATURES.md (feature -> items index,
for picking regression targets) into $LWE_LIBRARY_DIR (default ~/.local/share/lwe-library).
Reads scene.pkg in memory, nothing unpacked.
"""
import collections, json, os, re, struct, sys

W = os.environ.get('LWE_WORKSHOP_DIR', '/workspace/SteamLibrary/steamapps/workshop/content/431960')
ASSETS = os.environ.get('LWE_ASSETS_DIR', '/workspace/SteamLibrary/steamapps/common/wallpaper_engine/assets')
HERE = os.environ.get('LWE_LIBRARY_DIR', os.path.expanduser('~/.local/share/lwe-library'))


def parse_json(data):
    if isinstance(data, bytes):
        data = data.decode('utf-8-sig', errors='replace')
    data = re.sub(r',(\s*[}\]])', r'\1', data)
    try:
        return json.loads(data)
    except Exception:
        return None


class Source:
    """Files of one item: scene.pkg entries, then loose files, then its dependency, then assets."""

    def __init__(self, root, dep_root=None):
        self.roots = [root] + ([dep_root] if dep_root else []) + [ASSETS]
        self.pkgs = []
        for r in self.roots[:-1]:
            for name in ('scene.pkg', 'gifscene.pkg'):
                p = os.path.join(r, name)
                if os.path.exists(p):
                    self.pkgs.append(self._index(p))

    @staticmethod
    def _index(path):
        with open(path, 'rb') as f:
            def u32():
                return struct.unpack('<I', f.read(4))[0]
            f.read(u32())
            entries = {}
            for _ in range(u32()):
                name = f.read(u32()).decode('utf-8', errors='replace').replace('\\', '/')
                entries[name] = (u32(), u32())
            return path, f.tell(), entries

    def names(self):
        out = set()
        for _, _, e in self.pkgs:
            out.update(e)
        for r in self.roots[:1]:
            for dp, _, fs in os.walk(r):
                for f in fs:
                    out.add(os.path.relpath(os.path.join(dp, f), r))
        return out

    def read(self, rel, limit=None):
        if not isinstance(rel, str):
            return None
        rel = rel.replace('\\', '/')
        for path, base, entries in self.pkgs:
            if rel in entries:
                off, ln = entries[rel]
                with open(path, 'rb') as f:
                    f.seek(base + off)
                    return f.read(ln if limit is None else min(ln, limit))
        for r in self.roots:
            p = os.path.join(r, rel)
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    return f.read(limit if limit else -1)
        return None

    def json(self, rel):
        d = self.read(rel)
        return parse_json(d) if d is not None else None


def val(v):
    return v.get('value') if isinstance(v, dict) else v


def truthy(v):
    v = val(v)
    return v not in (None, False, 0, '', '0')


def scan_scene(src, out, info):
    name = info.get('file') if info.get('type') == 'scene' and info.get('file') else 'scene.json'
    if name != 'scene.json':
        out['scenefile:' + name] += 1
    sc = src.json(name)
    if sc is None:
        out['ERROR:no-' + name] += 1
        return
    info['version'] = sc.get('version')
    g = sc.get('general', {})
    for k in ('bloom', 'hdr', 'cameraparallax', 'camerashake', 'fog', 'fogheight',
              'reflection', 'lightconfig'):
        if truthy(g.get(k)):
            out['general:' + k] += 1
    if g.get('orthogonalprojection') is None:
        out['3d-perspective'] += 1
    else:
        op = g['orthogonalprojection']
        if isinstance(op, dict) and op.get('auto'):
            out['ortho-auto'] += 1
        elif isinstance(op, dict):
            info['size'] = f"{op.get('width')}x{op.get('height')}"
    if sc.get('camera', {}).get('paths'):
        out['camera-paths'] += 1
    objs = sc.get('objects', [])
    info['objs'] = len(objs)
    ids = {o.get('id') for o in objs}
    for o in objs:
        blob = json.dumps(o, ensure_ascii=False)
        if o.get('visible') is False or (isinstance(o.get('visible'), dict) and o['visible'].get('value') is False):
            out['invisible-obj'] += 1
        if isinstance(o.get('visible'), dict) and o['visible'].get('user'):
            out['user-visible-toggle'] += 1
        if o.get('parent') is not None and o.get('parent') in ids:
            out['parented'] += 1
        if 'image' in o:
            img = o['image'] or ''
            model = src.json(img) if img else None
            if img.startswith('models/util/solidlayer'):
                out['solidlayer'] += 1
            elif 'fullscreenlayer' in img:
                out['fullscreenlayer'] += 1
            elif 'composelayer' in img:
                out['composelayer'] += 1
            elif model and model.get('puppet'):
                out['puppet'] += 1
            else:
                out['image'] += 1
            if model and model.get('autosize'):
                out['autosize'] += 1
            if model and model.get('material'):
                mat = src.json(model['material'])
                if mat:
                    for p in mat.get('passes', []):
                        for t in p.get('textures', []) or []:
                            if t and isinstance(t, str) and t.startswith('_rt_'):
                                out['material-rt-texture'] += 1
                        if p.get('usertextures'):
                            out['usertexture'] += 1
            if truthy(o.get('perspective')):
                out['image-3d-perspective'] += 1
        if 'model' in o:
            out['model3d'] += 1
        for k in ('text', 'particle', 'sound', 'light'):
            if k in o:
                out[k if k != 'light' else 'light:' + str(o.get('light'))] += 1
        if 'camera' in o or o.get('type') == 'camera':
            out['camera-object'] += 1
        if 'particle' in o:
            p = src.json(o['particle']) or {}
            if p.get('children'):
                out['particle-children'] += 1
            if 'rope' in json.dumps(p.get('renderers', [])):
                out['particle-rope'] += 1
            for sect in ('operators', 'initializers', 'emitters'):
                for x in p.get(sect, []) or []:
                    n = x.get('name')
                    if n in ('boids', 'collisionplane', 'collisionsphere', 'collisionbox', 'collisionquad', 'remapvalue',
                             'maintaindistancetocontrolpoint', 'maintaindistancebetweencontrolpoints', 'controlpointattract',
                             'vortex', 'vortex_v2', 'turbulence', 'audioprocessing', 'hsvcolorrandom', 'colorlist',
                             'capvelocity', 'reducemovementnearcontrolpoint', 'inheritvaluefromevent', 'mapsequencebetweencontrolpoints',
                             'mapsequencearoundcontrolpoint', 'boxrandom', 'sphererandom'):
                        out['pcomp:' + n] += 1
            if o.get('instanceoverride'):
                out['particle-instanceoverride'] += 1
        if o.get('copybackground') is False:
            out['copybackground=false'] += 1
        if o.get('passthrough'):
            out['passthrough'] += 1
        if o.get('solid') is not None or o.get('disablepropagation'):
            out['cursor-solid/propagation'] += 1
        pd = o.get('parallaxDepth')
        if isinstance(pd, str) and pd.strip() not in ('0 0', '1 1', '0.00000 0.00000', '1.00000 1.00000'):
            out['parallaxDepth'] += 1
        if val(o.get('colorBlendMode')) not in (None, 0):
            out['colorBlendMode'] += 1
        n = blob.count('"script"')
        if n:
            out['scripts'] += n
            if 'createLayer' in blob:
                out['script:createLayer'] += 1
            if 'getVideoTexture' in blob:
                out['script:videoTexture'] += 1
            if 'registerAudioBuffers' in blob:
                out['script:audio'] += 1
            if 'cursor' in blob.lower():
                out['script:cursor'] += 1
            if 'mediaPlayback' in blob or 'mediaThumbnail' in blob or 'mediaProperties' in blob:
                out['script:media'] += 1
        if '"animation"' in blob:
            out['prop-animation'] += 1
        if 'animationlayers' in blob:
            out['puppet-animationlayers'] += 1
        if '"user"' in blob:
            out['user-bound-values'] += 1
        for e in o.get('effects', []) or []:
            f = e.get('file', '')
            m = re.match(r'effects/(workshop/\d+/)?([^/]+)/', f)
            name = m.group(2) if m else f
            if m and m.group(1):
                out['workshop-effect'] += 1
                name = 'ws:' + name
            elif src.read(f, 1) is not None and not os.path.exists(os.path.join(ASSETS, f)):
                name += '*'
            out['fx:' + name] += 1
            if isinstance(e.get('visible'), dict) and e['visible'].get('user'):
                out['user-effect-toggle'] += 1
    names = src.names()
    shaders = [n for n in names if n.startswith('shaders/') and n.endswith(('.frag', '.vert'))]
    if shaders:
        out['custom-shaders'] = len(shaders)
    for d in ('videos', 'sounds', 'fonts', 'scripts'):
        n = len([x for x in names if x.startswith(d + '/')])
        if n:
            out['dir:' + d] = n
    vt = gif = 0
    for n in names:
        if n.startswith('materials/') and n.endswith('.tex'):
            head = src.read(n, 4096) or b''
            if b'ftyp' in head:
                vt += 1
            elif head[:9] == b'TEXV0005\x00' and b'TEXS' in head[:200]:
                gif += 1
    if vt:
        out['video-tex'] = vt


def scan_web(root, out):
    for dp, _, fs in os.walk(root):
        for f in fs:
            if f.endswith(('.js', '.html', '.htm')):
                try:
                    t = open(os.path.join(dp, f), encoding='utf-8', errors='replace').read()
                except OSError:
                    continue
                for k in ('wallpaperRegisterAudioListener', 'wallpaperPropertyListener', 'wallpaperRegisterMediaPropertiesListener',
                          'wallpaperRegisterMediaThumbnailListener', 'wallpaperRegisterMediaPlaybackListener', 'webgl', 'WebGL',
                          'THREE.', 'PIXI', 'createjs', 'AudioContext', 'requestAnimationFrame', '<video', 'localStorage'):
                    if k in t:
                        out['web:' + k.strip('<.').lower()] += 1
    out.update({k: 1 for k in list(out) if k.startswith('web:')})


def scan(id_):
    root = os.path.join(W, id_)
    proj = parse_json(open(os.path.join(root, 'project.json'), 'rb').read()) or {}
    info = {'title': proj.get('title'), 'type': (proj.get('type') or '').lower(), 'dep': proj.get('dependency'),
            'file': proj.get('file'), 'rating': proj.get('contentrating')}
    if not info['type'] and proj.get('dependency'):
        info['type'] = 'preset'
    out = collections.Counter()
    dep_root = os.path.join(W, proj['dependency']) if proj.get('dependency') else None
    if dep_root and not os.path.isdir(dep_root):
        out['ERROR:missing-dependency'] += 1
        dep_root = None
    if info['type'] == 'scene' or (info['type'] == 'preset' and dep_root):
        if info['type'] == 'preset':
            out['preset'] += 1
            dp = parse_json(open(os.path.join(dep_root, 'project.json'), 'rb').read()) or {}
            info['dep_type'] = (dp.get('type') or '').lower()
        if info['type'] == 'scene' or info.get('dep_type') == 'scene':
            scan_scene(Source(root, dep_root), out, info)
    elif info['type'] == 'web':
        scan_web(root, out)
    elif info['type'] == 'video':
        f = proj.get('file') or ''
        out['video:' + os.path.splitext(f)[1].lower()] += 1
    props = (proj.get('general') or {}).get('properties') or {}
    info['props'] = len(props)
    for p in props.values():
        if isinstance(p, dict) and p.get('type') in ('usershortcut', 'file', 'directory', 'scenetexture', 'textinput', 'combo'):
            out['prop:' + p['type']] += 1
    if (proj.get('general') or {}).get('supportsaudioprocessing'):
        out['supportsaudioprocessing'] += 1
    if proj.get('preset'):
        out['has-preset-values'] += 1
    try:
        info['size_mb'] = round(sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(root) for f in fs) / 1e6, 1)
        info['mtime'] = int(max(os.path.getmtime(os.path.join(root, f)) for f in os.listdir(root)))
    except (OSError, ValueError):
        pass
    return info, out


def main():
    os.makedirs(HERE, exist_ok=True)
    path = os.path.join(HERE, 'features.json')
    db = json.load(open(path)) if os.path.exists(path) else {}
    ids = sys.argv[1:] or sorted(x for x in os.listdir(W) if os.path.exists(os.path.join(W, x, 'project.json')))
    if not sys.argv[1:]:
        for gone in set(db) - set(ids):
            print('removed from library:', gone, db[gone]['info'].get('title'))
            del db[gone]
    for id_ in ids:
        try:
            info, feats = scan(id_)
        except Exception as e:
            info, feats = {'title': '?', 'type': '?'}, collections.Counter({'ERROR:scan ' + type(e).__name__: 1})
        if id_ not in db:
            print('new:', id_, info.get('title'))
        db[id_] = {'info': info, 'features': dict(sorted(feats.items()))}
    json.dump(db, open(path, 'w'), indent=1, ensure_ascii=False, sort_keys=True)

    index = collections.defaultdict(list)
    for id_, d in db.items():
        for k in d['features']:
            index[re.sub(r'\*$', '', k)].append(id_)
    types = collections.Counter(d['info'].get('type') for d in db.values())
    with open(os.path.join(HERE, 'FEATURES.md'), 'w') as f:
        f.write(f'# Library features (generated by scan.py, {len(db)} items: '
                + ', '.join(f'{v} {k}' for k, v in types.most_common()) + ')\n\n')
        f.write('## Feature -> items (rarest first; `fx:name*` in features.json = effect shipped by the wallpaper)\n\n')
        for k, v in sorted(index.items(), key=lambda kv: (len(kv[1]), kv[0])):
            f.write(f'- **{k}** ({len(v)}): {" ".join(sorted(v))}\n')
        f.write('\n## Items\n\n')
        for id_, d in sorted(db.items()):
            i = d['info']
            extra = ' '.join(f'{k}={i[k]}' for k in ('version', 'objs', 'size', 'dep') if i.get(k) is not None)
            f.write(f'- {id_} [{i.get("type")}] {i.get("title")} ({extra})\n  '
                    + ', '.join(f'{k}{"x" + str(v) if v > 1 else ""}' for k, v in d['features'].items()) + '\n')


if __name__ == '__main__':
    main()
