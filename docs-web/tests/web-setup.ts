import { afterEach, vi } from 'vitest';
import { JSDOM } from 'jsdom';
import { cleanup } from '@testing-library/react';
afterEach(() => { cleanup(); vi.restoreAllMocks(); localStorage.clear(); });
window.scrollTo = () => {};
Element.prototype.scrollIntoView = () => {};

Object.defineProperty(globalThis, 'localStorage', {value: new JSDOM('', {url: 'https://docs.test'}).window.localStorage, configurable: true});
