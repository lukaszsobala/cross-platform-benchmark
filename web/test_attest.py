#!/usr/bin/env python3
"""Tests for attestation checking.

    python3 web/test_attest.py

Every test here is about refusing something. The positive cases exist to prove
the refusals are not vacuous; the point of the file is the list of things a
submitter can write that must not be accepted.

The signing key is generated in-process, small and deterministic. No key
material is committed -- a private key in a repository is a thing to explain
forever, even a worthless one -- and generating it exercises the same modular
arithmetic the verifier uses.
"""

import base64
import hashlib
import json
import random
import time
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import attest                                            # noqa: E402


def _prime(rng, bits):
    while True:
        n = rng.getrandbits(bits) | (1 << (bits - 1)) | 1
        if _probably_prime(n, rng):
            return n


def _probably_prime(n, rng, rounds=24):
    for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(rounds):
        a = rng.randrange(2, n - 1)
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def make_key(bits=1024, seed=20260816):
    """A throwaway RSA key as (n, e, d). Deterministic, so tests do not vary."""
    rng = random.Random(seed)
    e = 65537
    while True:
        p, q = _prime(rng, bits // 2), _prime(rng, bits // 2)
        if p == q:
            continue
        phi = (p - 1) * (q - 1)
        if phi % e == 0:
            continue
        return p * q, e, pow(e, -1, phi)


N, E, D = make_key()


def b64(data):
    if isinstance(data, str):
        data = data.encode()
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def sign(payload, *, kid="test-key", alg="RS256", key=None):
    """A JWS over `payload`, signed the way the issuer would."""
    n, d = key or (N, D)
    head = b64(json.dumps({"alg": alg, "kid": kid, "typ": "JWT"}))
    body = b64(json.dumps(payload))
    signed = f"{head}.{body}".encode()
    k = (n.bit_length() + 7) // 8
    h = hashlib.sha256(signed).digest()
    block = (b"\x00\x01"
             + b"\xff" * (k - len(attest.SHA256_DIGEST_INFO) - len(h) - 3)
             + b"\x00" + attest.SHA256_DIGEST_INFO + h)
    sig = pow(int.from_bytes(block, "big"), d, n).to_bytes(k, "big")
    return f"{head}.{body}.{b64(sig)}"


BODY = b'{"schema": "cpu-bench/1", "mode": "full"}'
WORKFLOW = attest.DEFAULT_WORKFLOW


def claims(**over):
    c = {
        "iss": attest.ISSUER,
        "aud": attest.AUDIENCE_PREFIX + hashlib.sha256(BODY).hexdigest(),
        "iat": time.time() - 10,
        "nbf": time.time() - 10,
        "exp": time.time() + 300,
        "job_workflow_ref": WORKFLOW + "@refs/tags/v1.0.0",
        "runner_environment": "github-hosted",
        "repository": "someone/their-hardware",
        "run_id": 42,
    }
    c.update(over)
    return c


class AttestTest(unittest.TestCase):
    def setUp(self):
        self.store = attest.KeyStore()
        self.store.keys = {"test-key": (N, E)}
        self.store.fetched = time.monotonic()

    def check(self, token=None, body=BODY, **over):
        return attest.check(token or sign(claims(**over)), body, self.store,
                            [WORKFLOW])

    def refuses(self, token=None, body=BODY, **over):
        with self.assertRaises(attest.AttestationError) as cm:
            self.check(token, body, **over)
        return str(cm.exception)

    # -- what a good one looks like ----------------------------------------
    def test_a_github_hosted_run_is_the_binding_tier(self):
        out = self.check()
        self.assertEqual(out["tier"], "ci")
        self.assertEqual(out["repository"], "someone/their-hardware")
        self.assertEqual(out["run_url"],
                         "https://github.com/someone/their-hardware/actions/runs/42")

    def test_a_self_hosted_run_is_a_weaker_tier(self):
        """Same signatures, submitter's machine. Never the same word."""
        self.assertEqual(self.check(runner_environment="self-hosted")["tier"],
                         "attested")

    # -- the binding to the document ---------------------------------------
    def test_a_token_does_not_cover_a_different_result(self):
        """The whole point: stapling a real token to edited numbers fails."""
        edited = BODY.replace(b'"full"', b'"full" ')
        msg = self.refuses(body=edited)
        self.assertIn("different document", msg)

    def test_a_token_for_another_audience_is_refused(self):
        self.assertIn("different document", self.refuses(aud="cpu-bench-result:sha256:" + "0" * 64))
        self.assertIn("different document", self.refuses(aud="some-other-service"))

    def test_audience_may_be_a_list(self):
        good = attest.AUDIENCE_PREFIX + hashlib.sha256(BODY).hexdigest()
        self.assertEqual(self.check(aud=["unrelated", good])["tier"], "ci")

    # -- the signature ------------------------------------------------------
    def test_a_corrupted_signature_is_refused(self):
        head, body, sig = sign(claims()).split(".")
        broken = sig[:-6] + ("AAAAAA" if not sig.endswith("AAAAAA") else "BBBBBB")
        self.assertIn("signature", self.refuses(token=f"{head}.{body}.{broken}"))

    def test_claims_cannot_be_swapped_under_a_valid_signature(self):
        """The signature covers the claims, so editing them breaks it."""
        head, _, sig = sign(claims()).split(".")
        forged = b64(json.dumps(claims(runner_environment="github-hosted",
                                       repository="not/mine")))
        self.assertIn("signature", self.refuses(token=f"{head}.{forged}.{sig}"))

    def test_alg_none_is_refused(self):
        head = b64(json.dumps({"alg": "none", "kid": "test-key"}))
        body = b64(json.dumps(claims()))
        self.assertIn("algorithm", self.refuses(token=f"{head}.{body}."))

    def test_another_key_is_refused(self):
        other_n, _, other_d = make_key(seed=999)
        self.assertIn("signature",
                      self.refuses(token=sign(claims(), key=(other_n, other_d))))

    def test_an_unknown_key_id_is_refused(self):
        # No network in the tests, so the refetch this triggers finds nothing
        # and the token stays unknown -- which is the required outcome.
        self.store.url = "http://127.0.0.1:1/nowhere"
        self.assertRaises(Exception, lambda: self.check(token=sign(claims(), kid="nope")))

    def test_a_symmetric_algorithm_cannot_be_smuggled_in(self):
        """HS256 signed with the public modulus, the classic confusion attack."""
        head = b64(json.dumps({"alg": "HS256", "kid": "test-key"}))
        body = b64(json.dumps(claims()))
        import hmac
        mac = hmac.new(str(N).encode(), f"{head}.{body}".encode(),
                       hashlib.sha256).digest()
        self.assertIn("algorithm", self.refuses(token=f"{head}.{body}.{b64(mac)}"))

    # -- who and when -------------------------------------------------------
    def test_another_issuer_is_refused(self):
        self.assertIn("issued by", self.refuses(iss="https://token.example.invalid"))

    def test_an_expired_token_is_refused(self):
        self.assertIn("expired", self.refuses(exp=time.time() - 3600,
                                              iat=time.time() - 7200))

    def test_a_future_dated_token_is_refused(self):
        self.assertIn("future", self.refuses(iat=time.time() + 9999,
                                             exp=time.time() + 99999))

    def test_a_stale_token_is_refused(self):
        old = time.time() - attest.MAX_TOKEN_AGE - 60
        self.assertIn("too old", self.refuses(iat=old, exp=time.time() + 300))

    # -- which workflow -----------------------------------------------------
    def test_a_forked_workflow_is_refused(self):
        """Editing the measuring steps changes this claim, which is the point."""
        msg = self.refuses(
            job_workflow_ref="attacker/fork/.github/workflows/measure.yml@refs/heads/main")
        self.assertIn("does not measure with", msg)

    def test_a_different_workflow_in_the_right_repo_is_refused(self):
        owner = WORKFLOW.rsplit("/", 1)[0]
        self.assertIn("does not measure with", self.refuses(
            job_workflow_ref=f"{owner}/anything-else.yml@refs/heads/main"))

    def test_the_caller_repository_is_not_restricted(self):
        """Anyone may run it on their own hardware; that is the feature."""
        self.assertEqual(self.check(repository="stranger/lab")["tier"], "ci")

    def test_a_missing_workflow_ref_is_refused(self):
        self.assertIn("job_workflow_ref", self.refuses(job_workflow_ref=None))
        self.assertIn("job_workflow_ref", self.refuses(job_workflow_ref="no-at-sign"))

    def test_an_unknown_runner_environment_is_refused(self):
        self.assertIn("runner environment",
                      self.refuses(runner_environment="my-laptop"))

    # -- shapes -------------------------------------------------------------
    def test_junk_is_refused_without_exploding(self):
        for junk in ("", "not.a.token", "a.b", "x" * 9000, "..", "a.b.c.d"):
            with self.subTest(junk[:20]):
                with self.assertRaises(attest.AttestationError):
                    attest.check(junk, BODY, self.store, [WORKFLOW])


class KeyStoreTest(unittest.TestCase):
    def test_jwks_is_parsed_from_a_file(self):
        import tempfile
        doc = {"keys": [{"kty": "RSA", "kid": "k", "alg": "RS256",
                         "n": b64(N.to_bytes((N.bit_length() + 7) // 8, "big")),
                         "e": b64((65537).to_bytes(3, "big"))}]}
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(doc, f)
            path = f.name
        store = attest.KeyStore(path=path)
        self.assertEqual(store.load()["k"], (N, E))

    def test_a_document_with_no_rsa_keys_is_refused(self):
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"keys": [{"kty": "EC", "kid": "k"}]}, f)
            path = f.name
        with self.assertRaises(attest.AttestationError):
            attest.KeyStore(path=path).load()


if __name__ == "__main__":
    unittest.main(verbosity=2)
