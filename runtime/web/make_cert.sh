#!/bin/sh
# Certificates for serving the game to your own devices over https:// (serve.py --cert/--key/--ca).
#
#   make_cert.sh <output directory> <LAN address> [more addresses...]
#
# Makes a local certificate authority (ca.pem, kept and reused on later runs, so devices that trust
# it keep working) and a server certificate signed by it for the given addresses plus localhost
# (server.pem, server-key.pem). A bare self-signed certificate is not enough for iPhones: Safari can
# refuse to let you past its warning. Install ca.pem on such a device instead
# (serve.py --ca offers it at http://<address>:<port>/ca.crt) and trust it.
# The server certificate follows Apple's TLS rules: subjectAltName, serverAuth extended key usage,
# no CA flag, under 398 days. Run this again when it expires or the computer's address changes.
set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <output directory> <LAN address> [more addresses...]" >&2
    exit 2
fi
out=$1
shift
mkdir -p "$out"
cd "$out"

if [ ! -f ca.pem ] || [ ! -f ca-key.pem ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -keyout ca-key.pem -out ca.pem \
        -subj "/CN=WiiCompiled local CA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
    chmod 600 ca-key.pem
fi

san="DNS:localhost,IP:127.0.0.1"
for address in "$@"; do
    case $address in
        *[!0-9.:a-fA-F]*) san="$san,DNS:$address" ;;
        *) san="$san,IP:$address" ;;
    esac
done

extensions=$(mktemp)
trap 'rm -f "$extensions" server.csr' EXIT
cat > "$extensions" <<EOF
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=$san
EOF

openssl req -newkey rsa:2048 -nodes -keyout server-key.pem -out server.csr -subj "/CN=WiiCompiled local"
chmod 600 server-key.pem
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca-key.pem -CAcreateserial -days 397 \
    -extfile "$extensions" -out server.pem
echo "server certificate for $san in $(pwd)/server.pem (CA: $(pwd)/ca.pem)"
