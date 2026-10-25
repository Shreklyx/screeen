#!/bin/bash
# Screen Share for macOS: signs the app with this Mac's own signing identity – made once, kept in a
# keychain of its own – so that macOS remembers what was allowed (screen and system sound recording,
# the password in the keychain, the local network) across updates. The builds are signed "ad hoc", and
# an ad-hoc signature is another identity for every version: macOS would ask again after each update.
#
#   sign_local.sh sign <app>                               signs <app>
#   sign_local.sh swap <pid> <app> <new app | -> <0 | 1>   once <pid> has quit: <new app> (signed if it
#                                                          can be) or a signed copy of <app> in place of
#                                                          <app>; 1: then starts it
set -u
NAME="Screen Share Local Signing"
DIR="$HOME/Library/Application Support/Screen Share/Signing"
KC="$DIR/signing.keychain-db"
PWF="$DIR/password"

log() { echo "$(date '+%F %T') $*"; }

# the identity: a self-signed code signing certificate, in a keychain of its own with its password beside it
identity() {
  if [ -f "$KC" ] && [ -f "$PWF" ] && security unlock-keychain -p "$(cat "$PWF")" "$KC" 2>/dev/null &&
    security find-identity -p codesigning "$KC" 2>/dev/null | grep -q "$NAME"; then
    return 0
  fi
  log "making the signing identity"
  rm -rf "$DIR"
  mkdir -p "$DIR" && chmod 700 "$DIR" || return 1
  local pw tmp list ok=1
  pw=$(/usr/bin/openssl rand -hex 16) || return 1 # (no "tr </dev/urandom | head": the app ignores SIGPIPE, and so would tr)
  tmp=$(mktemp -d) || return 1
  cat >"$tmp/c.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $NAME
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
EOF
  if /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 7300 -keyout "$tmp/k.pem" -out "$tmp/c.pem" -config "$tmp/c.cnf" &&
    /usr/bin/openssl pkcs12 -export -inkey "$tmp/k.pem" -in "$tmp/c.pem" -name "$NAME" -out "$tmp/id.p12" -passout "pass:$pw"; then
    # (creating a keychain puts it on the user's search list: the list goes back to how it was)
    list=$(security list-keychains -d user | tr '\n' ' ')
    if security create-keychain -p "$pw" "$KC"; then
      [ -n "$list" ] && eval "security list-keychains -d user -s $list"
      security set-keychain-settings "$KC" &&
        security unlock-keychain -p "$pw" "$KC" &&
        security import "$tmp/id.p12" -k "$KC" -f pkcs12 -P "$pw" -T /usr/bin/codesign &&
        security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$pw" "$KC" >/dev/null &&
        printf %s "$pw" >"$PWF" && chmod 600 "$PWF" && ok=0
    fi
  fi
  rm -rf "$tmp"
  [ $ok = 0 ] || rm -rf "$DIR"
  return $ok
}

sign() {
  identity || { log "no signing identity"; return 1; }
  xattr -cr "$1" 2>/dev/null
  local hash list r
  hash=$(security find-identity -p codesigning "$KC" | awk -v n="\"$NAME\"" 'index($0, n) { print $2; exit }')
  [ -n "$hash" ] || { log "the identity is not in its keychain"; security find-identity -p codesigning "$KC"; return 1; }
  # codesign finds identities on the keychain search list only: ours is on it while it signs
  list=$(security list-keychains -d user | tr '\n' ' ')
  eval "security list-keychains -d user -s \"\$KC\" $list"
  codesign --force --deep --keychain "$KC" --sign "$hash" "$1" && codesign --verify --deep "$1"
  r=$?
  [ -n "$list" ] && eval "security list-keychains -d user -s $list"
  [ $r = 0 ] || { log "signing failed"; security find-identity -p codesigning "$KC"; }
  return $r
}

case "${1:-}" in
  sign)
    sign "$2"
    ;;
  swap)
    pid=$2 app=$3 new=$4 start=$5
    mkdir -p "$HOME/Library/Logs"
    exec >>"$HOME/Library/Logs/Screen Share.log" 2>&1
    while kill -0 "$pid" 2>/dev/null; do sleep 0.2; done
    tmp=""
    if [ "$new" = "-" ]; then
      tmp=$(mktemp -d) && new="$tmp/Screen Share.app" && ditto "$app" "$new" && sign "$new"
      ok=$?
    else
      sign "$new" # an update goes in even if this fails (signed ad hoc, as it came)
      ok=0
    fi
    if [ $ok = 0 ]; then
      old="$app.old-$$"
      if mv "$app" "$old"; then
        if mv "$new" "$app"; then rm -rf "$old"; else mv "$old" "$app"; fi
      fi
      xattr -dr com.apple.quarantine "$app" 2>/dev/null
      log "in place: $app"
    else
      log "not signed, the app stays as it was"
    fi
    [ -n "$tmp" ] && rm -rf "$tmp"
    if [ "$start" = 1 ]; then open "$app"; fi
    [ $ok = 0 ]
    ;;
  *)
    echo "usage: $0 sign <app> | swap <pid> <app> <new app|-> <0|1>" >&2
    exit 2
    ;;
esac
