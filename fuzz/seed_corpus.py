#!/usr/bin/env python3
"""Write the seed corpus for the libFuzzer campaigns.

A seeded corpus matters: libFuzzer is far more effective starting from inputs
that already reach interesting code than from random bytes, and these seeds
double as a regression set (every one of them must keep passing).

Run: python3 fuzz/seed_corpus.py
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))

PARSER_SEEDS = [
    # Well-formed requests, the shapes the server must accept.
    b"GET / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /index.html HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\n\r\n",
    b"HEAD /a HTTP/1.1\r\nHost: x\r\n\r\n",
    b"OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1.0\r\n\r\n",
    b"POST /f HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello",
    b"GET /f HTTP/1.1\r\nHost: x\r\nRange: bytes=0-9\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nRange: bytes=-5\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nRange: bytes=5-\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nRange: bytes=99999999999999999999-\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nIf-None-Match: \"abc\"\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Wed, 01 Jan 2030 00:00:00 GMT\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nIf-Range: \"a\"\r\nRange: bytes=1-2\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
    b"GET /f HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    # Framing edge cases the parser must reject.
    b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0x10\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n",
    b"GET / HTTP/1.1\nHost: x\n\n",
    b"GET / HTTP/1.1\r\nHost : x\r\n\r\n",
    b"GET  / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\nX-E:\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\nX-N: caf\xc3\xa9\r\n\r\n",
    # Method and version handling.
    b"GET / HTTP/2.0\r\nHost: x\r\n\r\n",
    b"GET / HTTP/0.9\r\n\r\n",
    b"BREW / HTTP/1.1\r\nHost: x\r\n\r\n",
    b"TRACE / HTTP/1.1\r\nHost: x\r\n\r\n",
    # Malformed starts, to exercise the rejection paths.
    b"\r\n\r\n",
    b"GET",
    b"GET ",
    b"GET /",
    b"\x00\x01\x02",
    b"GET / HTTP/1.1\r\n",
    b"GET / HTTP/1.1\r\nHost:",
]

PATH_SEEDS = [
    # Legitimate targets.
    b"/",
    b"/index.html",
    b"/a/b/c.txt",
    b"/a/b/c.txt?v=1",
    b"/a/b/c.txt#frag",
    b"/hello%20world.txt",
    b"/sub/../file",
    b"/a//b///c",
    b"/.well-known/acme.txt",
    # Traversal and encoding attacks.
    b"/../../etc/passwd",
    b"/%2e%2e%2f%2e%2e%2fetc%2fpasswd",
    b"/..%2f..%2fetc",
    b"/%252e%252e/x",
    b"/%00",
    b"/%00.txt",
    b"/a%00b",
    b"/.env",
    b"/.git/config",
    # Malformed percent escapes.
    b"/%",
    b"/%2",
    b"/%zz",
    b"/%%41",
    # Absolute and scheme-like forms.
    b"http://example.com/a/b",
    b"//evil.com/x",
    b"///x",
    b"*",
    b"",
    # Long and unusual inputs.
    b"/" + b"a" * 300,
    b"/" + b"a/" * 200,
    b"/\xff\xfe",
    b"/a?b=c&d=e",
]


ESCAPE_SEEDS = [
    b"",
    b"plain.txt",
    b"<script>alert(1)</script>",
    b"\"quoted\"",
    b"'single'",
    b"a&b",
    b"a<b>c",
    b"100%",
    b"%2e%2e/",
    b"a\r\nb",
    b"a b",
    b"\xff\xfe",
    b"/a/b?c=d",
    b"<img src=x onerror=alert(1)>",
    b"&amp;",
    b"&#39;",
    b"\x00",
    b"a\x00b",
    b"../../etc/passwd",
    b"\x7f\x1b[31m",
]


def write_seeds(subdir, seeds):
    out = os.path.join(HERE, "corpus", subdir)
    os.makedirs(out, exist_ok=True)
    for i, data in enumerate(seeds):
        with open(os.path.join(out, f"seed_{i:03d}"), "wb") as f:
            f.write(data)
    print(f"{out}: {len(seeds)} seeds")


def main():
    write_seeds("parser", PARSER_SEEDS)
    write_seeds("path", PATH_SEEDS)
    write_seeds("escape", ESCAPE_SEEDS)


if __name__ == "__main__":
    main()
