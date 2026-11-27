"""Build the animation wire dictionaries from the player's own AS resources."""
from pathlib import Path

import oead


# AS element types whose input kinds are 44..62 or 64 (string selectors).
STRING_SELECTORS = {2, 3, 4, 8, 18, 20, 24, 31, 33, 38, 45, 46, 47, 50, 84, 87, 94, 97, 98, 99}
TABLE_FILE = 'coop_astree.dat'


def tables(player):
    def load(path):
        return oead.aamp.ParameterIO.from_binary(bytes(player.get_file(path).data))

    aslist = load('Actor/ASList/Player.baslist')
    definitions = {str(obj.params['Name'].v): str(obj.params['Filename'].v)
                   for obj in aslist.lists['ASDefines'].objects.values()}
    names = sorted(definitions)
    strings = set()
    for name in names:
        tree = load(f'Actor/AS/{definitions[name]}.bas')
        for element in tree.lists['Elements'].lists.values():
            try:
                kind = element.objects['Parameters'].params['TypeIndex'].v
                if kind not in STRING_SELECTORS:
                    continue
                cases = element.lists['Extend'].lists['StringArray'].objects['StringArray0']
            except KeyError:
                continue
            strings.update(value for param in cases.params.values()
                           if (value := str(param.v)) and value != 'default')
    return ['', *names], ['', *sorted(strings)]


def write_tables(player, overlay):
    names, strings = tables(player)
    path = Path(overlay) / TABLE_FILE
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix('.tmp')
    temp.write_bytes(('SWASTREE1\n' + '\n'.join(names + strings) + '\n').encode('ascii'))
    temp.replace(path)
