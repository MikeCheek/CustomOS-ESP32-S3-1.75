#!/usr/bin/env python3
"""Creates the key pair for signed firmware updates (see ota_pubkey.h).

Writes keys/ota_private.pem (secret - keep a backup, git-ignored) and
rewrites ota_pubkey.h with the public key. Needs: pip install cryptography
"""
import os, sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
key_path = os.path.join(root, "keys", "ota_private.pem")
if os.path.exists(key_path) and "--force" not in sys.argv:
    sys.exit(f"{key_path} already exists (use --force to replace it - old signed builds stop being accepted)")
os.makedirs(os.path.dirname(key_path), exist_ok=True)
key = ec.generate_private_key(ec.SECP256R1())
with open(key_path, "wb") as f:
    f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                              serialization.NoEncryption()))
pub = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
hdr = os.path.join(root, "ota_pubkey.h")
text = open(hdr).read()
start = text.index("#define OTA_PUBKEY_SET")
arr = ", ".join(f"0x{b:02x}" for b in pub)
text = text[:start] + f"#define OTA_PUBKEY_SET 1\n// Uncompressed point: 0x04 || X(32) || Y(32)\nstatic const uint8_t OTA_PUBKEY[65] = {{ {arr} }};\n"
open(hdr, "w").write(text)
print(f"Private key: {key_path}\nPublic key written to {hdr}. Flash this build once over USB.")
