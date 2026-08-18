"""Attestations: proving a result came from somewhere the submitter is not.

The problem this exists for. `build.binary_sha256` says which binary produced a
run, and a hub can check it against the digests a release published -- but the
field is inside a document the submitter wrote, so it is a claim, not a proof.
Copying an official digest into a hand-edited file earns the same mark as
actually running the official build. That is worth having (it makes honest
results comparable) and worth not calling verification.

What can be made binding is narrow and worth stating exactly:

  * A binary cannot sign its own output. Whatever key it held would be in a
    file anyone can download and read. There is no arrangement of software on
    a machine its owner controls that produces a number that owner cannot
    forge. This is not an implementation gap.

  * So the signature has to come from somewhere the submitter does not control.
    GitHub Actions will issue an OpenID Connect token for a running job, signed
    by GitHub, carrying claims about *which workflow* is running and *what kind
    of runner* it is on. The requester chooses the token's `aud`, and this is
    the hinge: the measuring workflow sets it to the SHA-256 of the result
    document. GitHub then signs a statement that binds those exact bytes to
    that workflow. Change one digit of the result and the token no longer
    matches it; mint a token for different bytes and you need GitHub's private
    key.

That leaves two honest tiers, which this module returns and the hub keeps
apart:

  ci        The job ran on a GitHub-hosted runner. The workflow is ours, the
            machine is GitHub's, the signature is GitHub's. Nothing in the
            chain is the submitter's, so the result is as good as the trust
            placed in GitHub -- this is the tier that is actually binding, and
            it can only ever describe GitHub's own hardware.

  attested  The job ran the same pinned workflow on a self-hosted runner, so
            the chain of signatures is intact and the *machine* belongs to the
            submitter. It rules out editing a JSON file; it does not rule out
            an operator who patches the runner. It is the only tier available
            for interesting hardware, and it must never be displayed as though
            it were `ci`.

Standard library only, like the rest of the hub: RS256 is an RSA verification,
which is a modular exponentiation Python's integers do natively.
"""

from __future__ import annotations

import base64
import hashlib
import json
import secrets
import threading
import time
import urllib.request
from pathlib import Path

# GitHub's OIDC issuer. Both values are part of the trust root: a token from
# anywhere else is not evidence of anything, however well-formed.
ISSUER = "https://token.actions.githubusercontent.com"
JWKS_URL = ISSUER + "/.well-known/jwks"

# The audience the measuring workflow asks for, with the result's digest
# appended. This is what ties a signature to one document.
AUDIENCE_PREFIX = "cpu-bench-result:sha256:"

# The reusable workflow whose OIDC claims a hub believes by default. A caller
# in any repository may invoke it -- that is the point, since the interesting
# hardware is not ours -- but `job_workflow_ref` names the workflow that is
# *running*, not the one that called it. A fork that edits the measuring steps
# is a different path here and stops matching.
DEFAULT_WORKFLOW = (
    "lukaszsobala/cross-platform-benchmark/.github/workflows/measure.yml")

# A token is minted for one upload and should be used at once. GitHub issues
# them with a short life of their own; this is a second bound, in case a hub
# meets an issuer that is more generous than expected.
MAX_TOKEN_AGE = 3600.0
CLOCK_SKEW = 300.0

# SHA-256 DigestInfo, the prefix PKCS#1 v1.5 puts before the hash.
SHA256_DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")


class AttestationError(Exception):
    """A presented attestation that does not hold up."""


def b64url(data: str | bytes) -> bytes:
    """Decode base64url without padding, as JOSE writes it."""
    if isinstance(data, str):
        data = data.encode("ascii")
    return base64.urlsafe_b64decode(data + b"=" * (-len(data) % 4))


def _int(data: bytes) -> int:
    return int.from_bytes(data, "big")


# ---------------------------------------------------------------------------
# RSA PKCS#1 v1.5 verification
# ---------------------------------------------------------------------------


def rsa_verify(message: bytes, signature: bytes, n: int, e: int) -> bool:
    """RS256: is `signature` this key's signature over `message`?

    Verification is public-key only -- exponentiate the signature and compare
    the result with the padding the standard requires. Everything here is a
    comparison against a value we constructed, so a forged signature has to
    produce the whole encoded block, not merely collide with part of it.
    """
    k = (n.bit_length() + 7) // 8
    if len(signature) != k or k < len(SHA256_DIGEST_INFO) + 43:
        return False
    m = pow(_int(signature), e, n)
    got = m.to_bytes(k, "big")
    digest = hashlib.sha256(message).digest()
    # 0x00 || 0x01 || 0xFF… || 0x00 || DigestInfo || H
    want = (b"\x00\x01"
            + b"\xff" * (k - len(SHA256_DIGEST_INFO) - len(digest) - 3)
            + b"\x00" + SHA256_DIGEST_INFO + digest)
    return secrets.compare_digest(got, want)


# ---------------------------------------------------------------------------
# The issuer's keys
# ---------------------------------------------------------------------------


class KeyStore:
    """GitHub's signing keys, fetched once and kept for a while.

    Fetched rather than pinned because GitHub rotates them; cached because an
    upload must not become an outbound request. A hub with no outbound network
    can be given the same document as a file, and then nothing here talks to
    anyone.
    """

    def __init__(self, url: str = JWKS_URL, path: str | None = None,
                 ttl: float = 6 * 3600.0, timeout: float = 10.0):
        self.url = url
        self.path = path
        self.ttl = ttl
        self.timeout = timeout
        self.lock = threading.Lock()
        self.keys: dict[str, tuple[int, int]] = {}
        self.fetched = 0.0

    def _parse(self, doc: object) -> dict[str, tuple[int, int]]:
        if not isinstance(doc, dict) or not isinstance(doc.get("keys"), list):
            raise AttestationError("malformed JWKS document")
        out = {}
        for key in doc["keys"]:
            if not isinstance(key, dict):
                continue
            # RSA signing keys only. An issuer offering something else is not
            # one this verifier understands, and guessing is not verification.
            if key.get("kty") != "RSA" or key.get("alg") not in (None, "RS256"):
                continue
            kid, n, e = key.get("kid"), key.get("n"), key.get("e")
            if not (isinstance(kid, str) and isinstance(n, str)
                    and isinstance(e, str)):
                continue
            out[kid] = (_int(b64url(n)), _int(b64url(e)))
        if not out:
            raise AttestationError("no usable RSA keys in the JWKS document")
        return out

    def load(self, force: bool = False) -> dict[str, tuple[int, int]]:
        with self.lock:
            fresh = time.monotonic() - self.fetched < self.ttl
            if self.keys and fresh and not force:
                return self.keys
            if self.path:
                doc = json.loads(Path(self.path).read_text())
            else:
                req = urllib.request.Request(
                    self.url, headers={"Accept": "application/json"})
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    doc = json.loads(resp.read(1 << 20).decode("utf-8"))
            self.keys = self._parse(doc)
            self.fetched = time.monotonic()
            return self.keys

    def key_for(self, kid: str) -> tuple[int, int]:
        keys = self.load()
        if kid not in keys:
            # An unknown kid is what a rotation looks like from here, so refetch
            # once before calling it a forgery.
            keys = self.load(force=True)
        if kid not in keys:
            raise AttestationError(f"unknown signing key {kid!r}")
        return keys[kid]


# ---------------------------------------------------------------------------
# Tokens
# ---------------------------------------------------------------------------


def verify_token(token: str, store: KeyStore) -> dict:
    """Check a JWS and return its claims. Signature first, always."""
    if not isinstance(token, str) or token.count(".") != 2 or len(token) > 8192:
        raise AttestationError("not a JWS compact serialization")
    head_b64, body_b64, sig_b64 = token.split(".")
    try:
        header = json.loads(b64url(head_b64))
        signature = b64url(sig_b64)
    except (ValueError, json.JSONDecodeError):
        raise AttestationError("malformed token")
    if not isinstance(header, dict):
        raise AttestationError("malformed token header")
    # `alg` is taken from the header only to refuse everything but the one we
    # implement. Nothing else about the header decides how it is checked --
    # that is the mistake behind every "alg: none" story.
    if header.get("alg") != "RS256":
        raise AttestationError(f"unsupported signing algorithm {header.get('alg')!r}")
    kid = header.get("kid")
    if not isinstance(kid, str):
        raise AttestationError("token names no signing key")

    n, e = store.key_for(kid)
    signed = f"{head_b64}.{body_b64}".encode("ascii")
    if not rsa_verify(signed, signature, n, e):
        raise AttestationError("signature does not verify")

    try:
        claims = json.loads(b64url(body_b64))
    except (ValueError, json.JSONDecodeError):
        raise AttestationError("malformed token claims")
    if not isinstance(claims, dict):
        raise AttestationError("token claims are not an object")
    return claims


def _audience(claims: dict) -> list[str]:
    aud = claims.get("aud")
    if isinstance(aud, str):
        return [aud]
    if isinstance(aud, list) and all(isinstance(a, str) for a in aud):
        return aud
    return []


def check(token: str, body: bytes, store: KeyStore,
          workflows: list[str], now: float | None = None) -> dict:
    """Verify an attestation over `body`, or raise AttestationError.

    Returns what the hub should record about it. Every check here can refuse;
    none of them can be satisfied by anything the submitter is able to write.
    """
    now = time.time() if now is None else now
    claims = verify_token(token, store)

    if claims.get("iss") != ISSUER:
        raise AttestationError(f"not issued by {ISSUER}")

    # The binding. The workflow asked GitHub to sign a token for this exact
    # document; a token minted for any other bytes names a different audience,
    # and one cannot be minted at all without GitHub's key.
    want = AUDIENCE_PREFIX + hashlib.sha256(body).hexdigest()
    if not any(secrets.compare_digest(a, want) for a in _audience(claims)):
        raise AttestationError(
            "this attestation was issued for a different document; it does not "
            "cover the result that was uploaded")

    exp, iat, nbf = claims.get("exp"), claims.get("iat"), claims.get("nbf")
    for name, value in (("exp", exp), ("iat", iat)):
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise AttestationError(f"token has no usable {name}")
    if now > float(exp) + CLOCK_SKEW:                   # type: ignore[arg-type]
        raise AttestationError("attestation has expired; measure and upload "
                               "in one go rather than keeping tokens")
    if (isinstance(nbf, (int, float)) and not isinstance(nbf, bool)
            and now + CLOCK_SKEW < float(nbf)):
        raise AttestationError("attestation is not valid yet")
    if now + CLOCK_SKEW < float(iat):                   # type: ignore[arg-type]
        raise AttestationError("attestation is dated in the future")
    if now - float(iat) > MAX_TOKEN_AGE:                # type: ignore[arg-type]
        raise AttestationError("attestation is too old to accept")

    # Which workflow produced it. `job_workflow_ref` is the workflow whose steps
    # are running, so a caller in someone else's repository still names ours --
    # and a fork that changes the steps names itself instead.
    ref = claims.get("job_workflow_ref")
    if not isinstance(ref, str) or "@" not in ref:
        raise AttestationError("token carries no job_workflow_ref")
    path = ref.split("@", 1)[0]
    if not any(path == w for w in workflows):
        raise AttestationError(
            f"produced by {path}, which this hub does not measure with")

    env = claims.get("runner_environment")
    if env not in ("github-hosted", "self-hosted"):
        raise AttestationError(f"unknown runner environment {env!r}")

    repo = claims.get("repository")
    run_id = claims.get("run_id")
    return {
        # The only distinction that matters, and the reason it is stored rather
        # than recomputed: `ci` has nothing of the submitter's in the chain,
        # `attested` has their machine in it.
        "tier": "ci" if env == "github-hosted" else "attested",
        "workflow_ref": ref[:300],
        "repository": repo[:200] if isinstance(repo, str) else None,
        "runner": env,
        "run_id": str(run_id)[:32] if run_id is not None else None,
        "run_url": (f"https://github.com/{repo}/actions/runs/{run_id}"
                    if isinstance(repo, str) and run_id is not None
                    and all(c.isalnum() or c in "-_./" for c in repo)
                    and str(run_id).isdigit() else None),
    }
