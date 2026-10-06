import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
export default defineConfig({ plugins: [react()], test: { environment: 'jsdom', environmentOptions: { jsdom: { url: process.env.NOVA_RUNNER_TEST_ORIGIN || 'http://127.0.0.1:3000/' } }, include: ['tests/*.test.tsx'], setupFiles: ['tests/web-setup.ts'] } });
