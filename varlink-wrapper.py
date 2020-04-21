#!/usr/bin/env python3

import sys
import os

path_in = sys.argv[1]
path_out = sys.argv[2]
interface = os.path.basename(path_in).replace('.', '_')

with open(path_out, 'wt') as output:
    print('static const char *{} ='.format(interface), end='\n"', file=output)
    print(*open(path_in).read().split('\n'), sep='\\n"\n"', end='";', file=output)
