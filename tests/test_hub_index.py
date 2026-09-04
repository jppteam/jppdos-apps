"""App Hub index and signature behaviour of toolchain/hub_publish.py.

What is pinned here is the part the device depends on and CI cannot re-check
for itself: the index is the wire format described in the firmware repo's
`docs/ota-registry.md`, read by firmware that ships on its own schedule and
cannot be updated in step with this repository. So the grammar (one section per
app, `key value` lines, ASCII under 128 bytes), the metadata each app carries,
and the fixed 64-byte signature are all fixed here rather than left to whatever
the publisher happens to emit.

Skips when `cryptography` is absent, which is the only dependency either half
of the tool has.
"""

import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "toolchain" / "hub_publish.py"

pytest.importorskip("cryptography",
                    reason="toolchain/hub_publish.py needs the cryptography package")

sys.path.insert(0, str(REPO / "toolchain"))
import hub_publish as hub  # noqa: E402


MANIFEST = {
    "schema_version": 2,
    "app_id": "slots",
    "app_type": "native",
    "name": "Slots",
    "version": "2.0.0",
    "author": "evaqum",
    "sdk_min": 1,
    "sdk_max": 1,
    "entry": "slots.bin",
    "capabilities": [],
}


@pytest.fixture
def dist(tmp_path):
    """A stand-in build output tree: dist/<app_id>/ with a manifest and a blob."""
    def make(app_id="slots", **overrides):
        manifest = dict(MANIFEST, app_id=app_id, **overrides)
        app_dir = tmp_path / "dist" / app_id
        app_dir.mkdir(parents=True)
        (app_dir / "manifest.json").write_text(json.dumps(manifest))
        (app_dir / manifest["entry"]).write_bytes(b"\x7fELF" + app_id.encode() * 8)
        return tmp_path / "dist"
    return make


# --- index parsing / rendering --------------------------------------------- #

INDEX = """\
jppdos-index 1
generated 2026-09-04T00:00:00Z
apps 1

# a comment, and a blank line, both ignored

[slots]
version 2.0.0
file slots.bin 39512 2c26b46b
"""


def test_parse_index_reads_sections_and_skips_comments():
    header, sections = hub.parse_index(INDEX)
    assert dict(header)["jppdos-index"] == "1"
    assert dict(header)["apps"] == "1"
    assert dict(sections["slots"])["version"] == "2.0.0"


def test_parse_index_keeps_the_whole_value_after_the_first_space():
    _, sections = hub.parse_index("[slots]\nname Three Reel Fruit Machine\n")
    assert dict(sections["slots"])["name"] == "Three Reel Fruit Machine"


def test_render_index_round_trips():
    _, sections = hub.parse_index(INDEX)
    _, again = hub.parse_index(hub.render_index(sections))
    assert again == sections


def test_render_index_keeps_repeated_keys():
    """`file` and `cap` repeat within a section; neither may collapse."""
    section = [("file", "a.bin 1 aa"), ("file", "b.bin 2 bb"), ("cap", "ble.scan")]
    _, sections = hub.parse_index(hub.render_index({"slots": section}))
    assert sections["slots"] == section


def test_render_index_refuses_a_line_the_device_cannot_read():
    # The device reads lines into a fixed-size buffer, so this has to fail in
    # CI rather than months later on hardware.
    with pytest.raises(SystemExit):
        hub.render_index({"slots": [("name", "x" * 200)]})
    with pytest.raises(SystemExit):
        hub.render_index({"slots": [("name", "Слоты")]})


# --- value sanitising ------------------------------------------------------ #

@pytest.mark.parametrize("raw,expected", [
    ("Slots", "Slots"),
    ("Café Racer", "Cafe Racer"),      # accents folded, not dropped
    ("two\nlines", "two lines"),        # never a second line
    ("  padded   out  ", "padded out"),
])
def test_index_value_flattens_to_ascii(raw, expected):
    assert hub.index_value(raw) == expected


def test_index_value_of_an_unrepresentable_name_is_empty():
    # build_section falls back to the app id, which is ASCII by construction.
    assert hub.index_value("Слоты") == ""


# --- app sections ---------------------------------------------------------- #

def test_section_carries_the_manifest_metadata(dist):
    section = dict(hub.build_section(dist(), "slots", repo=None))
    assert section["version"] == "2.0.0"
    assert section["name"] == "Slots"
    assert section["author"] == "evaqum"
    assert section["type"] == "native"
    assert section["schema"] == "2"
    assert section["sdk_min"] == "1"
    assert section["sdk_max"] == "1"
    assert section["entry"] == "slots.bin"


def test_section_records_every_file_with_its_size_and_digest(dist):
    root = dist()
    pairs = hub.build_section(root, "slots", repo=None)
    files = {v.split(" ")[0]: v for k, v in pairs if k == "file"}
    assert set(files) == {"manifest.json", "slots.bin"}
    blob = root / "slots" / "slots.bin"
    name, size, digest = files["slots.bin"].split(" ")
    assert size == str(blob.stat().st_size)
    assert digest == hub.sha256_file(blob)


def test_section_lists_one_cap_line_per_capability(dist):
    root = dist(capabilities=["http.request", "ble.scan"])
    pairs = hub.build_section(root, "slots", repo=None)
    assert [v for k, v in pairs if k == "cap"] == ["http.request", "ble.scan"]


def test_section_falls_back_to_the_app_id_for_an_untranslatable_name(dist):
    root = dist(name="Слоты")
    assert dict(hub.build_section(root, "slots", repo=None))["name"] == "slots"


def test_section_refuses_a_manifest_in_the_wrong_directory(dist):
    root = dist(app_id="slots")
    (root / "slots").rename(root / "elsewhere")
    with pytest.raises(SystemExit):
        hub.build_section(root, "elsewhere", repo=None)


# --- whole index ----------------------------------------------------------- #

def test_index_has_the_magic_and_one_section_per_app(dist):
    root = dist("slots")
    dist_two = root  # same tree, second app added below
    (dist_two / "mtproto").mkdir()
    (dist_two / "mtproto" / "manifest.json").write_text(
        json.dumps(dict(MANIFEST, app_id="mtproto", entry="mtproto.bin")))
    (dist_two / "mtproto" / "mtproto.bin").write_bytes(b"x")

    text = hub.build_index(dist_two, repo=None)
    header, sections = hub.parse_index(text)
    assert text.startswith("jppdos-index 1\n")
    assert dict(header)["apps"] == "2"
    assert list(sections) == ["mtproto", "slots"]   # sorted, so runs compare


def test_index_values_are_single_line_and_ascii(dist):
    """The device parses this with a fixed-size line buffer and no unescaping."""
    text = hub.build_index(dist(), repo=None)
    for line in text.splitlines():
        assert line.isascii()
        assert len(line) < 128


def test_index_records_provenance_from_git(dist):
    """Per-app commit and date come from the app's own last commit."""
    root = dist()
    repo = root.parent
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.email", "t@example.com"], cwd=repo, check=True)
    subprocess.run(["git", "config", "user.name", "T"], cwd=repo, check=True)
    (repo / "apps").mkdir()
    (repo / "apps" / "slots").mkdir()
    (repo / "apps" / "slots" / "manifest.json").write_text("{}")
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)
    subprocess.run(["git", "commit", "-qm", "add slots"], cwd=repo, check=True)

    _, sections = hub.parse_index(hub.build_index(root, repo=repo))
    section = dict(sections["slots"])
    assert len(section["commit"]) == 40
    assert section["date"].endswith("Z")


def test_index_needs_at_least_one_app(tmp_path):
    (tmp_path / "dist").mkdir()
    with pytest.raises(SystemExit):
        hub.build_index(tmp_path / "dist", repo=None)


# --- signatures ------------------------------------------------------------ #

@pytest.fixture(scope="module")
def keypair():
    from cryptography.hazmat.primitives.asymmetric import ec
    key = ec.generate_private_key(ec.SECP256R1())
    return key, key.public_key()


def test_signature_is_64_raw_bytes(keypair):
    key, _ = keypair
    assert len(hub.sign_bytes(key, b"payload")) == hub.SIG_LEN == 64


def test_signature_round_trips(keypair):
    key, pub = keypair
    assert hub.verify_bytes(pub, b"payload", hub.sign_bytes(key, b"payload"))


def test_tampered_payload_fails(keypair):
    key, pub = keypair
    assert not hub.verify_bytes(pub, b"payloae", hub.sign_bytes(key, b"payload"))


def test_tampered_signature_fails(keypair):
    key, pub = keypair
    sig = bytearray(hub.sign_bytes(key, b"payload"))
    sig[0] ^= 0x01
    assert not hub.verify_bytes(pub, b"payload", bytes(sig))


def test_wrong_length_signature_fails(keypair):
    key, pub = keypair
    assert not hub.verify_bytes(pub, b"payload", hub.sign_bytes(key, b"payload")[:-1])


def test_public_key_is_64_raw_bytes(keypair):
    _, pub = keypair
    assert len(hub.public_key_raw(pub)) == 64


# --- the CLI the workflow actually calls ----------------------------------- #

def run(*args, **kw):
    return subprocess.run([sys.executable, str(TOOL), *args],
                          capture_output=True, text=True, **kw)


@pytest.fixture
def seckey(tmp_path, keypair):
    """The signing key on disk, as the firmware repo's keygen would write it."""
    from cryptography.hazmat.primitives import serialization
    key, pub = keypair
    sec_path = tmp_path / "ota_seckey.pem"
    pub_path = tmp_path / "ota_pubkey.pem"
    sec_path.write_bytes(key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption()))
    pub_path.write_bytes(pub.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo))
    return sec_path, pub_path


def test_cli_sign_verify_round_trip(tmp_path, seckey):
    sec_path, pub_path = seckey
    blob = tmp_path / "slots.bin"
    blob.write_bytes(b"\x00\x01\x02\x03" * 64)

    assert run("sign", "--key", str(sec_path), str(blob)).returncode == 0
    assert (tmp_path / "slots.bin.sig").stat().st_size == 64
    assert run("verify", "--pubkey", str(pub_path), str(blob)).returncode == 0
    # The workflow's post-signing self-check derives the public half itself.
    assert run("verify", "--key", str(sec_path), str(blob)).returncode == 0

    blob.write_bytes(b"\xff" * 256)
    assert run("verify", "--pubkey", str(pub_path), str(blob)).returncode != 0


def test_cli_verify_reports_a_missing_sig(tmp_path, seckey):
    _, pub_path = seckey
    blob = tmp_path / "unsigned.bin"
    blob.write_bytes(b"x")
    result = run("verify", "--pubkey", str(pub_path), str(blob))
    assert result.returncode != 0
    assert "MISSING" in result.stderr


def test_cli_index_build_writes_an_index(tmp_path, dist):
    root = dist()
    out = tmp_path / "index"
    result = run("index-build", "--dist", str(root), "--out", str(out),
                 "--repo", str(root.parent))
    assert result.returncode == 0, result.stderr
    text = out.read_text()
    assert text.startswith("jppdos-index 1\n")
    assert "[slots]" in text


def test_cli_pubkey_c_matches_the_hex_form(seckey):
    sec_path, pub_path = seckey
    hex_out = run("pubkey-c", "--pubkey", str(pub_path), "--format", "hex")
    c_out = run("pubkey-c", "--key", str(sec_path))
    assert hex_out.returncode == 0 and c_out.returncode == 0
    raw = bytes.fromhex(hex_out.stdout.strip())
    assert len(raw) == 64
    # The firmware pastes this array in; it must be the same 64 bytes.
    assert f"0x{raw[0]:02x}" in c_out.stdout
    assert "JPP_OTA_PUBKEY[64]" in c_out.stdout
