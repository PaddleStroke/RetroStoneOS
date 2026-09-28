"""Print board positions of named parts from an Eagle .brd (XML). Usage: eagle_parts_pos.py <brd> NAME..."""
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
want = set(sys.argv[2:])
for e in root.iter('element'):
    if not want or e.get('name') in want:
        print(f"{e.get('name'):10} x={float(e.get('x')):7.2f} y={float(e.get('y')):7.2f} rot={e.get('rot') or 'R0':6} pkg={e.get('package')}")
