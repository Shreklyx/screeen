# Screen Share – 4K képernyőmegosztás hanggal

Saját szerveren futó képernyőmegosztó oldal: **4K / 60 FPS**, **rendszerhang** (sztereó), minimális késleltetés.
Megosztást csak a **megosztási jelszóval** lehet indítani, nézni a linkkel bárki tud – regisztráció és telepítés nélkül, telefonon is.

- **WebRTC, közvetlen kapcsolat** – a kép és a hang közvetlenül a gépek között megy, titkosítva (DTLS-SRTP).
  A szerver csak a kapcsolat felépítését intézi, illetve **TURN relével** segít, ha a két gép nem éri el egymást
  közvetlenül (pl. mobilnet, szigorú NAT).
- **Alapból a legjobb beállítás, a gép alapján (Automatikus):** ha a videókártya kódol, **4K · 60 FPS · max. 80 Mbit/s**
  (4K-ra korlátozva, így egy 5K-s Retina kijelző sem terheli feleslegesen a gépet). Ha a processzor kódol, a szálak
  száma szerint annyit, amennyit késés nélkül bír (pl. 12+ szál: 1440p60, 8 szál: 1080p60, kevesebb: 1080p30), és
  több néző esetén egy fokozattal lejjebb. Nézőnként külön dönt: aki a processzorra szorul (pl. a GeForce kb. 8-as
  korlátja fölött), csak az kap könnyebb beállítást. A tényleges sebességhez a WebRTC folyamatosan igazodik.
  Kézzel minden felülírható (felbontás, FPS, 6–80 Mbit/s). A WebRTC alapértelmezett 2,5 Mbit/s-os plafonja fel van
  oldva, és a kapcsolat rögtön magas bitrátáról indul.
- **Hardveres kódolás a videókártyán**: NVIDIA NVENC, Apple VideoToolbox (Mac), Intel Quick Sync, AMD VCN. Automatikusan
  azt a kodeket választja (AV1 / H.265 / VP9 / H.264 / VP8), amelyiket a videókártya kódol – megosztás közben
  **ellenőrzi**, hogy tényleg a videókártya kódol-e, ha nem, kodeket vált, és a gépre megjegyzi az eredményt.
  A megosztó panelen látszik: *Videókártya: NVIDIA GeForce …* · *Hardveres kódolás: NVIDIA NVENC · H.264 · 3 ms/kép*.
- **Rendszerhang**: Opus, sztereó, 510 kbit/s, 10 ms-os csomagok, zajszűrés / visszhangszűrés / hangerő-szabályzás
  nélkül (filmhez, zenéhez).
- **Alacsony késleltetés**: hardveres kódolás, minimális jitter puffer a nézőnél (rossz hálózaton „Simább” / „Stabil”
  módra állítható), mozgásra hangolt kódolás filmhez/játékhoz. A késést méri is: a néző statisztikájában és a megosztó
  nézőlistájában nézőnként látszik (kódolás + hálózat + fogadástól a megjelenésig).
- Élő beállítások megosztás közben (felbontás, FPS, bitráta, tartalomtípus, kodek), forrásváltás, szüneteltetés,
  hangszint-mérő, nézőnkénti statisztika (FPS, bitráta, kodek, HW/SW, RTT, mi korlátozza a minőséget).
- Ha a megosztó újratölti az oldalt vagy megszakad a nete, a szoba (és a link) 2 percig megmarad, a nézők maguktól
  visszakapják a képet.

## Telepítés a szerverre

A szerveren (SSH-val belépve), Debian 12/13 vagy Ubuntu 22.04/24.04:

```bash
git clone https://github.com/Shreklyx/screeen.git
cd screeen
sudo bash deploy/install.sh
```

A telepítő:

1. felteszi a Dockert (ha még nincs),
2. megkérdezi a domaint (`mozizas.hu`) és a **megosztási jelszót** (üresen hagyva generál egyet és kiírja),
3. ha egy másik Docker alkalmazás – pl. a **Sori** – foglalja a 80/443-as portot, **leállítja**: a konténerei
   megszűnnek, de az adatai (filmek, adatbázis) a lemezen maradnak, semmit nem töröl,
4. megnyitja a portokat a tűzfalon (ha `ufw` fut),
5. elindítja az oldalt, a **Caddy**-t (automatikus HTTPS, Let's Encrypt) és a **coturn** TURN relét.

Pár perc múlva él a **https://mozizas.hu**.

**DNS:** a domain `A` rekordja a szerver IP-jére mutasson, Cloudflare-nél **„DNS only”** (szürke felhő) módban.
`AAAA` (IPv6) rekordot ne vegyél fel: a Docker hálózat IPv4-es, IPv6-on minden látogató ugyanazzal a belső címmel
érkezne, és közös próbálkozási korláton osztoznának.

**Tűzfal:** ha a szolgáltató webes felületén is van tűzfal, ott ezeket engedd:

| Port | Protokoll | Mire |
| --- | --- | --- |
| 80, 443 | TCP | weboldal (HTTPS) |
| 443 | UDP | HTTP/3 |
| 3478 | TCP + UDP | TURN relé |
| 49152–50151 | UDP | TURN relé adatforgalma |

### Frissítés, napló, jelszócsere

```bash
sudo bash deploy/install.sh          # frissítés: új kód letöltése + újraindítás
sudo docker compose logs -f app      # napló (caddy, turn is)
```

- **Jelszócsere:** futtasd újra a telepítőt, és a „Megtartod a jelenlegi megosztási jelszót?” kérdésre válaszolj `n`-nel.
  A jelszó a `.env` fájlban is megtalálható: `sudo grep HOST_PASSWORD .env`.
- **A Sori újraindítása** (ha valaha kell): `sudo docker compose down` ebben a mappában, majd a Sori mappájában
  `sudo docker compose up -d`.

## Használat

1. Nyisd meg az oldalt **Chrome**-ban vagy **Edge**-ben, írd be a megosztási jelszót, és kattints a
   **Képernyő megosztása** gombra.
2. Válaszd ki, mit osztasz meg, majd küldd el a kapott linket (vagy a 6 karakteres kódot) a nézőknek.
3. A nézők megnyitják a linket bármilyen modern böngészőben (telefonon is).

### Hang Windows 10 / 11 alatt

- **Teljes képernyő** megosztásakor pipáld be a **Rendszerhang megosztása** négyzetet a Chrome/Edge ablakában.
- Ha csak egy böngészőfül hangja kell (pl. egy film), válaszd a **Chrome-lap** fület, és kapcsold be a lap hangjának megosztását.
- Ha egy ablak megosztásakor nincs hang, oszd meg inkább a teljes képernyőt. A megosztó oldalon a **Hang** doboz
  csíkja mutatja, hogy megy-e hang.

### Mac

- Első megosztáskor engedélyezd a böngészőnek a képernyőfelvételt: *Rendszerbeállítások → Adatvédelem és biztonság →
  Képernyőfelvétel* (újabb macOS-en *Képernyő- és rendszerhangfelvétel*), majd indítsd újra a böngészőt.
- Hang: Chrome-ban egy böngészőlap hangja mindig megosztható (*Chrome-lap* fül → lap hangja). Teljes képernyőnél akkor
  van hang, ha a Chrome felajánlja a *Rendszerhang megosztása* opciót. A Safari nem tud hangot megosztani.
- A Mac a H.264-et hardveresen kódolja (VideoToolbox) – a Kodek maradjon „Automatikus”.

### Tippek, hogy ne akadjon

- **Vezetékes net** a megosztó gépen, és elég feltöltési sávszélesség: 4K60-hoz kb. 20–30 Mbit/s nézőnként.
- Chrome / Edge: legyen bekapcsolva a *Beállítások → Rendszer → Grafikus gyorsítás használata, ha rendelkezésre áll*,
  és legyen friss a videókártya illesztőprogramja – a **Kódolás** dobozban látszik, hogy a videókártya kódol-e.
  Szoftveres kódolásnál a késés a többszöröse is lehet.
- NVIDIA: a GeForce kártyák egyszerre kb. 8 hardveres kódolást bírnak (nézőnként egyet); afölött a processzor kódol.
- Filmhez / játékhoz a **Tartalom: Film / játék** mód kell, szöveghez / kódhoz a **Szöveg / prezentáció**.
- Minden néző külön kódolt adatfolyamot kap, ezért sok nézőnél (4–5 felett) érdemes 1080p-re vagy kisebb bitrátára váltani.
- A néző statisztikájában (**S** billentyű) a **Kapcsolat: TURN relé** azt jelenti, hogy a kép a szerveren keresztül
  megy – ilyenkor a szerver sávszélessége is számít.
- Hibakereséshez: a link elé írt `?relay=1` (pl. `https://mozizas.hu/?relay=1#kod`) mindenképp a TURN relén át küldi a képet.

## Fejlesztés

```bash
npm install
HOST_PASSWORD=titok123 npm start     # http://localhost:8080
npm test                              # szerver tesztek
```

A képernyőmegosztás csak HTTPS-en vagy `localhost`-on működik. TURN relé nélkül (üres `TURN_SECRET`) csak STUN-nal fut.

## Hogyan működik

- `server/server.js` – Node.js szerver (egyetlen függőség: `ws`): kiszolgálja az oldalt biztonsági fejlécekkel (CSP stb.),
  a `/ws` WebSocketen szobákat kezel és továbbítja a kapcsolatfelépítő üzeneteket (SDP, ICE) a megosztó és a nézők között,
  és 24 órás TURN jelszavakat ad (TURN REST API). Jelszó- és csatlakozási próbálkozások IP-nként korlátozva.
- `public/` – a böngészős oldal. A megosztó nézőnként egy `RTCPeerConnection`-t tart, csak küldő (sendonly) sávokkal;
  a minőséget `RTCRtpSender.setParameters()` (bitráta, FPS, felbontás-skálázás, degradáció, prioritás),
  `setCodecPreferences()` és az SDP hangolása (Opus sztereó/510k, kezdő bitráta) állítja. Forrásváltáskor
  `replaceTrack()`, kodekváltáskor újratárgyalás ugyanazon a kapcsolaton, megszakadáskor ICE restart.
- `docker-compose.yml` – `app` (Node), `caddy` (HTTPS, beállítás: `deploy/caddy/Caddyfile`), `turn` (coturn, host
  hálózaton; a belső/foglalt címek felé tiltott relézéssel).
