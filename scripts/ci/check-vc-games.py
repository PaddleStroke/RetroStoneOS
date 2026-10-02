#!/usr/bin/env python3
"""Check that every selected built-in VC game is complete in an image."""
import configparser
import re
import sys
from pathlib import Path
import xml.etree.ElementTree as ET


def check(target, config, expected_commit=''):
    selected = set(re.findall(r'^BR2_PACKAGE_RSOS_VC_GAMES_([A-Z]+)=y$', config.read_text(), re.M))
    if 'BR2_PACKAGE_RSOS_VC_GAMES=y' not in config.read_text().splitlines():
        print('RetroStone VC games disabled')
        return
    assert selected, 'No RetroStone VC games selected'
    folder = target / 'usr/share/rsos/games/retrostone'
    licenses = target / 'usr/share/rsos/licenses/retrostone-vc'
    commit = (licenses / 'COMMIT').read_text().strip()
    assert re.fullmatch(r'[0-9a-f]{12,40}', commit), f'Uncommitted or invalid game source: {commit}'
    if expected_commit:
        assert expected_commit.startswith(commit), f'Wrong game source: {commit}, expected {expected_commit}'
    for filename in ('LICENSE-MIT', 'LICENSE-CC-BY-NC-SA-4.0.txt', 'THIRD_PARTY.md'):
        assert (licenses / filename).stat().st_size > 0, filename
    entries = {}
    for entry in ET.parse(folder / 'gamelist.xml').getroot().findall('game'):
        game = Path(entry.findtext('path')).suffix[1:]
        assert game not in entries, f'Duplicate game entry: {game}'
        entries[game] = entry
    for symbol in sorted(selected):
        game = symbol.lower()
        entry = entries[game]
        core = target / f'usr/lib/libretro/{game}_libretro.so'
        assert core.read_bytes()[:4] == b'\x7fELF', f'Missing or invalid core: {core}'
        metadata = configparser.ConfigParser()
        metadata.read(target / f'usr/share/rsos/cores/{game}.ini')
        fields = metadata['core']
        for field, expected in {'id': game, 'extensions': game, 'library': f'/usr/lib/libretro/{game}_libretro.so', 'systems': 'retrostone', 'commit': commit}.items():
            assert fields[field] == expected, f'{game}: incorrect {field}'
        assert fields.getboolean('no_content') and fields.getboolean('savestates'), game
        assert (folder / entry.findtext('path')).stat().st_size > 0, f'{game}: missing menu entry'
        assert (folder / entry.findtext('image')).read_bytes()[:8] == b'\x89PNG\r\n\x1a\n', f'{game}: missing title PNG'
        assert (licenses / game / 'LICENSE').stat().st_size > 0, f'{game}: missing licence'
        assert entry.findtext('players') == ('1' if game == 'bombermole' else '1-4'), f'{game}: players'
        print(f'{entry.findtext("name")}: core, metadata, menu, picture, licence OK ({commit})')
    print(f'RetroStone VC: {len(selected)} installed games verified')


if __name__ == '__main__':
    check(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else '')
