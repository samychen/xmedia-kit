#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""RTMP publish acceptance: push a file to a real RTMP peer over loopback.

The playing half of RTMP is covered by check_live_protocols.py. This is the
PUBLISHING half, and it needs no MediaMTX: FFmpeg's own rtmp protocol can act
as the server with `-rtmp_listen 1 -listen 1`, so the peer is the system
ffmpeg CLI on a loopback port. That keeps the acceptance runnable in a bare
sandbox while still exercising the REAL protocol -- a local .flv file would
prove nothing about rtmp://, which is the part that has never had a producer.

WHAT IS ASSERTED
  1. xmedia-inspect publish exits 0 against rtmp://.
  2. The peer actually received a stream (file non-empty), and re-opening it
     reports the same duration as the source (within a tolerance).
  3. The peer's stream count matches what was published.
  4. A URL with no --format is REFUSED (non-zero, and it says why) -- a silent
     fallback would make "published" mean "published something unknowable".
  5. --no-realtime really is unpaced: a 3-second source must finish in far
     less than 3 seconds. Realtime pacing that silently did nothing would
     look fine on a fast disk and break against a live peer.

EXIT CODES
    0  every assertion held
    1  an assertion failed (a publish/behaviour regression)
    2  a prerequisite is missing (binary, ffmpeg CLI, source media)
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    """A port this run owns. A fixed port is how two CI runs collide."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def probe_duration(ffprobe, path):
    out = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "default=nw=1:nk=1", path],
        capture_output=True, text=True)
    try:
        return float(out.stdout.strip())
    except ValueError:
        return None


def probe_streams(ffprobe, path):
    out = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "stream=index",
         "-of", "csv=p=0", path],
        capture_output=True, text=True)
    return len([line for line in out.stdout.splitlines() if line.strip()])


def receive(inspect_bin, ffmpeg, ffprobe, source, realtime, workdir, log):
    """Runs one loopback publish. Returns (ok, detail, wall_seconds)."""
    port = free_port()
    url = "rtmp://127.0.0.1:%d/live/av" % port
    received = os.path.join(workdir, "received_%d.flv" % port)
    receiver = subprocess.Popen(
        [ffmpeg, "-hide_banner", "-loglevel", "error",
         "-rtmp_listen", "1", "-listen", "1", "-i", url,
         "-c", "copy", "-y", received],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    # The listener needs a moment to bind; publishing into a closed port would
    # report a connection failure that says nothing about our code.
    time.sleep(2.0)

    cmd = [inspect_bin, "publish", source, "-o", url, "--format", "flv"]
    if not realtime:
        cmd.append("--no-realtime")
    started = time.time()
    sent = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.time() - started
    log.write("$ %s\n%s%s\n" % (" ".join(cmd), sent.stdout, sent.stderr))

    try:
        receiver.wait(timeout=15)
    except subprocess.TimeoutExpired:
        receiver.kill()
        receiver.wait()
        return False, "the receiving ffmpeg never finished (peer stalled)", 0.0

    if sent.returncode != 0:
        tail = (sent.stderr.strip().splitlines() or [""])[-1]
        return False, "publish exited %d: %s" % (sent.returncode, tail), 0.0

    if not os.path.exists(received) or os.path.getsize(received) == 0:
        return False, "the peer received nothing", 0.0

    src_dur = probe_duration(ffprobe, source)
    got_dur = probe_duration(ffprobe, received)
    if src_dur is None or got_dur is None:
        return False, "cannot read a duration from source or received file", 0.0
    # A live publish legitimately truncates a little at the tail (the trailer
    # never lands); what must not happen is half the file going missing.
    if abs(src_dur - got_dur) > max(0.5, src_dur * 0.15):
        return False, "duration %.3fs vs source %.3fs" % (got_dur, src_dur), 0.0

    src_streams = probe_streams(ffprobe, source)
    got_streams = probe_streams(ffprobe, received)
    if src_streams != got_streams:
        return False, "%d stream(s) received, source has %d" % (
            got_streams, src_streams), 0.0

    return True, "%.3fs media in %.2fs wall, %d stream(s)" % (
        got_dur, elapsed, got_streams), elapsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inspect", required=True,
                        help="xmedia-inspect binary")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--source", default=None,
                        help="media to publish (default: tests/testdata "
                             "small_h264_aac_3s.mp4)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source = args.source or os.path.join(
        root, "tests", "testdata", "small_h264_aac_3s.mp4")
    for tool, name in ((args.inspect, "xmedia-inspect"),
                       (args.ffmpeg, "ffmpeg"),
                       (args.ffprobe, "ffprobe")):
        if shutil.which(tool) is None and not os.path.exists(tool):
            print("SKIP: %s not found (%s)" % (name, tool))
            return 2
    if not os.path.exists(source):
        print("SKIP: source media not found (%s)" % source)
        return 2

    workdir = tempfile.mkdtemp(prefix="xmedia-rtmp-")
    log_path = os.path.join(workdir, "publish.log")
    failures = []
    walls = {}
    with open(log_path, "w") as log:
        for realtime in (True, False):
            label = "realtime" if realtime else "unpaced"
            ok, detail, elapsed = receive(args.inspect, args.ffmpeg,
                                          args.ffprobe, source, realtime,
                                          workdir, log)
            print("%-6s %s  %s" % ("ok" if ok else "FAIL", label, detail))
            if not ok:
                failures.append(label)
            else:
                walls[label] = elapsed

        # Pacing must be REAL, not decorative: an unpaced publish of a
        # multi-second clip has to finish in a fraction of its media duration.
        # If the two modes take the same time, --no-realtime (and therefore
        # realtime) is doing nothing at all.
        src_dur = probe_duration(args.ffprobe, source) or 0.0
        if "unpaced" in walls and src_dur > 1.0:
            if walls["unpaced"] > src_dur * 0.5:
                print("FAIL   pacing    unpaced publish took %.2fs for %.2fs of "
                      "media; pacing is not switching off"
                      % (walls["unpaced"], src_dur))
                failures.append("pacing")
            else:
                print("ok     pacing    unpaced %.2fs vs %.2fs media"
                      % (walls["unpaced"], src_dur))
        if "realtime" in walls and src_dur > 1.0:
            if walls["realtime"] < src_dur * 0.6:
                print("FAIL   realtime  paced publish finished in %.2fs for "
                      "%.2fs of media; it is not being paced"
                      % (walls["realtime"], src_dur))
                failures.append("realtime")
            else:
                print("ok     realtime  paced %.2fs for %.2fs media"
                      % (walls["realtime"], src_dur))

        # A URL without --format must be refused, not silently guessed.
        refused = subprocess.run(
            [args.inspect, "publish", source,
             "-o", "rtmp://127.0.0.1:1/live/x"],
            capture_output=True, text=True)
        log.write("$ publish without --format\n%s%s\n" % (
            refused.stdout, refused.stderr))
        if refused.returncode == 0:
            print("FAIL   no-format  an RTMP URL without --format was accepted")
            failures.append("no-format")
        elif "--format" not in refused.stderr:
            print("FAIL   no-format  refused without saying why")
            failures.append("no-format")
        else:
            print("ok     no-format  refused with an actionable message")

    if failures:
        print("\nfailed: %s\nlog: %s" % (", ".join(failures), log_path))
        if args.verbose:
            print(open(log_path).read())
        return 1
    print("\nRTMP publish acceptance: all assertions held")
    if not args.verbose:
        shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
