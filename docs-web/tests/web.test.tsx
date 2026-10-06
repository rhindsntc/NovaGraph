import React from 'react';
import { test, expect, vi } from 'vitest';
import { render, screen, waitFor, within, act } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { App } from '../src/App';
import { DocContent } from '../src/components/DocContent';
import type { DocArticle } from '../src/types/docs';

const article: DocArticle = { id: 'test', title: 'Test', category: 'Examples', lead: '', content: '', html: '<h1>Test</h1>', codeTabs: [
 { label: 'First', language: 'swift', code: 'first()' }, { label: 'Second', language: 'ngql', code: 'second()' }
] };
function start(hash = '#overview') { window.history.replaceState(null, '', hash); return render(<App />); }
test('deep links and browser back restore the article and unknown routes are explicit', async () => {
 const user = userEvent.setup(); start('#ngql-syntax');
 expect(screen.getByRole('heading', {name: /NGQL syntax and bounded execution/})).toBeTruthy();
 await user.click(screen.getAllByRole('link', {name: /Quickstart/i})[0]);
 await waitFor(() => expect(window.location.hash).toBe('#quickstart'));
 act(() => window.history.back());
 await waitFor(() => expect(window.location.hash).toBe('#ngql-syntax'));
 await waitFor(() => expect(screen.getByRole('heading', {name: /NGQL syntax and bounded execution/})).toBeTruthy());
 act(() => { window.location.hash = 'missing-article'; });
 await waitFor(() => expect(screen.getByRole('heading', {name: 'Article not found'})).toBeTruthy());
});
test('empty and malformed hashes recover safely', async () => {
 start('#%invalid/anchor');
 expect(await screen.findByRole('heading', {name: 'Article not found'})).toBeTruthy();
 act(() => { window.location.hash = ''; });
 await waitFor(() => expect(screen.queryByRole('heading', {name: 'Article not found'})).toBeNull());
});
test('changing article with fewer tabs resets selection and copy uses the visible code', async () => {
 const user = userEvent.setup(); const view = render(<DocContent article={article} onNavigate={() => {}} />);
 await user.click(screen.getByRole('tab', {name: 'Second'}));
 view.rerender(<DocContent article={{...article, id: 'one', codeTabs: article.codeTabs!.slice(0,1)}} onNavigate={() => {}} />);
 expect(screen.getByRole('tabpanel').textContent).toContain('first()');
 const copy = vi.spyOn(navigator.clipboard, 'writeText');
 await user.click(screen.getByRole('button', {name: 'Copy code'}));
 expect(copy).toHaveBeenCalledWith('first()'); expect(await screen.findByText('Copied!')).toBeTruthy();
});
test('clipboard denial reports failure instead of copied success', async () => {
 const user = userEvent.setup(); vi.spyOn(navigator.clipboard, 'writeText').mockRejectedValue(new Error('denied'));
 render(<DocContent article={article} onNavigate={() => {}} />);
 await user.click(screen.getByRole('button', {name: 'Copy code'}));
 expect(await screen.findByText('Copy failed')).toBeTruthy(); expect(screen.queryByText('Copied!')).toBeNull();
});
test('no-result arrow keys cannot poison the next keyboard selection; search restores focus', async () => {
 const user = userEvent.setup(); start();
 const trigger = screen.getByRole('button', {name: /Search docs/}); await user.click(trigger);
 const dialog = screen.getByRole('dialog', {name: 'Search documentation'});
 const input = within(dialog).getByRole('combobox');
 await user.type(input, 'zzzz-no-result'); await user.keyboard('{ArrowDown}{ArrowUp}{Enter}');
 expect(screen.getByRole('dialog')).toBeTruthy();
 await user.clear(input); await user.type(input, 'Quickstart'); await user.keyboard('{ArrowDown}{Enter}');
 await waitFor(() => expect(window.location.hash).toBe('#quickstart'));
 expect(screen.queryByRole('dialog')).toBeNull(); expect(document.activeElement).toBe(trigger);
});
test('theme persists and mobile menu has named expanded state and closes on navigation', async () => {
 const user = userEvent.setup(); localStorage.setItem('novagraph-docs-theme', 'light'); start();
 expect(document.documentElement.dataset.theme).toBe('light');
 await user.click(screen.getByRole('button', {name: 'Switch to Dark Mode'}));
 expect(localStorage.getItem('novagraph-docs-theme')).toBe('dark');
 const menu = screen.getByRole('button', {name: 'Open navigation'}); await user.click(menu);
 expect(menu.getAttribute('aria-expanded')).toBe('true');
 await user.click(screen.getAllByRole('link', {name: /Quickstart/i})[0]);
 expect(screen.getByRole('button', {name: 'Open navigation'}).getAttribute('aria-expanded')).toBe('false');
});

test('section search navigates to a real heading; dialog traps Tab and Escape restores focus', async () => {
 const user = userEvent.setup(); start(); const trigger=screen.getByRole('button', {name:/Search docs/}); await user.click(trigger);
 const input=screen.getByRole('combobox'); await user.type(input, 'Copy');
 await user.keyboard('{Shift>}{Tab}{/Shift}'); expect(document.activeElement).toBe(screen.getByRole('button',{name:'Close search'}));
 await user.keyboard('{Tab}'); expect(document.activeElement).toBe(input);
 await user.keyboard('{Escape}'); expect(document.activeElement).toBe(trigger);
});
test('static playground is read-only and never contacts a local server', () => {
 const request=vi.spyOn(globalThis,'fetch'); start('#playground');
 expect(screen.getByRole('textbox').hasAttribute('readonly')).toBe(true);
 expect(screen.queryByRole('button',{name:/Simulate|Run query|Connect/})).toBeNull();
 expect(screen.getByText(/Recorded real-engine output for Social Network/)).toBeTruthy();
 expect(request).not.toHaveBeenCalled();
});

test.each([
 {name:'Social Network', nodes:['user:ada','user:grace','user:margaret'], edges:[['user:ada','FOLLOWS','user:grace'],['user:grace','FOLLOWS','user:margaret']]},
 {name:'Architecture & Tech Stack', nodes:['c:catalog','c:engine','c:graphdbkit','c:wal'], edges:[['c:engine','USES','c:catalog'],['c:engine','USES','c:wal'],['c:graphdbkit','WRAPS','c:engine']]},
 {name:'Authorization & Roles', nodes:['r:admin','res:database','u:alice'], edges:[['r:admin','GRANTS','res:database'],['u:alice','HAS_ROLE','r:admin']]},
])('recorded $name graph renders the actual traversal paths', async ({name,nodes,edges}) => {
 const {parseDisplayJSON,resultGraph}=await import('../src/lib/runner');
 const user=userEvent.setup(); const view=start('#playground');
 await user.click(screen.getByRole('button',{name,exact:true}));
 const graph=resultGraph(parseDisplayJSON(screen.getByTestId('query-output').textContent!));
 expect(graph.nodes.map(node=>node.id).sort()).toEqual(nodes);
 expect(graph.edges.map(edge=>[edge.from,edge.type,edge.to]).sort()).toEqual(edges);
 await user.click(screen.getByRole('button',{name:'Result graph'}));
 expect(screen.getAllByRole('button',{name:/^Select node /}).map(node=>node.getAttribute('aria-label')!.replace('Select node ','')).sort()).toEqual(nodes);
 expect(view.container.querySelectorAll('.graph-canvas line').length).toBe(edges.length);
});

test('code tabs support arrow key selection and keep one keyboard tab stop', async () => {
 const user=userEvent.setup(); render(<DocContent article={article} onNavigate={()=>{}} />);
 const first=screen.getByRole('tab',{name:'First'}); first.focus(); await user.keyboard('{ArrowRight}');
 expect(screen.getByRole('tab',{name:'Second'}).getAttribute('aria-selected')).toBe('true');
 expect(first.tabIndex).toBe(-1); expect(screen.getByRole('tabpanel').textContent).toContain('second()');
});

test.each([false,true])('Enter on Close search closes without navigating (empty results: %s)', async empty => {
 const user=userEvent.setup(); start('#ngql-syntax');
 await user.click(screen.getByRole('button',{name:/Search docs/}));
 if(empty) await user.type(screen.getByRole('combobox'),'zzzz-no-result');
 await user.keyboard('{Tab}'); expect(document.activeElement).toBe(screen.getByRole('button',{name:'Close search'}));
 await user.keyboard('{Enter}');
 expect(screen.queryByRole('dialog')).toBeNull(); expect(window.location.hash).toBe('#ngql-syntax');
});
test('Back from playground restores the article section after it mounts', async () => {
 const user=userEvent.setup(); const scroll=vi.spyOn(Element.prototype,'scrollIntoView');
 start('#documentation/register-an-example');
 await waitFor(()=>expect(scroll.mock.instances.some(el=>(el as Element).id==='documentation/register-an-example')).toBe(true));
 await user.click(screen.getByRole('button',{name:'Playground'}));
 expect(screen.queryByRole('heading',{name:'Register an example'})).toBeNull(); scroll.mockClear();
 act(()=>window.history.back());
 await screen.findByRole('heading',{name:'Register an example'});
 await waitFor(()=>expect(scroll.mock.instances.some(el=>(el as Element).id==='documentation/register-an-example')).toBe(true));
});

test('recorded graphs contain only returned native records and preserve integer digits for display', async () => {
 const {parseDisplayJSON,resultGraph}=await import('../src/lib/runner');
 const value=parseDisplayJSON('{"schemaVersion":2,"ok":true,"data":{"nodes":[{"kind":"node","id":"__proto__","label":"N","properties":{"name":{"type":"string","value":"9007199254740993"},"count":{"type":"int","value":9223372036854775807}}}],"paths":[]}}');
 const graph=resultGraph(value);
 expect(graph.nodes[0].id).toBe('__proto__');expect(graph.nodes[0].properties.count).toBe('9223372036854775807');
 expect(graph.nodes[0].properties.name).toBe('9007199254740993');expect(graph.edges).toEqual([]);
 expect(resultGraph({data:[{id:'projection',name:'Ada'}]}).nodes).toEqual([]);
});
test('graph layout refreshes for new IDs at the same count and nodes support keyboard inspection', async () => {
 const {GraphCanvas}=await import('../src/components/GraphCanvas'); const select=vi.fn(); const user=userEvent.setup();
 const view=render(<GraphCanvas graph={{nodes:[{id:'before',label:'N',properties:{}}],edges:[]}} onSelectNode={select}/>);
 view.rerender(<GraphCanvas graph={{nodes:[{id:'__proto__',label:'N',properties:{}}],edges:[]}} onSelectNode={select}/>);
 const node=screen.getByRole('button',{name:'Select node __proto__'});expect(node.getAttribute('transform')).not.toContain('undefined');
 node.focus();await user.keyboard('{Enter}');expect(select).toHaveBeenCalledWith(expect.objectContaining({id:'__proto__'}));
});
test('edge-only results cannot reuse endpoint positions from an earlier graph', async () => {
 const {GraphCanvas}=await import('../src/components/GraphCanvas');
 const edge={from:'a',to:'b',type:'E'};
 const view=render(<GraphCanvas graph={{nodes:[{id:'a',label:'N',properties:{}},{id:'b',label:'N',properties:{}}],edges:[edge]}}/>);
 expect(view.container.querySelectorAll('line').length).toBe(1);
 view.rerender(<GraphCanvas graph={{nodes:[],edges:[]}}/>);
 view.rerender(<GraphCanvas graph={{nodes:[],edges:[edge]}}/>);
 expect(view.container.querySelectorAll('line').length).toBe(0);
});


test('documentation navigation opens public compatibility guidance', async () => {
 const user = userEvent.setup(); start();
 await user.click(within(screen.getByRole('complementary', {name: 'Documentation navigation'})).getByRole('link', {name: 'Compatibility and migration'}));
 await waitFor(() => expect(window.location.hash).toBe('#compatibility'));
 expect(screen.getByRole('heading', {name: 'Compatibility and migration'})).toBeTruthy();
 expect(screen.getByRole('heading', {name: 'Product limitations'})).toBeTruthy();
 expect(screen.queryByText('Project Status')).toBeNull();
});
