import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { stripTypeScriptTypes } from 'node:module';
const source = await readFile(new URL('../src/worker.ts', import.meta.url), 'utf8');
const output = stripTypeScriptTypes(source, { mode: 'strip' });
const directory = new URL('../dist/', import.meta.url);
await mkdir(directory, { recursive: true });
await writeFile(new URL('worker.js', directory), output);
console.log('Built dist/worker.js (portable Worker, no runtime dependencies).');
