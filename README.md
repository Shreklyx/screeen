# Screen Share – 4K képernyőmegosztás hanggal

Saját szerveren futó képernyőmegosztó oldal: **4K / 60 FPS**, **rendszerhang** (sztereó, az asztali alkalmazással
**5.1 / 7.1 térhang** is), minimális késleltetés.
Megosztást csak a **megosztási jelszóval** lehet indítani, nézni a linkkel bárki tud – regisztráció és telepítés nélkül, telefonon is.

- **Saját átvitel, sehol nincs WebRTC** (*közvetlen mód*, lásd lent) – puffer nélkül, végponttól végpontig titkosítva
  (X25519 + AES-GCM, a szerver sem látja a képet): két asztali alkalmazás között közvetlenül, UDP-n; böngészőben
  (WebCodecs H.264 + Opus) a szerver WebSocketjén át. Ahol a böngészőből hiányzik az X25519 kulcscsere vagy az
  Opus-dekódoló (régebbi Safari / Firefox), az alkalmazások saját C/C++ kódja (BearSSL, libopus) fut az oldalon
  WebAssemblyként (`native/web`).
- **Alapból a legjobb beállítás, a gép alapján (Automatikus):** ha a videókártya kódol, **4K · 60 FPS**, ha a
  processzor, 1080p 30 – és ha a kódoló mégsem bírja a tempót, magától kisebb felbontásra vált. A bitrátát a nézők
  jelentései szabályozzák (csomagvesztés, kihagyott kép, növekvő RTT → kevesebb; ha minden rendben, lassan több).
  Kézzel minden felülírható (felbontás, FPS, bitráta, tartalomtípus).
- **Hardveres kódolás a videókártyán** (H.264): az alkalmazásban NVIDIA NVENC, Intel / AMD (Media Foundation), Apple
  VideoToolbox; böngészőben a WebCodecs a videókártyán kódol, ha tud. A megosztó **egyszer** kódol minden nézőnek.
  A megosztó panelen látszik: *Hardveres kódolás (videókártya) · H.264 · 3840×2160 · 3 ms/kép*.
- **Rendszerhang**: Opus, 2,5 ms-os csomagokban (az Opus legkisebb késése), zajszűrés / visszhangszűrés /
  hangerő-szabályzás nélkül (filmhez, zenéhez).
- **5.1 / 7.1 térhang** (Windows alkalmazás): ha a megosztó gépen 5.1/7.1 hangszóró vagy 7.1-es fejhallgató van
  beállítva, az alkalmazás mind a 8 csatornát átküldi. A térhangos hangszórójú nézők Chrome-ban, Edge-ben vagy az
  alkalmazásban **térhangot** kapnak, a többiek helyesen lekevert sztereót – a középső (párbeszéd) csatornával együtt.
  Böngészőből megosztva csak sztereó megy (a böngészők a rendszerhangot sztereóban adják).
- **Alacsony késleltetés**: a néző **puffer nélkül** dolgozik – minden képkockát azonnal dekódol és egy alacsony
  késleltetésű vásznon kirajzol; ha a gép nem bírja a tempót, mindig csak a legfrissebb kép kerül ki. A megosztó, ha
  a kódoló épp dolgozik, a következő képkockát kihagyja ahelyett, hogy sorba állna (a sor csak késés). A tesztgépen
  (videókártya nélkül, szoftveres kódolással) képtől képig: alkalmazás → alkalmazás **~21–23 ms**, böngésző →
  böngésző **~23–30 ms** (medián). 0 ms nem lehetséges: a képet rögzíteni, kódolni, átküldeni, dekódolni és a
  monitornak kirajzolni is idő (60 Hz-en egy képkocka 16,7 ms). A késést méri is: a néző statisztikájában és a
  megosztó nézőlistájában nézőnként látszik.
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

1. Nyisd meg az oldalt **Chrome**-ban, **Edge**-ben, **Firefoxban** vagy **Safariban** (számítógépen), írd be a megosztási jelszót, és kattints a
   **Képernyő megosztása** gombra.
2. Válaszd ki, mit osztasz meg, majd küldd el a kapott linket (vagy a 6 karakteres kódot) a nézőknek.
3. A nézők megnyitják a linket bármilyen friss böngészőben (telefonon is). Ami nagyon régi (WebCodecs nélküli)
   böngésző, az üzenetet kap, hogy frissítsen vagy használja az alkalmazást.

### Hang Windows 10 / 11 alatt

- **Teljes képernyő** megosztásakor pipáld be a **Rendszerhang megosztása** négyzetet a Chrome/Edge ablakában.
- Ha csak egy böngészőfül hangja kell (pl. egy film), válaszd a **Chrome-lap** fület, és kapcsold be a lap hangjának megosztását.
- Ha egy ablak megosztásakor nincs hang, oszd meg inkább a teljes képernyőt. A megosztó oldalon a **Hang** doboz
  csíkja mutatja, hogy megy-e hang.

### Mac

- Első megosztáskor engedélyezd a böngészőnek a képernyőfelvételt: *Rendszerbeállítások → Adatvédelem és biztonság →
  Képernyőfelvétel* (újabb macOS-en *Képernyő- és rendszerhangfelvétel*), majd indítsd újra a böngészőt.
- Hang: a Mac **teljes rendszerhangjához az asztali alkalmazás** kell (lent) – semmit nem kell mellé telepíteni.
  Böngészőből egy Chrome-lap hangja megosztható (*Chrome-lap* fül → lap hangja).
- **Safari:** a képernyő megosztása megy, a gép hangja nem (a Safari nem adja). Hanghoz a **Hangforrás** listából
  választható egy hangbemenet, vagy oszd meg Chrome-ból / a Mac alkalmazásból.
- A Mac a H.264-et hardveresen kódolja (VideoToolbox) – a Kodek maradjon „Automatikus”.

### iPhone / iPad

- **Nézni** Safariban lehet. Alkalmazásként: Safari → **Megosztás** gomb (négyzet felfelé nyíllal) →
  **Főképernyőhöz adás**. Utána ikonról indul, teljes képernyőn (App Store nem kell). A kapott link Safariban nyílik
  meg; a főképernyős alkalmazásban a kódot a **Nézek** mezőbe írd. Fekvő telefonon a kép kitölti a kijelzőt.
- **Megosztani** a böngészőből nem lehet (az Apple egyik iOS-böngészőben sem engedi), ehhez a **Screen Share iOS
  alkalmazás** kell – lásd lent.

### Screen Share iOS alkalmazás (iPhone / iPad képernyő megosztása)

Az `ios/` mappában van (SwiftUI + ReplayKit közvetítés-kiegészítő + WebRTC). **Még WebRTC-vel küld, amit az oldal
már nem fogad** – amíg nem kapja meg a saját átvitelt, az iPhone-ról megosztott képet nem lehet nézni (a *Nézés*
füle működik). Két füle van:
**Megosztás** (szerver, jelszó, „Képernyő megosztása”, a szobakód és a link küldése) és **Nézés** (az oldal).
Megosztáskor a telefon képernyője (H.264 hardveres kódolás, max. 1080p 60 FPS) és **az alkalmazások hangja**
(pl. YouTube, játék – a mikrofon nem) megy, akkor is, ha közben más alkalmazásra váltasz.

**Telepítés** – az IPA-t a saját Apple ID-ddal kell aláírni, App Store nélkül:

1. Töltsd le: <https://github.com/Shreklyx/screeen/releases/download/desktop-latest/ScreenShare-iOS.ipa>
2. Telepítsd a **Sideloadly**-t (<https://sideloadly.io>, Windows / Mac; Windowson az Apple weboldaláról letöltött
   iTunes és iCloud is kell hozzá).
3. Csatlakoztasd a telefont USB-vel, a Sideloadly-ben add meg az Apple ID-d, húzd be az IPA-t, **Start**.
4. iPhone: *Beállítások → Általános → VPN és eszközkezelés* → a saját Apple ID-d → **Megbízom**; iOS 16-tól még
   *Beállítások → Adatvédelem és biztonság → Fejlesztői mód* → be (újraindítás).
5. Ingyenes Apple ID-vel az alkalmazás 7 napig fut, utána újra kell telepíteni (a Sideloadly ezt tudja magától is);
   fizetős Apple fejlesztői fiókkal (99 USD/év) 1 évig.

**Megosztás:** nyisd meg az alkalmazást → add meg a jelszót → **Képernyő megosztása** → **Közvetítés indítása**.
Leállítás: a piros jelzés a képernyő tetején, vagy a Vezérlőközpont. Egyes alkalmazások (pl. Netflix) a
képernyőfelvételnél fekete képet adnak – ez az Apple / az alkalmazás korlátja.

**TestFlight (számítógép nélküli telepítés, 1 évig érvényes):** fizetős Apple fejlesztői fiókkal (99 USD/év)
a GitHub minden változásnál magától feltölti az appot a TestFlightba, az iPhone-on pedig a TestFlight alkalmazásból
egy koppintással telepíthető és frissül. Beállítás egyszer:

1. <https://developer.apple.com/programs/> – csatlakozz az Apple Developer Programhoz.
2. A *Team ID*: developer.apple.com → Account → **Membership details**.
3. App Store Connect → **Users and Access → Integrations → App Store Connect API** → új kulcs **Admin**
   szereppel; töltsd le a `.p8` fájlt, és jegyezd fel a *Key ID*-t és az *Issuer ID*-t.
4. GitHub → a repó → **Settings → Secrets and variables → Actions → New repository secret**:
   `APPLE_TEAM_ID`, `ASC_KEY_ID`, `ASC_ISSUER_ID`, `ASC_KEY_P8` (a `.p8` fájl teljes tartalma).
5. Futtasd az *iOS app* workflow-t (Actions → iOS app → Run workflow): ez regisztrálja az azonosítókat. Utána
   App Store Connect → **Apps → + → New App**: iOS, a `hu.mozizas.screenshare` csomagazonosítóval (a név legyen
   egyedi, pl. „Screen Share HU”), majd futtasd újra a workflow-t.
6. App Store Connect → az app → **TestFlight** → add hozzá magad (és akit szeretnél) belső tesztelőnek; az iPhone-on
   a **TestFlight** alkalmazásból telepíthető.

Fordítás: a GitHub Actions (`.github/workflows/ios.yml`) macOS gépen építi minden változásnál; kézzel Macen:
`brew install xcodegen`, `cd ios && xcodegen generate`, majd Xcode-ban megnyitható.

### Tippek, hogy ne akadjon

- **Vezetékes net** a megosztó gépen, és elég feltöltési sávszélesség: 4K60-hoz kb. 20–30 Mbit/s nézőnként.
- Chrome / Edge: legyen bekapcsolva a *Beállítások → Rendszer → Grafikus gyorsítás használata, ha rendelkezésre áll*,
  és legyen friss a videókártya illesztőprogramja – a **Kódolás** dobozban látszik, hogy a videókártya kódol-e.
  Szoftveres kódolásnál a késés a többszöröse is lehet.
- Filmhez / játékhoz a **Tartalom: Film / játék** mód kell, szöveghez / kódhoz a **Szöveg / prezentáció**.
- A megosztó egyszer kódol minden nézőnek. A böngészős nézők képe a szerveren át megy (böngészőből nem lehet UDP-t
  küldeni), ezért sok böngészős nézőnél a szerver feltöltési sávszélessége a korlát – az alkalmazások közvetlenül is
  tudnak egymásnak küldeni.
- A néző statisztikájában (**S** billentyű) a **Kapcsolat** sor mutatja, merre megy a kép (helyi hálózat /
  interneten át / a szerveren át).

## Asztali alkalmazás (Windows, Mac)

Letöltés: a weboldal főoldalán, vagy <https://github.com/Shreklyx/screeen/releases/tag/desktop-latest>
(**ScreenShare-Setup.exe** Windowsra – 64 bites, **ScreenShare-mac.dmg** Macre – Apple Silicon és Intel, macOS 12.3+).

**Natív alkalmazás, Electron és Chromium nélkül** (`native/`): Windowson egyetlen ~1,5 MB-os program (Win32,
Direct3D 11, Media Foundation, WASAPI), Macen egy ~2 MB-os alkalmazás (AppKit, ScreenCaptureKit, VideoToolbox, Core
Audio). A két alkalmazás közös C++ magja (`native/core`) végzi a kapcsolódást a szerverhez és a közvetlen mód
átvitelét (lásd lent). A szervert (mindig `https://mozizas.hu`, az alkalmazásban nem írható át) csak a
kapcsolódáshoz, a relékhez és a nézőknek küldött linkhez használja. Macen a rendszer saját vezérlőivel (macOS 26-on
Liquid Glass) és a szokásos Névjegy panellel; Windowson ugyanez az elrendezés (Direct2D-vel rajzolva, a Windows saját,
sötét menüivel): kompakt ablak, Megosztás / Nézés kapcsoló, csoportosított beállítások, kis Névjegy ablak.

- **Megosztás:** jelszó (Windowson DPAPI-val, Macen a Kulcskarikában megjegyezve), képernyő, minőség (4K 60 FPS-ig,
  1080p 120 FPS), a gép hangja. A szobakód és a link után a nézőlista: ki milyen úton (helyi hálózat / internet /
  szerveren át), hány FPS-sel, mekkora bitrátával, RTT-vel és mért késéssel néz.
- **Windows:** a DXGI Desktop Duplication abban a pillanatban adja a képet, amikor a Windows összeállította (az
  egérmutató berajzolva, a méretezés a GPU-n); **NVIDIA kártyán NVENC** kódolja közvetlenül ezt a textúrát (P1,
  „ultra low latency”, CBR, B-kocka nélkül, kulcskocka csak kérésre), **Intel / AMD kártyán** a videókártya saját
  Media Foundation kódolója (BGRA → NV12 a GPU-n, BT.709), ha egyik sincs, a Windows szoftveres kódolója (1080p 30).
  A gép teljes hangja WASAPI loopbackkel – **5.1 / 7.1 térhangban** is, ha a hangszóró-beállítás az.
- **Mac:** a ScreenCaptureKit a kirajzolás pillanatában adja a képet (már a küldött méretre méretezve, YUV-ban),
  a **VideoToolbox** a hardveres kódolón kódolja (alacsony késleltetésű sebességszabályozás); a gép hangja
  macOS 13-tól, semmit nem kell mellé telepíteni.
- **Nézés:** kód vagy link; a képkockákat a videókártya dekódolja (Windowson Media Foundation alacsony késleltetésű
  módban + flip-model swap chain, ami minden képet azonnal kirak; Macen VideoToolbox → IOSurface közvetlenül a
  rétegre), a hang Opus → 5–100 ms-os, magától igazodó puffer. **F** / dupla kattintás: teljes képernyő, **S**:
  statisztika, **M**: némítás.
- **Frissítés:** induláskor megnézi (`native-latest.json`), van-e új verzió; Windowson letölti és csendben telepíti,
  majd újraindul; Macen letölti, kicseréli magát és újraindul. A korábbi (Electron) alkalmazás is erre frissül:
  Windowson magától (a telepítő eltávolítja az Electron-os változatot), Macen a *Letöltés* gombbal a DMG-t kapod –
  húzd az Alkalmazások mappába.

### Közvetlen mód (WebRTC nélkül – alkalmazásban és böngészőben)

A kép a saját átvitelünkkel megy, nem WebRTC-vel. Ha a megosztó és a néző is az asztali alkalmazást használja, a
lehető legrövidebb úton:

- a megosztó gép **egyszer** kódol (H.264, a videókártyán – lásd fent) minden nézőnek, és ha a kódoló épp dolgozik,
  a következő képkockát kihagyja ahelyett, hogy sorba állna (a sor csak késés); a kódolt képkockák a rögzítés után
  egyenesen az UDP-re mennek;
- a képkockák ~1,1 KB-os UDP-csomagokban mennek, AES-GCM titkosítással (a kulcsot a két alkalmazás X25519-cel
  egyezteti, a szerver sem látja a képet), a küldés gyors és csak enyhén tagolt (a célbitráta 3-szorosával);
- a néző **puffer nélkül** dolgozik: minden teljes képkockát azonnal dekódol, és egy alacsony késleltetésű
  („desynchronized”) vásznon rajzolja ki, amely nem vár a böngésző képösszeállítására; ha a gép nem bírja a tempót,
  mindig csak a legfrissebb kép kerül ki;
- elveszett csomagot a néző azonnal újrakér (NACK); ha egy kép így sem lesz teljes, kihagyja, és kulcskockát kér;
- a hang Opus, 2,5 ms-os csomagokban (az Opus legkisebb késése), 5.1/7.1 is (csatornapáronként); a néző csak 10–100 ms hangpuffert tart, amennyit a
  hálózat éppen kíván;
- kapcsolat: a két alkalmazás a szerver UDP-portján (**3479**) megtudja a saját nyilvános címét, és közvetlenül
  egymásnak küld (helyi hálózaton a helyi címen); ha ez nem megy, a szerver továbbítja a (titkosított) csomagokat
  (3479/udp, vagy a TURN szerveren át); ha semmilyen UDP nem jut át (szigorú tűzfal), a szerver HTTPS-portján,
  WebSocketben mennek. Két alkalmazás között **nincs WebRTC**: ha a kapcsolat megszakad, újra felépül;
- a bitrátát a néző jelentései szabályozzák (csomagvesztés, kihagyott kép, növekvő RTT → kevesebb; ha minden rendben,
  lassan több); szoftveres kódolásnál, ha a kódoló nem bírja a tempót, kisebb felbontásra vált.

A néző statisztikájában (**S**) a *Mód* sorban: *Közvetlen*, a *Késés (mért)* a valódi, végponttól végpontig mért
késés (a megosztó gépen a rögzítéstől a néző vásznára rajzolásig, a két gép órájának eltérésével korrigálva).
**Böngészőben is** (Chrome, Edge, Firefox, Safari – ahol van WebCodecs H.264; `public/direct-web.js`): ugyanez a
protokoll és ugyanaz a végponttól végpontig titkosítás, de mivel böngészőből nem lehet UDP-t küldeni, a csomagok
WebSocketben, a szerver HTTPS-portján (`wss://…/direct`) mennek, és a szerver adja tovább őket (a képet nem látja).
Ezen az úton egy képkocka egy csomag (a TCP nem veszít el semmit, nincs újrakérés); ha a néző vonala nem bírja, a
megosztó eldobja a képkockákat a következő kulcskockáig ahelyett, hogy sorba állítaná őket, és a bitráta lejjebb
megy. A néző itt is puffer nélkül, minden képet azonnal kirajzol. Így megy alkalmazás → böngésző, böngésző →
böngésző és böngésző → alkalmazás. A kulcscsere a böngésző WebCrypto X25519-e, ha van, különben az alkalmazások
BearSSL-kódja WebAssemblyként; a hang a böngésző Opus-dekódolója (WebCodecs), ha van, különben a libopus
WebAssemblyként (`public/ss-web.wasm`, forrás: `native/web`). A megosztó a képet és a hangot a
`MediaStreamTrackProcessor`-ral olvassa (Chrome, Edge), ahol ez nincs (Firefox, Safari), egy videóelemből
(`requestVideoFrameCallback`) és egy AudioWorkletből. **WebRTC sehol nincs** az oldalon; ahol a böngésző ezt sem
tudja (nagyon régi, WebCodecs nélküli böngésző), a néző üzenetet kap, és a megosztó nézőlistájában is látszik, miért
nem kap képet.

Windowson az első közvetlen kapcsolatnál a Windows tűzfal rákérdezhet a Screen Share-re: engedélyezd (enélkül is
működik, de a helyi hálózaton a közvetlen út biztosabb). A szerveren a telepítő megnyitja a **3479/udp** portot; ha a
szolgáltatónak külön tűzfala van, ott is engedd.

**Telepítés:** az alkalmazás nincs Microsofttal / Apple-lel aláírva (ahhoz fizetős fejlesztői fiók kell), ezért:

- Windows: „A Windows megvédte a számítógépet” → **További információ → Futtatás mindenképp**.
- Mac: első indításkor jobb klikk az alkalmazáson → **Megnyitás** (vagy *Rendszerbeállítások → Adatvédelem és
  biztonság → Megnyitás mindenképp*), majd engedélyezd a **Képernyőfelvételt** és indítsd újra.

**Mac hang:** macOS 13 vagy újabb kell, külön program nem. Első megosztáskor a macOS rákérdez a
**rendszerhang-felvételre** – engedélyezd. Ha korábban elutasítottad: *Rendszerbeállítások → Adatvédelem és biztonság →
Képernyő- és rendszerhangfelvétel* → kapcsold be a Screen Share-t a rendszerhang-felvételnél, majd indítsd újra.

**7.1 hang Windowson:** *Gépház → Rendszer → Hang → (kimeneti eszköz) → Formátum / Hangszóró-beállítás*: 7.1 vagy
5.1 – ha ez sztereó, sztereó megy. A megosztó panel **Hang** dobozában látszik: *Hang megosztva: 7.1 térhang*, a
nézőlistában pedig, ki kapja térhangban. A néző statisztikájában (**S**) a *Hang* sorban: *7.1 térhang*.


A `native/` mappában van. A GitHub Actions (`.github/workflows/native.yml`) minden változásnál lefordítja (Windows:
MSVC, Mac: arm64 + x86_64 egy alkalmazásba fűzve), és a CI gépeken valódi megosztással és nézéssel teszteli (az
alkalmazás megoszt egy natív nézőnek, és néz egy natív megosztót), majd feltölti a `desktop-latest` kiadásba. Kézzel:

```bash
cmake -S native -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build   # Windows (MSVC) / Mac / Linux (csak a mag + tesztek)
build/sscore_test                                                            # a mag tesztjei
```

Macen egy architektúra: `-DCMAKE_OSX_ARCHITECTURES=arm64` (vagy `x86_64`) `-DCMAKE_OSX_DEPLOYMENT_TARGET=12.3`.
A `build/ss-cli` parancssoros megosztó / néző (teszteléshez: `ss-cli host --server … --password … --h264 fájl`,
`ss-cli view --server … --code …`). A korábbi Electron alkalmazás (`desktop/`) már nem készül.

## Fejlesztés

```bash
npm install
HOST_PASSWORD=titok123 npm start     # http://localhost:8080
npm test                              # szerver, közvetlen mód (UDP-átvitel, relé, hang), WebAssembly tesztek
native/web/build.sh                   # public/ss-web.wasm újraépítése (clang, lld, wasi-libc kell)
```

A képernyőmegosztás csak HTTPS-en vagy `localhost`-on működik. TURN nélkül (üres `TURN_SECRET`) az alkalmazások a
szerver 3479/udp reléjét vagy a WebSocketet használják. A `ss-web.wasm` reprodukálható: a CI újraépíti a forrásból,
és ellenőrzi, hogy bájtra egyezik a feltöltöttel.

## Hogyan működik

- `server/server.js` – Node.js szerver (egyetlen függőség: `ws`): kiszolgálja az oldalt biztonsági fejlécekkel (CSP stb.),
  a `/ws` WebSocketen szobákat kezel és továbbítja a kapcsolatfelépítő üzeneteket (a közvetlen mód ajánlatai) a
  megosztó és a nézők között, a `/direct` WebSocketen a böngészők (titkosított) csomagjait adja tovább, és az
  alkalmazásoknak 24 órás TURN jelszavakat ad (TURN REST API). Jelszó- és csatlakozási próbálkozások IP-nként korlátozva.
- `public/` – a böngészős oldal, WebRTC nélkül: a megosztó a képernyőt `getDisplayMedia()`-val rögzíti, WebCodecs-szel
  egyszer kódolja (H.264 + Opus), és a közvetlen mód protokolljával küldi minden nézőnek (lásd lent).
- Közvetlen mód: `native/core` (a natív alkalmazások közös C++ magja: X25519 + AES-GCM titkosítás – BearSSL –,
  darabolás, NACK, kulcskocka-kérés, óra-egyeztetés, lyukasztás a NAT-on, szerveres / TURN relé, WebSocket-tartalék,
  a megosztó és a néző logikája), `server/direct-relay.js` (a szerver 3479/udp portja: címfelderítés és relé, csak a
  munkamenet titkával regisztrált címek felé), `public/app.js` *DIRECT MODE* része (WebCodecs kódolás/dekódolás,
  kirajzolás, hang), `public/direct-audio.js` (hanglejátszó AudioWorklet), `public/direct-web.js` (ugyanez a
  böngészőben: WebCrypto / WebAssembly + WebSocket a szerveren át), `public/capture-audio.js` (a megosztó hangja
  ott, ahol nincs `MediaStreamTrackProcessor`), `public/ss-web.js` + `native/web` (X25519 és Opus-dekódoló a
  BearSSL-ből és a libopusból, WebAssemblyként), `desktop/direct.js` (ugyanez Node-ban – a korábbi Electron
  alkalmazásé, a tesztek használják).
- `native/windows` (Win32 + Direct3D 11: DXGI képrögzítés, NVENC – az NVIDIA Video Codec SDK API-fejléce a
  `native/third_party/nvenc` mappában, MIT licenc –, Media Foundation kódolás / dekódolás, WASAPI hang, NSIS
  telepítő), `native/mac` (AppKit, ScreenCaptureKit, VideoToolbox, Core Audio), `native/common` (a nézők hangpuffere).
- `docker-compose.yml` – `app` (Node), `caddy` (HTTPS, beállítás: `deploy/caddy/Caddyfile`), `turn` (coturn, host
  hálózaton; a belső/foglalt címek felé tiltott relézéssel).
