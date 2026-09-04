#!/usr/bin/env python3
"""
J++Device App Hub publishing tool.

Signs app packages and writes the plaintext index a device reads to discover
what is in the App Hub.  Used by .github/workflows/app-hub.yml; also runnable
by hand for verification and one-off re-signing.

Requires: cryptography (pip install cryptography)

This is the App Hub half of the OTA registry.  The wire contract — bucket
layout, signature encoding, index grammar — is specified once, in the firmware
repository's `docs/ota-registry.md`, and is shared with `ota.yml` there and
with the firmware that reads both indexes.  The key and index helpers below are
deliberately the same code as that repo's `scripts/ota_publish.py`: two repos
publish into one bucket for one parser, and a divergence between them is a
device that stops updating, so the duplication is the cheaper risk.

Subcommands
-----------
sign FILE [FILE ...]
    Write FILE.sig next to each FILE.

verify FILE [FILE ...]
    Check each FILE against FILE.sig, using --pubkey or, as a post-signing
    self-check, the public half of --key.  Exits non-zero if any file fails.

pubkey-c
    Print the public key as the 64-byte C array the firmware embeds, or as raw
    hex with --format hex.

index-build
    Write the App Hub index for a built dist/ tree, deriving each app's
    metadata from its manifest, its file sizes and digests from disk, and its
    release date from the last commit that touched its sources.

There is deliberately NO keygen here.  One key signs both prefixes of the
bucket, it is generated once with the firmware repo's
`scripts/ota_publish.py keygen`, and it cannot be rotated without reflashing
every fielded device over USB.  A second way to make one is a second way to
end up with two.

Signature format
----------------
ECDSA over the NIST P-256 curve (secp256r1), SHA-256 message digest.  The .sig
file is exactly 64 raw bytes -- r || s, each a 32-byte big-endian integer.
Not DER: a fixed-size signature means the device reads 64 bytes and hands them
straight to mbedtls with no ASN.1 parser in the way.

The public key is hardcoded in the firmware as the 64-byte raw affine point
X || Y (i.e. the SEC1 uncompressed encoding minus its leading 0x04 tag).

Index format
------------
See docs/ota-registry.md in the firmware repository.  Same line-oriented format
as the firmware's `latest`, with one section per app instead of one per release
channel:

    jppdos-index 1
    generated 2026-09-04T12:34:56Z
    commit 7a7266b273baf67727a577a889b2733768a40cfa
    apps 1

    [slots]
    version 2.0.0
    name Slots
    ...
    entry slots.bin
    file slots.bin 39512 2c26b46b68ffc68ff99b453c1d304134...
    file manifest.json 212 116cb1e3b9394f8401d41287512a7277...

Blank lines and lines starting with '#' are ignored; "[name]" opens a section;
every other line is a key, one space, and the rest of the line as the value.
The section name is the app_id, and is also the directory the app's files live
in under the bucket prefix.  A `file` value carries three fields — name, size
in bytes, lowercase SHA-256 — because an app package holds a variable number of
files and fixed per-file keys cannot express that; everything else is a plain
one-value key, `cap` repeating once per declared capability.

Unlike the firmware's index, this one is rebuilt in full on every publish: each
run builds every app in the repository, so there is no other channel's entry to
preserve and nothing to merge into.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import unicodedata
from datetime import datetime, timezone
from pathlib import Path

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, utils as asym_utils
    from cryptography.exceptions import InvalidSignature
except ImportError:  # pragma: no cover - exercised only without the dep
    print("Error: cryptography is required.  Install with: pip install cryptography",
          file=sys.stderr)
    raise SystemExit(1)


SIG_SUFFIX = ".sig"
SIG_LEN = 64
INDEX_MAGIC = "jppdos-index"
INDEX_FORMAT = 1
# docs/ota-registry.md: "Lines are LF-terminated ASCII, under 128 bytes."  The
# device reads them into a fixed-size buffer, so this is a hard limit, not a
# style rule -- render_index refuses to emit a line that breaks it.
MAX_LINE = 128
# jpp_str_name_valid: [A-Za-z0-9_] segments joined by single dots.  Also what
# keeps an app_id safe as a "[section]" header and as a path in the bucket.
APP_ID_RE = re.compile(r"^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)*$")


# --------------------------------------------------------------------------
# keys and signatures
# --------------------------------------------------------------------------

def load_private_key(path=None, env=None):
    """Load the signing key from a PEM file or, preferably in CI, from an
    environment variable holding the PEM text."""
    if env:
        pem = os.environ.get(env)
        if not pem:
            raise SystemExit(f"Error: environment variable {env} is empty or unset.")
        data = pem.encode()
    elif path:
        data = Path(path).read_bytes()
    else:
        raise SystemExit("Error: one of --key or --key-env is required.")

    key = serialization.load_pem_private_key(data, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey):
        raise SystemExit("Error: signing key is not an EC key.")
    if not isinstance(key.curve, ec.SECP256R1):
        raise SystemExit(f"Error: signing key uses {key.curve.name}, expected secp256r1.")
    return key


def load_public_key(path=None, env=None):
    if env:
        pem = os.environ.get(env)
        if not pem:
            raise SystemExit(f"Error: environment variable {env} is empty or unset.")
        data = pem.encode()
    elif path:
        data = Path(path).read_bytes()
    else:
        raise SystemExit("Error: one of --pubkey or --pubkey-env is required.")
    key = serialization.load_pem_public_key(data)
    if not isinstance(key, ec.EllipticCurvePublicKey):
        raise SystemExit("Error: public key is not an EC key.")
    return key


def public_key_raw(pub):
    """64 raw bytes: the SEC1 uncompressed point minus its 0x04 tag."""
    point = pub.public_bytes(serialization.Encoding.X962,
                             serialization.PublicFormat.UncompressedPoint)
    assert len(point) == 65 and point[0] == 0x04
    return point[1:]


def sign_bytes(key, data):
    """ECDSA-P256/SHA-256, returned as fixed-width r || s."""
    der = key.sign(data, ec.ECDSA(hashes.SHA256()))
    r, s = asym_utils.decode_dss_signature(der)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_bytes(pub, data, sig):
    if len(sig) != SIG_LEN:
        return False
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    try:
        pub.verify(asym_utils.encode_dss_signature(r, s), data,
                   ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def c_array(raw, name="JPP_OTA_PUBKEY"):
    lines = ["/* ECDSA P-256 public key, raw X || Y (64 bytes). */",
             f"static const uint8_t {name}[64] = {{"]
    for i in range(0, len(raw), 8):
        chunk = ", ".join(f"0x{b:02x}" for b in raw[i:i + 8])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# index parsing / rendering
# --------------------------------------------------------------------------

def parse_index(text):
    """-> (header_pairs, {section: [(key, value), ...]}) preserving order.

    Unknown keys and unknown sections are kept verbatim, so a newer publisher
    adding a field cannot be silently dropped by an older one.
    """
    header = []
    sections = {}
    current = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            sections.setdefault(current, [])
            continue
        key, _, value = line.partition(" ")
        pair = (key, value.strip())
        if current is None:
            header.append(pair)
        else:
            sections[current].append(pair)
    return header, sections


def render_index(sections, header=(), generated=None):
    """Render sections in the order given, after the magic and any extra header.

    Every line is checked against the format's two hard limits rather than
    trusted: an over-long or non-ASCII line would not fail here, it would fail
    on a device, months later, with no way to tell why.
    """
    out = [f"{INDEX_MAGIC} {INDEX_FORMAT}",
           f"generated {generated or iso_now()}"]
    out.extend(f"{k} {v}" for k, v in header)
    for name, pairs in sections.items():
        if not pairs:
            continue
        out.append("")
        out.append(f"[{name}]")
        out.extend(f"{k} {v}" for k, v in pairs)

    for line in out:
        if not line.isascii():
            raise SystemExit(f"Error: index line is not ASCII: {line!r}")
        if len(line) >= MAX_LINE:
            raise SystemExit(
                f"Error: index line is {len(line)} bytes, the format allows "
                f"under {MAX_LINE}: {line!r}")
    return "\n".join(out) + "\n"


def section_get(pairs, key, default=None):
    for k, v in pairs:
        if k == key:
            return v
    return default


def iso_now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def iso_utc(timestamp):
    return datetime.fromtimestamp(float(timestamp), timezone.utc) \
                   .strftime("%Y-%m-%dT%H:%M:%SZ")


def index_value(value, limit=96):
    """Flatten a manifest value into something the line format can carry.

    A manifest is JSON and may hold anything; the index is line-oriented ASCII,
    so a newline in an app name would corrupt the file rather than just look
    odd, and a non-Latin one would be unreadable to a parser that assumes
    ASCII.  Accents are folded (NFKD, drop the combining marks) so "Café"
    survives as "Cafe"; anything with no ASCII form at all is dropped, and the
    caller falls back to the app id.
    """
    text = unicodedata.normalize("NFKD", str(value))
    text = text.encode("ascii", "ignore").decode("ascii")
    text = "".join(c if 0x20 <= ord(c) < 0x7F else " " for c in text)
    return " ".join(text.split())[:limit].strip()


# --------------------------------------------------------------------------
# app sections
# --------------------------------------------------------------------------

def git(repo, *args):
    try:
        out = subprocess.run(["git", "-C", str(repo), *args],
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip() or None


def last_commit(repo, path):
    """(sha, unix timestamp) of the last commit touching `path`, or None."""
    line = git(repo, "log", "-1", "--format=%H %ct", "--", str(path))
    if not line or " " not in line:
        return None
    sha, _, ts = line.partition(" ")
    try:
        return sha, float(ts)
    except ValueError:
        return None


def app_files(app_dir):
    """Every published file under an app dir, relative and sorted."""
    found = []
    for path in Path(app_dir).rglob("*"):
        # Signatures are written after the index and are implied by the
        # convention -- every object has one -- so they are never listed.
        if path.is_file() and path.suffix != SIG_SUFFIX:
            found.append(path.relative_to(app_dir).as_posix())
    return sorted(found)


def build_section(dist, app_id, repo=None, source_root="apps", provenance=None):
    """One app's index section, from its staged build output."""
    app_dir = Path(dist) / app_id
    manifest = json.loads((app_dir / "manifest.json").read_text())

    # jppd-build stages output as dist/<manifest app_id>/, so a mismatch means
    # the tree was assembled by something else.
    declared = manifest.get("app_id")
    if declared and declared != app_id:
        raise SystemExit(f"Error: {app_dir}/manifest.json declares app_id "
                         f"{declared!r} but sits in {app_id}/.")
    if not APP_ID_RE.match(app_id):
        raise SystemExit(f"Error: {app_id!r} is not a valid app id.")

    pairs = []

    def put(key, value):
        if value is None or value == "":
            return
        pairs.append((key, index_value(value)))

    # schema and sdk first: they are what a device checks before it bothers
    # reading the rest of the record.
    put("schema", manifest.get("schema_version"))
    put("version", manifest.get("version"))
    put("name", index_value(manifest.get("name", "")) or app_id)
    put("author", manifest.get("author"))
    put("type", manifest.get("app_type"))
    if manifest.get("sdk_min") is not None:
        put("sdk_min", manifest["sdk_min"])
    if manifest.get("sdk_max") is not None:
        put("sdk_max", manifest["sdk_max"])
    # One line per capability rather than a comma-joined list: the full set is
    # eleven names and would run past the format's 128-byte line limit, and a
    # silently truncated capability list is exactly the kind of thing a device
    # would act on.  Repeated keys are already how `file` works.
    for cap in manifest.get("capabilities") or []:
        pairs.append(("cap", index_value(cap)))

    # Per-app provenance: the last commit that touched this app's *sources*,
    # not the build.  An app nobody has changed keeps its original release date
    # even though every push to master republishes it.
    prov = None
    if repo is not None:
        prov = last_commit(repo, Path(source_root) / app_id)
    prov = prov or provenance
    if prov:
        put("commit", prov[0])
        put("date", iso_utc(prov[1]))

    put("entry", manifest.get("entry"))

    files = app_files(app_dir)
    if not files:
        raise SystemExit(f"Error: {app_dir}/ contains no files.")
    for rel in files:
        full = app_dir / rel
        pairs.append(("file", f"{rel} {full.stat().st_size} {sha256_file(full)}"))
    return pairs


def build_index(dist, repo=None, source_root="apps", generated=None):
    dist = Path(dist)
    app_ids = sorted(p.name for p in dist.iterdir()
                     if (p / "manifest.json").is_file())
    if not app_ids:
        raise SystemExit(f"Error: no built apps under {dist}/ "
                         f"(expected {dist}/<app_id>/manifest.json).")

    # The repository's own HEAD: recorded in the header, and the fallback for
    # an app git knows nothing about (a shallow clone, or a brand-new app that
    # is being built before it is committed).
    head = last_commit(repo, ".") if repo is not None else None
    if repo is not None and head is None:
        print("Warning: no git history available -- the index will carry no "
              "commit ids and no release dates.", file=sys.stderr)

    header = []
    if head:
        header.append(("commit", head[0]))
    header.append(("apps", str(len(app_ids))))

    sections = {app_id: build_section(dist, app_id, repo, source_root, head)
                for app_id in app_ids}
    return render_index(sections, header, generated)


# --------------------------------------------------------------------------
# subcommands
# --------------------------------------------------------------------------

def cmd_pubkey_c(args):
    if args.pubkey or args.pubkey_env:
        pub = load_public_key(args.pubkey, args.pubkey_env)
    elif args.key or args.key_env:
        pub = load_private_key(args.key, args.key_env).public_key()
    else:
        raise SystemExit("Error: one of --pubkey/--pubkey-env or --key/--key-env "
                         "is required.")
    raw = public_key_raw(pub)
    print(raw.hex() if args.format == "hex" else c_array(raw, args.name))
    return 0


def cmd_sign(args):
    key = load_private_key(args.key, args.key_env)
    for name in args.files:
        path = Path(name)
        sig = sign_bytes(key, path.read_bytes())
        sig_path = Path(str(path) + SIG_SUFFIX)
        sig_path.write_bytes(sig)
        print(f"signed {path} -> {sig_path}")
    return 0


def cmd_verify(args):
    # Verifying with the key that just signed is a self-check, not a trust
    # check -- but it is the one that catches a mangled secret or a truncated
    # write before the bucket fills up with objects no device can accept.
    if args.pubkey or args.pubkey_env:
        pub = load_public_key(args.pubkey, args.pubkey_env)
    elif args.key or args.key_env:
        pub = load_private_key(args.key, args.key_env).public_key()
    else:
        raise SystemExit("Error: one of --pubkey/--pubkey-env or --key/--key-env "
                         "is required.")
    failed = 0
    for name in args.files:
        path = Path(name)
        sig_path = Path(str(path) + SIG_SUFFIX)
        if not sig_path.exists():
            print(f"MISSING  {sig_path}", file=sys.stderr)
            failed += 1
            continue
        if verify_bytes(pub, path.read_bytes(), sig_path.read_bytes()):
            print(f"ok       {path}")
        else:
            print(f"BAD      {path}", file=sys.stderr)
            failed += 1
    return 1 if failed else 0


def cmd_index_build(args):
    text = build_index(args.dist, args.repo, args.source_root)
    Path(args.out).write_text(text)
    apps = len(parse_index(text)[1])
    print(f"wrote {args.out} ({apps} app(s), {len(text)} bytes)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="hub_publish.py",
        description="Sign App Hub packages and build the App Hub index.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pubkey-c", help="print the public key as a C array")
    p.add_argument("--pubkey")
    p.add_argument("--pubkey-env")
    p.add_argument("--key", help="private key PEM file; its public half is used")
    p.add_argument("--key-env", help="env var holding the private key PEM")
    p.add_argument("--name", default="JPP_OTA_PUBKEY")
    p.add_argument("--format", choices=("c", "hex"), default="c")
    p.set_defaults(func=cmd_pubkey_c)

    p = sub.add_parser("sign", help="write FILE.sig beside each FILE")
    p.add_argument("--key", help="PEM file holding the EC private key")
    p.add_argument("--key-env", help="env var holding the private key PEM")
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("verify", help="check each FILE against FILE.sig")
    p.add_argument("--pubkey")
    p.add_argument("--pubkey-env")
    p.add_argument("--key", help="private key PEM file; its public half is used")
    p.add_argument("--key-env", help="env var holding the private key PEM")
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("index-build", help="write the App Hub index for a dist tree")
    p.add_argument("--dist", default="dist",
                   help="build output root holding <app_id>/ dirs")
    p.add_argument("--out", required=True)
    p.add_argument("--repo", default=".",
                   help="git checkout the apps were built from, for per-app "
                        "commit/date provenance")
    p.add_argument("--source-root", default="apps",
                   help="path within --repo holding app sources, so app <id> "
                        "maps to <source-root>/<id>")
    p.set_defaults(func=cmd_index_build)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
