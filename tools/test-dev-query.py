#!/usr/bin/env python3
"""Exercise the real HTTP server, engine and filesystem lifecycle (no mocked DB)."""
import contextlib, http.client, json, os, pathlib, re, socket, subprocess, sys, tempfile, time, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARY = os.environ.get('NOVA_DEV_BINARY', str(ROOT/'devtools/.build/debug/NovaDevQuery'))

class Server:
    def __init__(self, database=None):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0)); self.port = s.getsockname()[1]
        self.origin = f'http://127.0.0.1:{self.port}'
        self.log = tempfile.TemporaryFile()
        args = [BINARY, '--port', str(self.port), '--web-root', str(ROOT/'docs-web/dist')]
        if database: args += ['--database', str(database)]
        self.process = subprocess.Popen(args, stdout=self.log, stderr=self.log,
            env={**os.environ, 'NOVA_DEV_PORT':str(self.port)}, cwd=ROOT)
        self.token = ''
        for _ in range(150):
            if self.process.poll() is not None:
                self.log.seek(0); raise RuntimeError(self.log.read().decode())
            try:
                status, _, html = self.call('/', method='GET', authenticated=False)
                if status == 200:
                    token = re.search(r'name="nova-session" content="([A-Fa-f0-9-]+)"', html)
                    self.token = token[1] if token else ''
                    return
            except OSError: pass
            time.sleep(.04)
        self.stop(); raise RuntimeError('server did not start')
    def call(self, path='/dev/query', payload=None, method='POST', authenticated=True, headers=None, raw=None):
        h = {'Content-Type':'application/json'}
        if authenticated: h.update(Origin=self.origin, **{'X-Nova-Session':self.token})
        if headers: h.update(headers)
        c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=12)
        try:
            c.request(method,path,body=raw if raw is not None else json.dumps(payload or {}),headers=h)
            r=c.getresponse(); return r.status, dict(r.getheaders()), r.read().decode()
        finally: c.close()
    def json(self, path='/dev/query', payload=None, **kwargs):
        status, headers, text=self.call(path,payload,**kwargs)
        return status, headers, json.loads(text)
    def query(self,q): return self.json(payload={'query':q})[2]
    def stop(self):
        self.process.terminate()
        try: self.process.wait(timeout=10)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait(); raise
        self.log.seek(0); output=self.log.read().decode(); self.log.close()
        if self.process.returncode != 0: raise RuntimeError(f"runner shutdown failed ({self.process.returncode}): {output}")
    def __enter__(self): return self
    def __exit__(self,*_): self.stop()

def normalized(value):
    if isinstance(value, list): return [normalized(x) for x in value]
    if isinstance(value, dict):
        return {k: 0 if k in ('last_read_ms','last_modified_ms') else '<transaction>' if k=='transactionId' else normalized(v) for k,v in value.items()}
    return value

class HTTPContracts(unittest.TestCase):
    def test_denied_origin_cannot_mutate(self):
        with Server() as s:
            status, headers, result=s.json(payload={'query':'upsert node N forbidden'},headers={'Origin':'https://attacker.test'})
            self.assertEqual(status,403); self.assertFalse(result['ok'])
            self.assertNotIn('access-control-allow-origin', {k.lower():v for k,v in headers.items()})
            self.assertEqual(s.query('get node forbidden')['error']['code'],'notFound')
    def test_auth_host_methods_and_body_limits(self):
        with Server() as s:
            self.assertTrue(s.token)
            for headers in ({'X-Nova-Session':'wrong'},{'Origin':'null'},{'Host':'attacker.test'},{'Origin':s.origin+'/'}):
                self.assertEqual(s.call(payload={'query':'upsert node N forbidden'},headers=headers)[0],403)
            self.assertEqual(s.call(authenticated=False)[0],403)
            self.assertEqual(s.call(method='OPTIONS')[0],405)
            self.assertEqual(s.call(raw='x'*65537)[0],413)
            self.assertEqual(s.call(raw='broken json')[0],400)
            self.assertEqual(s.call(headers={'Content-Type':'text/plain'})[0],415)
            self.assertEqual(s.query('get node forbidden')['error']['code'],'notFound')
    def test_chunked_body_and_slow_incomplete_request_are_bounded(self):
        with Server() as s:
            self.assertEqual(s.call(raw=iter([b'x'*40000,b'x'*40000]))[0],413)
            with socket.create_connection(('127.0.0.1',s.port),timeout=12) as connection:
                connection.sendall(b'POST /dev/query HTTP/1.1\r\nHost: ')
                self.assertIn(b'408 Request Timeout',connection.recv(4096))
            self.assertTrue(s.query('find nodes N limit 1')['ok'])

    def test_cli_passes_quoted_query_as_data(self):
        with Server() as s:
            text='triple'+chr(39)*3+' $() backtick`'
            query='upsert node N cli set text='+json.dumps(text)+'; get node cli'
            result=subprocess.run([str(ROOT/'tools/dev-query.sh'),query,str(s.port)],text=True,capture_output=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads(result.stdout)['data']['results'][1]['properties']['text']['value'],text)

    def test_empty_media_types_reject_without_crashing_or_mutating(self):
        with Server() as s:
            for value in ['', ';', ';;;', '   ', '; charset=utf-8']:
                self.assertEqual(s.call(payload={'query':'upsert node N rejected'},headers={'Content-Type':value})[0],415)
                self.assertEqual(s.query('get node rejected')['error']['code'],'notFound')

    def test_parameter_types_are_explicit_and_invalid_values_cannot_mutate(self):
        with Server() as s:
            # Ambiguous untagged numeric dictionaries are no longer accepted.
            query='upsert node N rejected set value=$value'
            for value in (1.0,9223372036854775808,9223372036854775809):
                self.assertEqual(s.json(payload={'query':query,'parameters':{'value':value}})[0],400)
            for kind,value in [('double',1.0),('int',9223372036854775807),('int',-9223372036854775808),('bool',True),('null',None),('string','quoted text')]:
                status,_,result=s.json(payload={'query':'upsert node N typed set value=$value; get node typed','parameters':[{'name':'value','type':kind,'value':value}]})
                self.assertEqual(status,200)
                self.assertEqual(result['data']['results'][1]['properties']['value'],{'type':kind,'value':value})
            for kind,value in [('int',9223372036854775808),('int',9223372036854775809),('int',1.1),('null',0),('unknown',1)]:
                self.assertEqual(s.json(payload={'query':query,'parameters':[{'name':'value','type':kind,'value':value}]})[0],400)
            duplicate=[{'name':'value','type':'int','value':1},{'name':'value','type':'int','value':2}]
            self.assertEqual(s.json(payload={'query':query,'parameters':duplicate})[0],400)
            unicode_names=[{'name':name,'type':'int','value':i} for i,name in enumerate(['é','e\u0301'])]
            self.assertEqual(s.json(payload={'query':'upsert node N rejected','parameters':unicode_names})[0],400)
            self.assertEqual(s.query('get node rejected')['error']['code'],'notFound')

    def test_reset_invalidates_cursor_even_when_new_database_reaches_same_lsn(self):
        with Server() as s:
            s.query('upsert node N z; upsert node N zz')
            page=s.json('/dev/inspect/nodes',{'limit':1})[2]['data']
            s.json('/dev/reset'); s.query('upsert node N a; upsert node N b')
            self.assertEqual(s.json('/dev/inspect/nodes',{'cursor':page['nextCursor'],'limit':1})[2]['error']['code'],'conflict')
            self.assertEqual([row['id'] for row in s.json('/dev/inspect/nodes')[2]['data']['items']],['a','b'])

    def test_static_assets_are_real_app_and_cannot_escape_root(self):
        with Server() as s:
            status,headers,html=s.call('/',method='GET',authenticated=False)
            self.assertEqual(status,200);self.assertIn('id="root"',html)
            self.assertIn("frame-ancestors 'none'",headers['content-security-policy'])
            asset=re.search(r'src="(/assets/[^\"]+\.js)"',html)[1]
            self.assertEqual(s.call(asset,method='GET',authenticated=False)[0],200)
            for path in ('/../Package.swift','/%2e%2e/Package.swift','/dev/session','/assets/../index.html'):
                self.assertNotEqual(s.call(path,method='GET',authenticated=False)[0],200)
    def test_parallel_assets_do_not_compete_for_engine_admission(self):
        import concurrent.futures, threading
        with Server() as s:
            html=s.call('/',method='GET',authenticated=False)[2]
            asset=re.search(r'src="(/assets/[^\"]+\.js)"',html)[1]
            barrier=threading.Barrier(8)
            def load(_):
                barrier.wait(timeout=5)
                return s.call(asset,method='GET',authenticated=False)[0]
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                self.assertEqual(list(pool.map(load,range(8))),[200]*8)

    def test_real_preset_matches_cli_and_edited_query_errors_are_typed(self):
        with Server() as s:
            actual=s.query((ROOT/'examples/ngql/playground-social.ngql').read_text())
            expected=json.loads((ROOT/'examples/expected/playground-social.json').read_text())
            self.assertEqual(normalized(actual),normalized(expected))
            self.assertTrue(s.query('upsert node N edited set exact=9223372036854775807')['ok'])
            self.assertEqual(s.query('get node edited')['data']['properties']['exact']['value'],9223372036854775807)
            error=s.query('definitely invalid NGQL')
            self.assertFalse(error['ok']);self.assertEqual(error['schemaVersion'],2)
            self.assertEqual(error['error']['code'],'parseError');self.assertIsInstance(error['error']['context'],dict)
            self.assertEqual(s.query('explain find nodes Person where email="ada@analytical.engine" limit 10')['data']['operation'],'FindNodes')
    def test_real_inspection_pages_and_conflicts(self):
        with Server() as s:
            self.assertTrue(s.query('upsert node N a; upsert node N b; upsert edge E a -> b; create index on N(name)')['ok'])
            first=s.json('/dev/inspect/nodes',{'limit':1})[2]['data']
            self.assertEqual([x['id'] for x in first['items']],['a'])
            page=s.json('/dev/inspect/nodes',{'limit':1,'cursor':first['nextCursor']})[2]['data']
            self.assertEqual([x['id'] for x in page['items']],['b']);self.assertIsNone(page['nextCursor'])
            self.assertEqual(s.json('/dev/inspect/edges')[2]['data']['items'][0]['type'],'E')
            self.assertEqual(s.json('/dev/inspect/indexes')[2]['data']['items'],[{'label':'N','property':'name'}])
            s.query('upsert node N c')
            self.assertEqual(s.json('/dev/inspect/nodes',{'cursor':first['nextCursor']})[2]['error']['code'],'conflict')
            self.assertEqual(s.json('/dev/inspect/nodes',{'limit':101})[0],400)
    def test_disposable_reset_and_restart_are_empty_persistent_restart_retains(self):
        with Server() as s:
            session=s.json('/dev/session')[2]['data'];self.assertEqual(session['mode'],'disposable')
            path=pathlib.Path(session['databasePath'])
            self.assertTrue(s.query('upsert node N transient')['ok'])
            reset=s.json('/dev/reset')[2]; self.assertTrue(reset['ok'])
            replacement_path=pathlib.Path(reset['data']['databasePath'])
            self.assertFalse(path.exists())
            self.assertEqual(s.query('get node transient')['error']['code'],'notFound')
        self.assertFalse(path.exists()); self.assertFalse(replacement_path.exists())
        with Server() as s: self.assertEqual(s.query('get node transient')['error']['code'],'notFound')
        with tempfile.TemporaryDirectory() as temp:
            path=pathlib.Path(temp)/'explicit'
            with Server(path) as s:
                self.assertEqual(s.json('/dev/session')[2]['data']['mode'],'persistent')
                self.assertTrue(s.query('upsert node N retained')['ok'])
                self.assertEqual(s.json('/dev/reset')[0],409)
            with Server(path) as s: self.assertEqual(s.query('get node retained')['data']['id'],'retained')

if __name__=='__main__':
    if '--web' in sys.argv:
        with Server() as server:
            sys.exit(subprocess.call(['npm','run','test:web','--','tests/runner.test.tsx'],cwd=ROOT/'docs-web',env={**os.environ,'NOVA_RUNNER_TEST_ORIGIN':server.origin}))
    else: unittest.main()
