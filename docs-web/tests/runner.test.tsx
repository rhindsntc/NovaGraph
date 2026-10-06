import React from 'react';
import { transferableAbortController } from 'node:util';
import { createServer } from 'node:net';
import { afterAll, beforeAll, beforeEach, expect, test, vi } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { Playground } from '../src/components/Playground';
import expected from '../../examples/expected/playground-social.json';

const origin = process.env.NOVA_RUNNER_TEST_ORIGIN;
const nativeFetch = globalThis.fetch;
function transport(input: RequestInfo | URL, init?: RequestInit) {
 const controller=transferableAbortController(); const abort=()=>controller.abort();
 if(init?.signal?.aborted) abort(); else init?.signal?.addEventListener('abort',abort,{once:true});
 return nativeFetch(new URL(String(input),origin),{...init,signal:controller.signal,headers:{...init?.headers,Origin:origin!}}).finally(()=>init?.signal?.removeEventListener('abort',abort));
}
let token = '';
const normalize = (value: any): any => Array.isArray(value) ? value.map(normalize) : value && typeof value === 'object' ?
 Object.fromEntries(Object.entries(value).map(([key,entry])=>[key,key==='last_read_ms'||key==='last_modified_ms'?0:key==='transactionId'?'<transaction>':normalize(entry)])) : value;
if(origin) {
 beforeAll(async()=>{
  const html=await (await nativeFetch(origin)).text(); token=html.match(/name="nova-session" content="([A-Fa-f0-9-]+)"/)![1];
  document.head.insertAdjacentHTML('beforeend',`<meta name="nova-session" content="${token}"><meta name="nova-mode" content="disposable">`);
 });
 beforeEach(async()=>{
  await nativeFetch(`${origin}/dev/reset`,{method:'POST',headers:{Origin:origin,'X-Nova-Session':token,'Content-Type':'application/json'},body:'{}'});
  // Node's fetch supplies the transport; a real browser adds its Origin header automatically.
  vi.spyOn(globalThis,'fetch').mockImplementation(transport);
 });
 afterAll(()=>document.querySelectorAll('meta[name^="nova-"]').forEach(el=>el.remove()));
}
const liveTest = origin ? test : test.skip;
liveTest('explicit live connection executes preset and edited query against the actual HTTP engine',async()=>{
 const user=userEvent.setup();render(<Playground/>);
 expect(screen.getByRole('textbox').hasAttribute('readonly')).toBe(true);
 expect(globalThis.fetch).not.toHaveBeenCalled();
 await user.click(screen.getByRole('button',{name:'Connect local runner'}));
 await screen.findByText('Live · disposable');
 await user.click(screen.getByRole('button',{name:'Run query'}));
 await waitFor(()=>expect(screen.getByTestId('query-output').textContent).toContain('committedLSN'));
 expect(normalize(JSON.parse(screen.getByTestId('query-output').textContent!))).toEqual(normalize(expected));
 fireEvent.change(screen.getByRole('textbox'),{target:{value:'upsert node N edited set exact=9223372036854775807; get node edited'}});
 await user.click(screen.getByRole('button',{name:'Run query'}));
 await waitFor(()=>expect(screen.getByTestId('query-output').textContent).toContain('9223372036854775807'));
 expect(screen.getByTestId('query-output').textContent).not.toContain('9223372036854776000');
});
liveTest('invalid NGQL and a dropped TCP connection clear the previous result and report the real failure',async()=>{
 const user=userEvent.setup();render(<Playground/>);await user.click(screen.getByRole('button',{name:'Connect local runner'}));await screen.findByText('Live · disposable');
 fireEvent.change(screen.getByRole('textbox'),{target:{value:'upsert node N previous; get node previous'}});
 await user.click(screen.getByRole('button',{name:'Run query'}));await waitFor(()=>expect(screen.getByTestId('query-output').textContent).toContain('previous'));
 fireEvent.change(screen.getByRole('textbox'),{target:{value:'not valid NGQL'}});
 await user.click(screen.getByRole('button',{name:'Run query'}));await screen.findByRole('alert');
 expect(screen.getByRole('alert').textContent).toContain('parseError');
 expect(screen.getByTestId('query-output').textContent).not.toContain('previous');
 expect(screen.queryByRole('button',{name:'Select node previous'})).toBeNull();
 let connections=0; const probe=createServer(socket=>{connections++;socket.destroy();});
 await new Promise<void>(resolve=>probe.listen(0,'127.0.0.1',resolve));
 // This must attempt real TCP I/O before reporting unavailable.
 const address=probe.address(); if(!address || typeof address==='string') throw new Error('Missing probe address');
 vi.mocked(globalThis.fetch).mockImplementation((input,init)=>transport(new URL(String(input),`http://127.0.0.1:${address.port}`),init));
 await user.click(screen.getByRole('button',{name:'Run query'}));
 await waitFor(()=>expect(screen.getByRole('alert').textContent).toContain('Runner unavailable'));
 expect(screen.getByTestId('query-output').textContent).toBe('');
 expect(screen.queryByRole('button',{name:'Run query'})).toBeNull();
 await new Promise<void>(resolve=>probe.close(()=>resolve()));
 expect(connections).toBeGreaterThan(0);
});
liveTest('actual paths, projections, explain, and paginated indexes/nodes/edges render without simulation',async()=>{
 const user=userEvent.setup();render(<Playground/>);await user.click(screen.getByRole('button',{name:'Connect local runner'}));await screen.findByText('Live · disposable');
 const run=async(query:string)=>{fireEvent.change(screen.getByRole('textbox'),{target:{value:query}});await user.click(screen.getByRole('button',{name:'Run query'}));await waitFor(()=>expect(screen.getByRole('button',{name:'Run query'}).hasAttribute('disabled')).toBe(false));};
 await run('upsert node N a set name="Ada"; upsert node N b; upsert edge E a -> b; create index on N(name); walk from a over E depth 1 limit 10 paths');
 expect(screen.getByTestId('query-output').textContent).toContain('paths');
 await user.click(screen.getByRole('button',{name:'Result graph'}));expect(screen.getByRole('button',{name:'Select node a'})).toBeTruthy();
 await run('find nodes N where name="Ada" return id,name limit 10');expect(screen.getByTestId('query-output').textContent).toContain('Ada');
 await run('explain find nodes N where name="Ada" limit 10');expect(screen.getByTestId('query-output').textContent).toContain('PropertyIndexScan');
 for(const kind of ['nodes','edges','indexes']) {
  await user.click(screen.getByRole('button',{name:`Inspect ${kind}`}));
  await waitFor(()=>expect(screen.getByTestId('inspection-output').textContent).toContain(`"kind":"${kind}"`));
 }
 await run(Array.from({length:52},(_,i)=>`upsert node Page p${String(i).padStart(2,'0')}`).join(';'));
 await user.click(screen.getByRole('button',{name:'Inspect nodes'}));await screen.findByRole('button',{name:'Next page'});
 const previous=screen.getByTestId('inspection-output').textContent;
 await user.click(screen.getByRole('button',{name:'Next page'}));
 await waitFor(()=>expect(screen.getByTestId('inspection-output').textContent).not.toBe(previous));
 expect(screen.getByTestId('inspection-output').textContent).toContain('p51');
 await user.click(screen.getByRole('button',{name:'Reset disposable database'}));
 await waitFor(()=>expect(screen.getByTestId('query-output').textContent).toContain('"reset":true'));
});
