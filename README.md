# mozizás – 4K képernyőmegosztás hanggal

Böngészős képernyőmegosztás **4K / 60 FPS** felbontásban, **rendszerhanggal** (sztereó), a lehető legkisebb késleltetéssel. Nincs regisztráció, nincs telepítés: a megosztó kap egy linket, a nézők megnyitják, és kész.

- **WebRTC, közvetlen P2P** – a kép és a hang közvetlenül a gépek között megy, végponttól titkosítva (DTLS-SRTP). Szerveren csak a kapcsolat felépítése (signaling) megy át.
- **Natív felbontás, akár 4K 60 FPS**, állítható max. bitráta (6–80 Mbit/s). A libwebrtc alapértelmezett 2,5 Mbit/s-os plafonja fel van oldva, és a kapcsolat rögtön magas bitrátáról indul.
- **Hardveres kódolás**: automatikusan azt a kodeket választja (AV1 / VP9 / H.264 / VP8), amelyikhez a megosztó *és* a néző gépén is van hardveres gyorsítás.
- **Rendszerhang**: Opus, sztereó, 510 kbit/s, zajszűrés / visszhangszűrés / hangerő-szabályzás kikapcsolva (filmhez, zenéhez).
- **Alacsony késleltetés**: minimális jitter puffer a nézőnél (állítható „Simább” / „Stabil” módra rossz hálózaton), mozgásra hangolt kódolás filmhez/játékhoz.
- Élő beállítások megosztás közben (felbontás, FPS, bitráta, tartalomtípus, kodek), forrásváltás, szüneteltetés, hangszint-mérő, nézőnkénti statisztika (FPS, bitráta, kodek, HW/SW, RTT, mi korlátozza a minőséget).
- Néző: teljes képernyő, hangerő, statisztika (felbontás, FPS, becsült késés, csomagvesztés…), automatikus újracsatlakozás, telefonon is megy.

## Használat

1. Nyisd meg az oldalt **Chrome**-ban vagy **Edge**-ben (Windows 10 / 11, macOS, Linux), és kattints a **Képernyő megosztása** gombra.
2. Válaszd ki, mit osztasz meg, majd küldd el a kapott linket (vagy a 6 betűs kódot) a nézőknek.
3. A nézők megnyitják a linket bármilyen modern böngészőben (telefonon is).

### Hang Windows 10 / 11 alatt

- **Teljes képernyő** megosztásakor pipáld be a **Rendszerhang megosztása** négyzetet a Chrome/Edge ablakában.
- Ha csak egy böngészőfül hangja kell (pl. egy film), válaszd a **Chrome-lap** fület, és kapcsold be a lap hangjának megosztását.
- Ha egy ablak megosztásakor nincs hang, oszd meg inkább a teljes képernyőt. A megosztó oldalon a **Hang** doboz csíkja mutatja, hogy megy-e hang.

### Tippek, hogy ne akadjon

- **Vezetékes net** a megosztó gépen (Wi-Fi helyett), és elég feltöltési sávszélesség: 4K60-hoz kb. 20–30 Mbit/s nézőnként.
- Chrome / Edge: legyen bekapcsolva a *Beállítások → Rendszer → Grafikus gyorsítás használata, ha rendelkezésre áll* opció (hardveres kódolás).
- Filmhez / játékhoz a **Tartalom: Film / játék** mód kell, szöveghez / kódhoz a **Szöveg / prezentáció**.
- Minden néző külön kódolt adatfolyamot kap (P2P), ezért sok nézőnél (4–5 felett) érdemes 1080p-re vagy kisebb bitrátára váltani.
- A néző statisztikájában a **Kapcsolat: TURN relé** azt jelenti, hogy nem jött létre közvetlen kapcsolat (pl. szigorú céges/mobilos NAT). Ilyenkor a minőség az ingyenes relé szerver miatt gyengébb lehet; saját TURN szervert az `app.js` elején, a `CONFIG.iceServers` alatt lehet megadni.

## Telepítés (Cloudflare, `share.mozizas.hu`)

Az oldal egy statikus Cloudflare Worker (`public/` mappa, `wrangler.jsonc`). A `mozizas.hu` főoldalt (Sori) nem érinti, az egyedi domain a `share.mozizas.hu`.

```bash
npm install
CLOUDFLARE_API_TOKEN=<token> CLOUDFLARE_ACCOUNT_ID=<account id> npx wrangler deploy
```

A tokenhez a Cloudflare **„Edit Cloudflare Workers”** sablonja kell (Account → Workers Scripts: Edit; Zone `mozizas.hu` → Workers Routes: Edit, DNS: Edit). A `share.mozizas.hu` DNS rekordot és a HTTPS tanúsítványt a deploy automatikusan létrehozza. Másik domainhez a `wrangler.jsonc` `routes` részét kell átírni.

Helyi fejlesztés: `npx wrangler dev`, majd <http://localhost:8787>. (A képernyőmegosztás csak HTTPS-en vagy `localhost`-on működik.)

Bármilyen más statikus tárhely is jó (GitHub Pages, Netlify, saját szerver): csak a `public/` mappa tartalmát kell kiszolgálni HTTPS-en.

## Hogyan működik

- Kapcsolatfelépítés: [PeerJS](https://peerjs.com/) a nyilvános PeerJS szerverrel (`0.peerjs.com`). Saját PeerJS szerver is megadható: `?server=https://peer.pelda.hu:443/`.
- A néző adatkapcsolatot nyit a megosztóhoz, a megosztó pedig felhívja a képernyő + hang sávokkal. Forrásváltáskor a sávokat `replaceTrack()` cseréli, újracsatlakozás nélkül.
- Minőség: `RTCRtpSender.setParameters()` (max. bitráta, FPS, felbontás-skálázás, degradáció-preferencia, prioritás), SDP-hangolás (Opus sztereó/510k, kezdő bitráta, kodek-sorrend), `contentHint`, `jitterBufferTarget`.

Fájlok: `public/index.html`, `public/style.css`, `public/app.js`, `public/_headers` (biztonsági fejlécek), `wrangler.jsonc`.
