#!/usr/bin/env python3
"""Signs a firmware image for Bluetooth updates.

  python scripts/sign_firmware.py <build folder>/firmware.ino.bin [keys/ota_private.pem]

Writes <name>.signed.bin = image + signature (64 bytes, ECDSA P-256 r||s over
SHA-256 of the image) + b"AWSIG001". The app strips the trailer and sends the
signature separately. Needs: pip install cryptography
"""
import os, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

TRAILER = b"AWSIG001"
if len(sys.argv) < 2:
    sys.exit(__doc__)
img_path = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
key_path = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("OTA_KEY_FILE", os.path.join(root, "keys", "ota_private.pem"))
img = open(img_path, "rb").read()
if img.endswith(TRAILER):
    sys.exit("Already signed")
if not img or img[0] != 0xE9:
    sys.exit("Not an ESP32 app image (.ino.bin)")
key = serialization.load_pem_private_key(open(key_path, "rb").read(), password=None)
r, s = decode_dss_signature(key.sign(img, ec.ECDSA(hashes.SHA256())))
sig = r.to_bytes(32, "big") + s.to_bytes(32, "big")
out = os.path.splitext(img_path)[0] + ".signed.bin"
open(out, "wb").write(img + sig + TRAILER)
print(f"Signed: {out}")
