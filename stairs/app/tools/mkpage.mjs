// dist/index.html → ../page.h as gzip bytes. The board sends it with Content-Encoding: gzip.
import { readFileSync, writeFileSync } from 'node:fs'
import { gzipSync } from 'node:zlib'

const html = readFileSync(new URL('../dist/index.html', import.meta.url))
const gz = gzipSync(html, { level: 9 })
const rows = []
for (let i = 0; i < gz.length; i += 24) rows.push([...gz.subarray(i, i + 24)].map(b => '0x' + b.toString(16).padStart(2, '0')).join(','))

writeFileSync(new URL('../../page.h', import.meta.url), `// GENERATED from stairs/app (npm run build). Edit stairs/app/src, not this file.
// The stair lights' phone page: React, gzipped (${html.length} bytes → ${gz.length}).
#pragma once
static const size_t PAGE_LEN = ${gz.length};
static const uint8_t PAGE[] PROGMEM = {
${rows.join(',\n')}
};
`)
console.log(`page.h: ${html.length} bytes → ${gz.length} gzipped`)
