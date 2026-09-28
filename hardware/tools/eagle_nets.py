import sys, xml.etree.ElementTree as ET, collections
t = ET.parse(sys.argv[1]); r = t.getroot()
parts = {p.get('name'): (p.get('library'), p.get('deviceset'), p.get('value')) for p in r.iter('part')}
nets = collections.defaultdict(set)
for net in r.iter('net'):
    for pr in net.iter('pinref'):
        nets[net.get('name')].add((pr.get('part'), pr.get('gate'), pr.get('pin')))
mode = sys.argv[2]
if mode == 'parts':
    for n,(l,d,v) in sorted(parts.items()): print(n, l, d, v)
elif mode == 'part':
    target = sys.argv[3]
    rows = []
    for name, refs in nets.items():
        for (p,g,pin) in refs:
            if p == target:
                others = sorted(f"{q}.{pn}" for (q,_,pn) in refs if q != target)
                rows.append((pin, name, ' '.join(others)[:110]))
    for row in sorted(rows): print(*row, sep=' | ')
elif mode == 'net':
    for name, refs in nets.items():
        if sys.argv[3].lower() in name.lower():
            print(name, '->', sorted(f"{p}.{pin}" for p,_,pin in refs))
