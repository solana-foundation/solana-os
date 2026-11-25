#!/usr/bin/env python3
"""Host test for game/gossip_genome.c (Gossip's genome, addresses, records, the
HELLO's g and tag): builds it with genome_test.c and core/ed25519.c and runs the
docs/GOSSIP.md §3 and §4 vectors.

    python3 main/lua/tests/gossip/genome_test.py      (from firmware/)
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.normpath(os.path.join(HERE, "..", "..", ".."))

with tempfile.TemporaryDirectory() as tmp:
    # gossip.h includes cJSON.h for the role seam; the genome needs none of it.
    open(os.path.join(tmp, "cJSON.h"), "w").write("")
    exe = os.path.join(tmp, "genome_test")
    cc = os.environ.get("CC", "cc")
    subprocess.check_call([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                           "-I", tmp, "-I", MAIN, "-I", os.path.join(MAIN, "game"),
                           os.path.join(HERE, "genome_test.c"), os.path.join(MAIN, "game", "gossip_genome.c"),
                           os.path.join(MAIN, "core", "ed25519.c"), "-o", exe])
    sys.exit(subprocess.call([exe]))
