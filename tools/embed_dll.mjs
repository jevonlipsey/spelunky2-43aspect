// node tools/embed_dll.mjs [discord_game_sdk.dll]
// writes the dll into index.html's payload script, so the page can embed the
// exact release build in Spel2.exe. run after every `make release`.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const dll = fs.readFileSync(process.argv[2] || path.join(root, 'discord_game_sdk.dll'));
const page = path.join(root, 'index.html');
const sha = crypto.createHash('sha256').update(dll).digest('hex');
const body = `\nconst MOD_DLL_SHA256 = '${sha}';\nconst MOD_DLL_B64 = '${dll.toString('base64')}';\n`;
const html = fs.readFileSync(page, 'utf8');
const re = /(<script id="payload">)[\s\S]*?(<\/script>)/;
if (!re.test(html)) throw new Error('payload script not found in index.html');
fs.writeFileSync(page, html.replace(re, (_, a, b) => a + body + b));
console.log(`embedded ${dll.length} bytes, sha256 ${sha}`);
