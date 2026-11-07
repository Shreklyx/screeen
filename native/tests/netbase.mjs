// The network on its own (CI): bursts like the picture's frames (25 KB in 1400-byte UDP packets, 60 a
// second) through the same simulated network, and how long each burst's last packet takes – one clock
// at both ends. What a path measures beyond this is ours.
import dgram from 'node:dgram';

const now = () => performance.timeOrigin + performance.now();
const rx = dgram.createSocket('udp4');
const tx = dgram.createSocket('udp4');
const lat = [];
rx.on('message', (m) => { if (m[0] === 1) lat.push(now() - m.readDoubleLE(1)); });
rx.bind(0, '127.0.0.1', () => {
  const port = rx.address().port;
  let n = 0;
  const timer = setInterval(() => {
    const at = now();
    for (let i = 0; i < 18; i++) {
      const b = Buffer.alloc(1400);
      b[0] = i === 17 ? 1 : 0;
      b.writeDoubleLE(at, 1);
      tx.send(b, port, '127.0.0.1');
    }
    if (++n < 300) return;
    clearInterval(timer);
    setTimeout(() => {
      lat.sort((a, b) => a - b);
      const r = (q) => Math.round(lat[Math.min(lat.length - 1, Math.floor(lat.length * q))] * 10) / 10;
      console.log(JSON.stringify({ median: r(0.5), p90: r(0.9), n: lat.length }));
      rx.close();
      tx.close();
    }, 300);
  }, 1000 / 60);
});
