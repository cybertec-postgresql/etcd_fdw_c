#!/usr/bin/env python3
"""A mock etcd JSON gateway that returns malformed / adversarial response bodies.

Every request gets HTTP 200 with a body chosen (and often mutated) from a corpus
of broken JSON. It is used to fuzz etcd_fdw's response parsing: the PostgreSQL
backend must error cleanly and never crash, whatever comes back.
"""
import http.server
import random

CORPUS = [
    b"",                                              # empty
    b"not json at all",                               # not JSON
    b"{",                                             # truncated object
    b"[",                                             # truncated array
    b'{"header":{"revision":"notanumber"}}',         # non-numeric counter
    b'{"kvs":123}',                                   # kvs not an array
    b'{"kvs":null}',                                  # kvs null
    b'{"kvs":[{}]}',                                  # kv with no fields
    b'{"kvs":[{"key":12345,"value":true}]}',         # wrong field types
    b'{"kvs":[{"key":"!!!not-base64!!!"}]}',          # invalid base64 key
    b'{"kvs":[{"value":"@@@@"}]}',                    # invalid base64, no key
    b'{"kvs":[{"key":null,"value":null}]}',           # null key/value
    b'{"count":"999999999999999999999999999"}',      # huge number-string
    b'{"more":"yes","kvs":[]}',                       # wrong type for bool
    b'{"succeeded":"maybe"}',                         # txn-ish junk
    b'{"ID":"abc","TTL":"xyz"}',                      # lease-ish junk
    b'\x00\x01\x02\x03 binary garbage',               # raw bytes
    b'{"kvs":[' + b'{"key":"YQ=="},' * 2000 + b'{"key":"YQ=="}]}',   # huge array
    b'{"a":' * 3000 + b'1' + b'}' * 3000,             # deeply nested
    b'{"kvs":[{"key":"' + b'QQ' * 50000 + b'=="}]}',  # very long base64 value
]


def mutate(b):
    r = random.random()
    if r < 0.25 and b:                 # truncate
        return b[: random.randint(0, len(b))]
    if r < 0.40 and b:                 # flip a byte
        i = random.randrange(len(b))
        return b[:i] + bytes([b[i] ^ 0x40]) + b[i + 1:]
    if r < 0.50:                       # append junk
        return b + bytes(random.randrange(256) for _ in range(random.randint(0, 16)))
    return b


class Handler(http.server.BaseHTTPRequestHandler):
    def _serve(self):
        try:
            n = int(self.headers.get("Content-Length", 0))
            if n:
                self.rfile.read(n)
        except Exception:
            pass
        body = mutate(random.choice(CORPUS))
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except Exception:
            pass

    do_POST = _serve
    do_GET = _serve

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    http.server.HTTPServer(("0.0.0.0", 2379), Handler).serve_forever()
