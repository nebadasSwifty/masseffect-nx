#!/usr/bin/env python3
"""Runs tools/build_full_nsp.py deterministically for test/nsp_python.test.js (the oracle of the JS port).

    python3 nsp_oracle.py pack <json>      json: {"argv": [...], "aes_keys": {"0": hex, "1": hex, "2": hex},
                                                  "rsa_pem": path, "salt": hex}
    python3 nsp_oracle.py basemeta <json>  json: {"base": path, "keys": path, "output": path}

The packer draws a random AES key per NCA and RSA-PSS uses a random salt; here the AES key comes from the content type
and the salt is fixed (the JS test passes the same values), so both packers must write identical bytes. Throwaway test
keys only.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import build_full_nsp as b  # noqa: E402

from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import padding  # noqa: E402


def mgf1(seed, length):
    out = b""
    counter = 0
    while len(out) < length:
        out += b.sha256(seed + counter.to_bytes(4, "big"))
        counter += 1
    return out[:length]


def pss_encode(message, salt, em_bits=2047):
    em_len = (em_bits + 7) // 8
    m_hash = b.sha256(message)
    h = b.sha256(bytes(8) + m_hash + salt)
    db = bytes(em_len - len(salt) - 32 - 2) + b"\x01" + salt
    masked = bytearray(x ^ y for x, y in zip(db, mgf1(h, em_len - 33)))
    masked[0] &= 0xFF >> (8 * em_len - em_bits)
    return bytes(masked) + h + b"\xbc"


class FixedSaltSigner:
    """Signs like cryptography's RSA-PSS (MGF1-SHA256, salt 32) but with a fixed salt; checks its own signature."""

    def __init__(self, key, salt):
        self.key, self.salt = key, salt

    def public_key(self):
        return self.key.public_key()

    def sign(self, data, _padding, _algorithm):
        priv = self.key.private_numbers()
        n, d = priv.public_numbers.n, priv.d
        em = pss_encode(bytes(data), self.salt)
        sig = pow(int.from_bytes(em, "big"), d, n).to_bytes(256, "big")
        self.key.public_key().verify(sig, bytes(data), padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32),
                                     hashes.SHA256())
        return sig


def pack(cfg):
    aes = {int(k): bytes.fromhex(v) for k, v in cfg["aes_keys"].items()}
    original_init = b.NcaBuilder.__init__

    def init(self, keys, title_id, content_type, sections, key_area_key=None, sign_key=None):
        original_init(self, keys, title_id, content_type, sections, key_area_key=aes[content_type], sign_key=sign_key)

    b.NcaBuilder.__init__ = init
    with open(cfg["rsa_pem"], "rb") as fh:
        key = serialization.load_pem_private_key(fh.read(), password=None)
    signer = FixedSaltSigner(key, bytes.fromhex(cfg["salt"]))
    b.load_sign_key = lambda _path: signer
    return b.main(cfg["argv"] + ["--sign-key", cfg["rsa_pem"]])


def basemeta(cfg):
    keys = b.load_keys(cfg["keys"])
    meta = b.metadata_from_nsp(cfg["base"], keys, log=lambda *_: None)
    with open(cfg["output"], "w", encoding="utf-8") as fh:
        json.dump(meta, fh)
    return 0


if __name__ == "__main__":
    with open(sys.argv[2], "r", encoding="utf-8") as fh:
        config = json.load(fh)
    sys.exit({"pack": pack, "basemeta": basemeta}[sys.argv[1]](config))
