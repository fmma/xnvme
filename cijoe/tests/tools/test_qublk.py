"""
qublk exposes an xNVMe device as a Linux ublk block-device.

Unlike the other command-line tools, qublk is a blocking daemon; it requires root and
the 'ublk_drv' module, and it runs until signalled. It therefore cannot be exercised by
a single command returning a status. Instead, each test-case runs one shell session
which launches qublk in the background, waits for the block-device to appear, exercises
it, and tears it down with SIGINT, asserting that the device is gone afterwards.
"""

import pytest

from ..conftest import xnvme_parametrize

UBLK_NODE = "/dev/ublkb0"


def qublk_session(cijoe, uri, be, args, payload):
    """
    Run 'payload' against the ublk block-device served by qublk

    Returns the (err, state) of the shell session; the session fails when the
    block-device does not appear, when the payload fails, or when the device is still
    present after teardown.
    """

    # Joined by newline, not by ';': the background-launch ends in '&', which already
    # terminates the command, and a ';' following it is a syntax error
    script = "\n".join(
        [
            "set -u",
            "id -un",
            "modprobe ublk_drv || echo MODPROBE-FAILED",
            # A per-run log; a fixed path breaks as soon as one is left behind by
            # another user
            "log=$(mktemp)",
            f"qublk run {uri} --be {be} {args} > $log 2>&1 &",
            "pid=$!",
            f"for i in $(seq 1 50); do [ -b {UBLK_NODE} ] && break; sleep 0.2; done",
            f"if [ ! -b {UBLK_NODE} ]; then echo MISSING-DEVICE; "
            f"cat $log; kill -INT $pid 2>/dev/null; exit 1; fi",
            f"{payload}",
            "rc=$?",
            "kill -INT $pid 2>/dev/null",
            "wait $pid",
            f"if [ -b {UBLK_NODE} ]; then echo LEFTOVER-DEVICE; rc=1; fi",
            "cat $log",
            "rm -f $log",
            "exit $rc",
        ]
    )

    return cijoe.run(f"bash -c '{script}'")


@pytest.fixture(autouse=True)
def qublk_cleanup(cijoe):
    """Ensure a failed test-case does not leave a ublk device behind"""

    yield

    cijoe.run("pkill -INT qublk || true")
    cijoe.run(f"for i in $(seq 1 25); do [ -b {UBLK_NODE} ] || break; sleep 0.2; done")


@xnvme_parametrize(labels=["nvm"], opts=["be"])
def test_run_dd(cijoe, device, be_opts, cli_args):
    err, _ = qublk_session(
        cijoe,
        device["uri"],
        be_opts["be"],
        "--qdepth 64",
        f"dd if={UBLK_NODE} of=/dev/null bs=1M count=32 iflag=direct",
    )
    assert not err


@xnvme_parametrize(labels=["nvm"], opts=["be"])
def test_run_multi_queue(cijoe, device, be_opts, cli_args):
    err, _ = qublk_session(
        cijoe,
        device["uri"],
        be_opts["be"],
        "--qdepth 64 --nqueues 4",
        f"dd if={UBLK_NODE} of=/dev/null bs=1M count=32 iflag=direct",
    )
    assert not err


@xnvme_parametrize(labels=["nvm"], opts=["be"])
def test_run_max_io_bytes(cijoe, device, be_opts, cli_args):
    err, _ = qublk_session(
        cijoe,
        device["uri"],
        be_opts["be"],
        "--qdepth 64 --max-io-bytes 131072",
        f"dd if={UBLK_NODE} of=/dev/null bs=128k count=64 iflag=direct",
    )
    assert not err
