#!/usr/bin/env bash
# Screen Share telepítő / frissítő – Debian 12/13, Ubuntu 22.04/24.04
#
# Használat a szerveren:
#   git clone https://github.com/Shreklyx/screeen.git && cd screeen
#   sudo bash deploy/install.sh
#
# Mit csinál:
#   1. felteszi a Dockert (ha még nincs),
#   2. kitölti a .env fájlt (domain, megosztási jelszó, TURN titok, szerver IP),
#   3. ha egy másik Docker alkalmazás (pl. a Sori) foglalja a 80/443-as portot, leállítja –
#      az adatai és fájljai megmaradnak, semmit nem töröl,
#   4. megnyitja a szükséges portokat a tűzfalon (ha ufw fut),
#   5. elindítja az oldalt, a Caddy-t (automatikus HTTPS) és a TURN relét
#      (+ a 3479-es UDP port: az alkalmazások közvetlen, WebRTC nélküli módja).
# Újrafuttatva frissít: letölti az új kódot és újraindít.
#
# Kérdések nélkül (a jelszót ilyenkor generálja, és a végén kiírja):
#   sudo DOMAIN=mozizas.hu ASSUME_YES=1 bash deploy/install.sh

set -euo pipefail

cd "$(dirname "$0")/.."
SCRIPT="$(pwd)/deploy/install.sh"
PROJECT=screenshare
DNS_WAIT="${DNS_WAIT:-120}"

say() { printf '\n\033[1;35m▶ %s\033[0m\n' "$*"; }
ok() { printf '  \033[32m✔\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m⚠\033[0m %s\n' "$*"; }
die() {
  printf '\n\033[31m✖ %s\033[0m\n' "$*" >&2
  exit 1
}

# Kérdés a terminálról (akkor is, ha a script csőből fut); ASSUME_YES=1 esetén az alapértéket veszi
ask() {
  local prompt="$1" default="${2:-}" answer=""
  if [ "${ASSUME_YES:-}" = "1" ] || [ ! -r /dev/tty ]; then
    printf '%s' "$default"
    return
  fi
  if [ -n "$default" ]; then
    read -r -p "  $prompt [$default]: " answer </dev/tty || true
  else
    read -r -p "  $prompt: " answer </dev/tty || true
  fi
  printf '%s' "${answer:-$default}"
}

ask_secret() {
  local prompt="$1" answer=""
  if [ "${ASSUME_YES:-}" = "1" ] || [ ! -r /dev/tty ]; then
    return
  fi
  IFS= read -r -s -p "  $prompt: " answer </dev/tty || true
  printf '\n' >/dev/tty
  printf '%s' "$answer"
}

yes_no() {
  case "${1,,}" in
    i | igen | y | yes | true | 1) return 0 ;;
    n | nem | no | false | 0) return 1 ;;
    *) die "Érvénytelen válasz: '$1' (i vagy n)" ;;
  esac
}

# .env kezelése (az értékek egyszeres idézőjelben, hogy a Docker Compose ne értelmezze a $ jelet)
env_get() {
  [ -f .env ] || return 0
  { grep -E "^$1=" .env || true; } | tail -n 1 | cut -d= -f2- | sed -e "s/^'\(.*\)'$/\1/" -e 's/^"\(.*\)"$/\1/'
}

env_set() {
  case "$2" in *"'"*) die "A(z) $1 értéke nem tartalmazhat aposztrófot (')." ;; esac
  touch .env
  KEY="$1" VAL="'$2'" awk 'BEGIN { k = ENVIRON["KEY"]; v = ENVIRON["VAL"]; done = 0 }
    index($0, k "=") == 1 { print k "=" v; done = 1; next }
    { print }
    END { if (!done) print k "=" v }' .env >.env.tmp
  mv .env.tmp .env
  chmod 600 .env
}

random_secret() {
  tr -dc 'A-Za-z0-9' </dev/urandom | head -c "${1:-40}" || true
}

# Friss szerveren az első percekben gyakran fut automatikus frissítés, ami fogja az apt-ot
APT_LOCK_CONF="$(mktemp)"
printf 'DPkg::Lock::Timeout "600";\n' >"$APT_LOCK_CONF"
export APT_CONFIG="$APT_LOCK_CONF"
trap 'rm -f "$APT_LOCK_CONF"' EXIT

apt_busy() {
  pgrep -x 'apt|apt-get|dpkg|apt.systemd.dai' >/dev/null && return 0
  pgrep -f '/unattended-upgrade( |$)' >/dev/null && return 0
  lslocks -n -o PATH 2>/dev/null | grep -qE '^/var/lib/(dpkg/lock(-frontend)?|apt/lists/lock)$'
}

wait_for_apt() {
  if command -v cloud-init >/dev/null; then cloud-init status --wait >/dev/null 2>&1 || true; fi
  local n=0
  while apt_busy; do
    [ "$n" -eq 0 ] && warn "Épp fut egy másik csomagkezelő (pl. automatikus frissítés) – megvárom, amíg befejezi (max. 15 perc)..."
    n=$((n + 1))
    [ "$n" -ge 180 ] && break
    sleep 5
  done
  [ "$n" -gt 0 ] && ok "A csomagkezelő szabad"
  return 0
}

# ---------------------------------------------------------------- ellenőrzések

[ "$(id -u)" -eq 0 ] || die "Rootként futtasd:  sudo bash deploy/install.sh"
[ -f docker-compose.yml ] && [ -f deploy/caddy/Caddyfile ] || die "A scriptet a letöltött screeen mappából futtasd."

if [ -r /etc/os-release ]; then
  . /etc/os-release
  case "${ID:-}" in
    debian | ubuntu) ok "Rendszer: ${PRETTY_NAME:-$ID}" ;;
    *) warn "Nem tesztelt rendszer (${PRETTY_NAME:-ismeretlen}) – Debian vagy Ubuntu ajánlott." ;;
  esac
fi

# ---------------------------------------------------------------- frissítés

[ "${ASSUME_YES:-}" = "1" ] && export GIT_TERMINAL_PROMPT=0
if [ -z "${SS_UPDATED:-}" ] && [ -d .git ] && [ -f .env ] && [ -n "$(env_get HOST_PASSWORD)" ]; then
  say "Már telepítve van – frissítés"
  if git pull --ff-only; then
    ok "Új kód letöltve"
    # az új telepítővel folytatjuk (ha az is változott)
    export SS_UPDATED=1
    exec bash "$SCRIPT" "$@"
  else
    warn "A git pull nem sikerült (helyi módosítás vagy hálózati hiba) – a meglévő kóddal folytatom."
  fi
fi

# ---------------------------------------------------------------- csomagok, Docker

say "Szükséges csomagok"
if ! command -v curl >/dev/null || ! command -v git >/dev/null || ! command -v ss >/dev/null || ! command -v ip >/dev/null; then
  wait_for_apt
  apt-get update -qq || die "Az apt nem futott le (lehet, hogy épp frissít a rendszer). Várj pár percet, és futtasd újra."
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq curl ca-certificates git iproute2 >/dev/null ||
    die "A csomagok telepítése nem sikerült. Várj pár percet, és futtasd újra a telepítőt."
fi
ok "curl, git, iproute2 rendben"

if ! command -v docker >/dev/null; then
  say "Docker telepítése (ez pár percig tarthat)"
  wait_for_apt
  curl -fsSL https://get.docker.com | sh ||
    die "A Docker telepítése nem sikerült (lehet, hogy épp frissít a rendszer). Várj pár percet, és futtasd újra."
fi
docker compose version >/dev/null 2>&1 || die "A 'docker compose' nem érhető el. Telepítsd a docker-compose-plugin csomagot."
systemctl enable --now docker >/dev/null 2>&1 || true
docker info >/dev/null 2>&1 || die "A Docker szolgáltatás nem fut. Indítsd el:  sudo systemctl start docker"
ok "Docker: $(docker --version | cut -d, -f1)"

# ---------------------------------------------------------------- beállítások

say "Beállítások"
[ -f .env ] || cp .env.example .env
chmod 600 .env

DOMAIN="${DOMAIN:-$(env_get DOMAIN)}"
DOMAIN="$(ask 'Domain' "${DOMAIN:-mozizas.hu}")"
DOMAIN="${DOMAIN#http://}"
DOMAIN="${DOMAIN#https://}"
DOMAIN="${DOMAIN%%/*}"
[[ "$DOMAIN" =~ ^[A-Za-z0-9.-]+\.[A-Za-z]{2,}$ ]] || [ "$DOMAIN" = "localhost" ] || die "Érvénytelen domain: $DOMAIN"
env_set DOMAIN "$DOMAIN"

GENERATED_PASSWORD=""
HOST_PASSWORD="${HOST_PASSWORD:-$(env_get HOST_PASSWORD)}"
if [ -n "$HOST_PASSWORD" ] && ! yes_no "$(ask 'Megtartod a jelenlegi megosztási jelszót? (i/n)' i)"; then
  HOST_PASSWORD=""
fi
if [ -z "$HOST_PASSWORD" ]; then
  echo "  A megosztási jelszóval lehet megosztást indítani. Nézni a linkkel bárki tud."
  HOST_PASSWORD="$(ask_secret 'Megosztási jelszó (üresen hagyva generálok egyet)')"
  if [ -z "$HOST_PASSWORD" ]; then
    HOST_PASSWORD="$(random_secret 12)"
    GENERATED_PASSWORD="$HOST_PASSWORD"
  fi
fi
[ "${#HOST_PASSWORD}" -ge 6 ] || die "A megosztási jelszó legalább 6 karakter legyen."
env_set HOST_PASSWORD "$HOST_PASSWORD"

TURN_SECRET="$(env_get TURN_SECRET)"
[[ "$TURN_SECRET" =~ ^[A-Za-z0-9]{20,}$ ]] || TURN_SECRET="$(random_secret 40)"
env_set TURN_SECRET "$TURN_SECRET"

TURN_MIN_PORT="$(env_get TURN_MIN_PORT)"
TURN_MAX_PORT="$(env_get TURN_MAX_PORT)"
[[ "$TURN_MIN_PORT" =~ ^[0-9]+$ ]] || TURN_MIN_PORT=49152
[[ "$TURN_MAX_PORT" =~ ^[0-9]+$ ]] || TURN_MAX_PORT=50151
[ "$TURN_MIN_PORT" -lt "$TURN_MAX_PORT" ] || die "Hibás TURN porttartomány: $TURN_MIN_PORT-$TURN_MAX_PORT"
env_set TURN_MIN_PORT "$TURN_MIN_PORT"
env_set TURN_MAX_PORT "$TURN_MAX_PORT"
ok ".env elmentve"

# ---------------------------------------------------------------- szerver IP

detect_ip() {
  curl -4 -fsS --max-time 8 https://1.1.1.1/cdn-cgi/trace 2>/dev/null | sed -n 's/^ip=//p' ||
    curl -4 -fsS --max-time 8 https://api.ipify.org 2>/dev/null || true
}

SERVER_IP="${SERVER_IP:-$(detect_ip)}"
SERVER_IP="$(ask 'A szerver nyilvános IPv4 címe' "$SERVER_IP")"
[[ "$SERVER_IP" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "Érvénytelen IPv4 cím: $SERVER_IP"

# Ha a nyilvános cím nincs a gép hálózati kártyáján (NAT-olt felhő), a TURN-nek meg kell adni a belső címet is
if ip -4 -o addr show 2>/dev/null | grep -q " inet $SERVER_IP/"; then
  TURN_EXTERNAL_IP="$SERVER_IP"
else
  PRIVATE_IP="$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n 1)"
  TURN_EXTERNAL_IP="$SERVER_IP${PRIVATE_IP:+/$PRIVATE_IP}"
fi
env_set TURN_EXTERNAL_IP "$TURN_EXTERNAL_IP"
ok "TURN relé címe: $TURN_EXTERNAL_IP"

# ---------------------------------------------------------------- DNS ellenőrzés

dns_points_here() {
  curl -fsS --max-time 8 -H 'accept: application/dns-json' \
    "https://cloudflare-dns.com/dns-query?name=$1&type=A" 2>/dev/null | grep -q "\"data\": *\"$SERVER_IP\""
}

if [ "$DOMAIN" != "localhost" ]; then
  say "DNS ellenőrzése"
  waited=0
  until dns_points_here "$DOMAIN"; do
    if [ "$waited" -ge "$DNS_WAIT" ]; then
      warn "A $DOMAIN nem erre a szerverre ($SERVER_IP) mutat. Állítsd be a DNS-ben: A  @  $SERVER_IP  és  A  www  $SERVER_IP"
      warn "(Cloudflare-nél \"DNS only\" / szürke felhő). Indítom így is – a Caddy újrapróbálja a tanúsítványt, ha a DNS rendben lesz."
      break
    fi
    sleep 5
    waited=$((waited + 5))
  done
  dns_points_here "$DOMAIN" && ok "$DOMAIN → $SERVER_IP"
fi

# ---------------------------------------------------------------- letöltés, build

# Minden letöltés és build még azelőtt, hogy bármit leállítanánk: ha ez elbukik, a régi oldal tovább fut
say "Képek letöltése és az oldal lefordítása (az első alkalommal pár perc)"
docker compose pull caddy turn || die "A Docker képek letöltése nem sikerült (hálózati hiba?). Semmit nem állítottam le – futtasd újra a telepítőt."
docker compose build --pull app || die "Az oldal fordítása nem sikerült. Semmit nem állítottam le – futtasd újra a telepítőt."
ok "Kész a letöltés és a build"

# ---------------------------------------------------------------- a 80/443-as port felszabadítása

say "Portok ellenőrzése"
STOPPED_PROJECTS=()
foreign_projects=()
for port in 80 443; do
  for cid in $(docker ps -q --filter "publish=$port"); do
    proj="$(docker inspect -f '{{index .Config.Labels "com.docker.compose.project"}}' "$cid" 2>/dev/null || true)"
    [ "$proj" = "$PROJECT" ] && continue
    if [ -z "$proj" ]; then
      name="$(docker inspect -f '{{.Name}}' "$cid" | sed 's#^/##')"
      die "A $port-es portot a(z) '$name' Docker konténer foglalja. Állítsd le (docker stop $name), majd futtasd újra a telepítőt."
    fi
    case " ${foreign_projects[*]} " in *" $proj "*) ;; *) foreign_projects+=("$proj") ;; esac
  done
done

for proj in "${foreign_projects[@]}"; do
  dir="$(docker ps -a --filter "label=com.docker.compose.project=$proj" --format '{{.Label "com.docker.compose.project.working_dir"}}' | head -n 1)"
  echo "  A(z) '$proj' alkalmazás foglalja a 80/443-as portot${dir:+ (mappa: $dir)}."
  echo "  Leállítom: a konténerei megszűnnek, de az adatai és fájljai (pl. filmek, adatbázis) megmaradnak."
  if yes_no "$(ask "Leállítsam a(z) '$proj' alkalmazást? (i/n)" i)"; then
    docker compose -p "$proj" down
    STOPPED_PROJECTS+=("$proj${dir:+|$dir}")
    ok "'$proj' leállítva"
  else
    die "A 80/443-as port foglalt, így nem tudok elindulni."
  fi
done

port_owner() { ss -Hltnp "( sport = :$1 )" 2>/dev/null | grep -o 'users:(("[^"]*"' | head -n 1 | cut -d'"' -f2; }
if [ -z "$(docker compose ps -q caddy 2>/dev/null)" ]; then
  for port in 80 443; do
    if ss -Hltn "( sport = :$port )" 2>/dev/null | grep -q .; then
      owner="$(port_owner "$port")"
      die "A $port-es portot már használja: ${owner:-egy másik program}. Állítsd le (pl.  sudo systemctl disable --now ${owner:-apache2}), majd futtasd újra a telepítőt."
    fi
  done
fi
if ss -Hlun "( sport = :3478 )" 2>/dev/null | grep -q . && [ -z "$(docker compose ps -q turn 2>/dev/null)" ]; then
  warn "A 3478-as UDP portot már használja egy másik program – a TURN relé nem fog elindulni."
fi
if ss -Hlun "( sport = :3479 )" 2>/dev/null | grep -q . && [ -z "$(docker compose ps -q app 2>/dev/null)" ]; then
  warn "A 3479-es UDP portot már használja egy másik program – az alkalmazások közvetlen módja csak relé nélkül megy."
fi
ok "A portok szabadok"

# ---------------------------------------------------------------- tűzfal

if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q 'Status: active'; then
  ufw allow 80/tcp >/dev/null
  ufw allow 443/tcp >/dev/null
  ufw allow 443/udp >/dev/null
  ufw allow 3478/tcp >/dev/null
  ufw allow 3478/udp >/dev/null
  ufw allow 3479/udp >/dev/null
  ufw allow "$TURN_MIN_PORT:$TURN_MAX_PORT/udp" >/dev/null
  ok "ufw: 80, 443, 3478, 3479/udp és $TURN_MIN_PORT-$TURN_MAX_PORT/udp megnyitva"
fi
warn "Ha a szolgáltatód webes felületén is van tűzfal, ott is engedd: 80 és 443 TCP, 443 UDP, 3478 TCP+UDP, 3479 UDP, $TURN_MIN_PORT-$TURN_MAX_PORT UDP."

# ---------------------------------------------------------------- indítás

say "Indítás"
docker compose up -d --remove-orphans
# a Caddy a frissített beállítást kapcsolatbontás nélkül tölti újra (ha nem változott, nem csinál semmit)
docker compose exec -T caddy caddy reload --config /etc/caddy/Caddyfile --adapter caddyfile >/dev/null 2>&1 ||
  docker compose restart caddy >/dev/null 2>&1 || true

APP_OK=0
for _ in $(seq 1 60); do
  if docker compose exec -T app node -e "fetch('http://localhost:8080/healthz').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))" 2>/dev/null; then
    APP_OK=1
    break
  fi
  sleep 2
done
if [ "$APP_OK" = "1" ]; then ok "Az oldal fut"; else warn "Az oldal nem indult el rendben. Napló: sudo docker compose logs app --tail 50"; fi

TURN_OK=0
for _ in $(seq 1 15); do
  if ss -Hlun "( sport = :3478 )" 2>/dev/null | grep -q .; then
    TURN_OK=1
    break
  fi
  sleep 2
done
if [ "$TURN_OK" = "1" ]; then ok "A TURN relé fut"; else warn "A TURN relé nem indult el. Napló: sudo docker compose logs turn --tail 50"; fi

HTTPS_OK=0
if [ "$DOMAIN" != "localhost" ]; then
  for _ in $(seq 1 30); do
    if curl -fsS --max-time 10 "https://$DOMAIN/healthz" >/dev/null 2>&1; then
      HTTPS_OK=1
      break
    fi
    sleep 4
  done
fi

say "Kész"
if [ "$HTTPS_OK" = "1" ]; then
  ok "Elérhető: https://$DOMAIN"
elif [ "$DOMAIN" != "localhost" ]; then
  warn "A https://$DOMAIN még nem válaszol. Ha a DNS most állt be, adj neki pár percet."
  warn "Napló: sudo docker compose logs caddy --tail 50"
fi
if [ -n "$GENERATED_PASSWORD" ]; then
  echo "  Generált megosztási jelszó: $GENERATED_PASSWORD   (jegyezd fel; a .env fájlban is megtalálod)"
else
  echo "  A megosztási jelszó a .env fájlban van:  sudo grep HOST_PASSWORD .env"
fi
for entry in "${STOPPED_PROJECTS[@]}"; do
  proj="${entry%%|*}"
  dir=""
  [ "$entry" != "$proj" ] && dir="${entry#*|}"
  echo "  A(z) '$proj' le van állítva, az adatai megmaradtak${dir:+ itt: $dir}."
done
echo "  Frissítés később:  sudo bash deploy/install.sh"
