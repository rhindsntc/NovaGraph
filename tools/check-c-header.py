#!/usr/bin/env python3
"""Compile the same C-only export consumed by SwiftPM as an ordinary C11 client."""
import os
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Werror','-fsyntax-only','-x','c','-I'+str(root/'cpp/c_api'),'-'],input='#include "CGraphDB.h"\nvoid check(GraphDBHandle *db) { GraphDBString response = graphdb_checkpoint(db); graphdb_string_free(response); response = graphdb_checkpoint_v2(db, 0); graphdb_string_free(response); }\n',text=True,check=True)
print('Canonical C export compiles without private C++ headers.')
