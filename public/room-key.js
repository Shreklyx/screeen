// The room's own key – end to end for real (as the apps' core, native/core/engine.cpp): 16 random bytes the
// sharer makes, in the link after the code ("#abc123.<26 letters>"). The part of a link after # never goes
// to a server, and the page never sends it: each side proves to the other that it holds it (an HMAC over
// its own public key and the session), and it is mixed into the keys (direct-web.js) – the server that
// carries the key agreement cannot put its own key in between. A viewer that has it accepts only a sharer
// that proves it. (A script of its own: the tests check it against the apps' values.)
(function (root) {
  'use strict';
  const B32 = 'abcdefghijklmnopqrstuvwxyz234567';

  function base32(bytes) {
    let out = '';
    let acc = 0;
    let bits = 0;
    for (const b of bytes) {
      acc = (acc << 8) | b;
      bits += 8;
      while (bits >= 5) {
        bits -= 5;
        out += B32[(acc >> bits) & 31];
      }
      acc &= (1 << bits) - 1;
    }
    if (bits) out += B32[(acc << (5 - bits)) & 31];
    return out;
  }

  function unbase32(s) {
    const out = [];
    let acc = 0;
    let bits = 0;
    for (const c of String(s)) {
      const v = B32.indexOf(c);
      if (v < 0) return null;
      acc = (acc << 5) | v;
      bits += 5;
      if (bits >= 8) {
        bits -= 8;
        out.push((acc >> bits) & 255);
        acc &= (1 << bits) - 1;
      }
    }
    return new Uint8Array(out);
  }

  /** The room's key from a link or "code.key" (none: null). */
  function parseRoomKey(input) {
    let s = String(input || '').trim();
    const hash = s.indexOf('#');
    if (hash >= 0) s = s.slice(hash + 1);
    const dot = s.indexOf('.');
    if (dot < 0) return null;
    const key = unbase32(s.slice(dot + 1).toLowerCase().replace(/[^a-z2-7]/g, ''));
    return key?.length === 16 ? key : null;
  }

  async function e2eProof(key, role, session, pub) {
    const subtle = root.crypto.subtle;
    const k = await subtle.importKey('raw', key, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const mac = new Uint8Array(await subtle.sign('HMAC', k, new TextEncoder().encode(`ss-e2e-v2|${role}|${session}|${pub}`)));
    return btoa(String.fromCharCode(...mac.subarray(0, 16)));
  }

  async function e2eProofOk(key, role, session, pub, proof) {
    if (key?.length !== 16 || typeof proof !== 'string') return false;
    const want = await e2eProof(key, role, session, pub);
    let diff = want.length ^ proof.length;
    for (let i = 0; i < want.length; i++) diff |= want.charCodeAt(i) ^ proof.charCodeAt(i);
    return diff === 0;
  }

  root.RoomKey = { base32, unbase32, parseRoomKey, e2eProof, e2eProofOk };
})(globalThis);
