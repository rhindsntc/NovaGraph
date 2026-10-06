#!/usr/bin/env bash
set -euo pipefail
# Arguments stay data; never interpolate the query into executable source.
python3 - "${1:-find nodes Person limit 10}" "${2:-8080}" <<'PY'
import json, re, sys, urllib.request, urllib.error
query, port = sys.argv[1:]
if not port.isdecimal() or not 1 <= int(port) <= 65535:
    sys.exit('Expected a numeric loopback port (1...65535).')
origin = f'http://127.0.0.1:{int(port)}'
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs): return None
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect)
try:
    with opener.open(origin, timeout=10) as response:
        html = response.read(4*1024*1024).decode()
    token = re.search(r'name="nova-session" content="([A-Fa-f0-9-]+)"',html)
    if not token: sys.exit('This origin is not a Nova local runner.')
    request = urllib.request.Request(origin+'/dev/query', data=json.dumps({'query':query}).encode(),
        headers={'Content-Type':'application/json','Origin':origin,'X-Nova-Session':token[1]})
    with opener.open(request, timeout=10) as response: print(response.read().decode())
except urllib.error.HTTPError as error:
    print(error.read().decode(),file=sys.stderr);sys.exit(1)
except (OSError, ValueError) as error:
    sys.exit(f'Runner unavailable: {error}')
PY
