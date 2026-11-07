// The server's cryptography – our own: the apps' C++ code (native/core/crypto_core.cpp), in the very
// WebAssembly the website runs (public/ss-web.wasm, through public/ss-web.js): SHA-256, SHA-1, HMAC and
// scrypt, the same bytes as any standard implementation gives (test/ss-web.test.js checks them against
// OpenSSL). Random numbers come from the system, as in the apps (node:crypto's randomBytes).
import fs from 'node:fs';
import vm from 'node:vm';

const read = (name) => fs.readFileSync(new URL(`../public/${name}`, import.meta.url));
const context = { TextEncoder, Uint8Array, WebAssembly };
context.globalThis = context;
vm.runInNewContext(read('ss-web.js').toString('utf8'), context);
const own = context.ScreenShareWasm.wrap(new WebAssembly.Instance(new WebAssembly.Module(read('ss-web.wasm')), {}).exports).crypto;

/** (data, key: a Buffer, a Uint8Array or a string – its UTF-8) */
export const sha256 = (data) => Buffer.from(own.sha256(data));
export const sha1 = (data) => Buffer.from(own.sha1(data));
export const hmacSha256 = (key, data) => Buffer.from(own.hmacSha256(key, data));
export const hmacSha1 = (key, data) => Buffer.from(own.hmacSha1(key, data));
/** scrypt (RFC 7914) with N = 16384, r = 8, p = 1 – what node:crypto's scryptSync makes with its defaults. */
export const scrypt = (password, salt, length = 32) => Buffer.from(own.scrypt(password, salt, { N: 16384, r: 8, p: 1 }, length));

/** Two byte strings compared in a time that does not tell where they differ (false when the lengths do). */
export function equal(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}
