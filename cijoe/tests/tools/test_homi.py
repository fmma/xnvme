import re
from time import sleep, time

import pytest
import yaml

from ..conftest import (
    MprocServer,
    cijoe_config_get_all_devices,
    get_shm_id,
    xnvme_parametrize,
)

# Selection is '-k' against the test id, so the cases taking no device carry
# 'upcie' in their name to run beside their parametrized siblings.


def _require_upcie(cijoe):
    """
    Skip unless this build has the uPCIe backend.

    Inspection reads the state the uPCIe runtime keeps, so 'homi status' exists
    only where that backend is built, which is Linux and only when it was
    enabled. Asking the library what it has beats deciding from the platform:
    a Linux build configured with -Dbe_upcie=false is in the same position, and
    an errno cannot be compared portably.
    """

    err, state = cijoe.run("xnvme library-info")
    assert not err, "could not ask the library what backends it has"

    if "name: 'upcie'" not in state.output():
        pytest.skip(reason="Requires the uPCIe backend; this build does not have it")


def _status(cijoe, shm_id):
    """Run 'homi status' and return its exit code and parsed document"""

    err, state = cijoe.run(f"homi status --shm_id {shm_id}")
    output = state.output()
    lines = output.split("\n")

    # A debug build logs before the document starts, and the CLI appends its
    # own error line after it; neither is part of the document
    start = next((i for i, ln in enumerate(lines) if ln.startswith("shm_id:")), None)
    assert start is not None, f"status emitted no document; output was: {output!r}"

    body = [ln for ln in lines[start:] if not ln.startswith("# ERR")]
    doc = yaml.safe_load("\n".join(body))

    assert doc is not None, f"status document did not parse; output was: {output!r}"

    return err, doc


def test_upcie_status_refuses_when_absent(cijoe):
    """
    A build that cannot inspect says so, rather than reporting nothing found.

    This is the inverse of what every other testcase here skips on, and it is
    the case the other platforms actually run. 'nothing is running' and 'this
    build cannot tell you' are different answers, and a caller gating on the
    exit status has to be able to distinguish them.
    """

    err, state = cijoe.run("xnvme library-info")
    assert not err, "could not ask the library what backends it has"

    if "name: 'upcie'" in state.output():
        pytest.skip(
            reason="This build has uPCIe; the refusal is what a build without it does"
        )

    err, state = cijoe.run("homi status --shm_id 4242")

    assert err, "status exits zero on a build that cannot inspect"
    assert "requires the uPCIe backend" in state.output()

    # A refusal, not an empty document, which would read as 'no server'
    assert "shm_id:" not in state.output()


def test_upcie_status_without_server(cijoe):
    """An id no server claimed reports as absent, and says so in its exit code"""

    _require_upcie(cijoe)

    # Well above what the suite hands out, so it cannot collide
    shm_id = 4242

    err, doc = _status(cijoe, shm_id)

    assert err, "status exits zero with no server running"
    assert doc["shm_id"] == shm_id
    assert doc["server_running"] is False
    assert doc["ready"] is False
    assert doc["controllers"] == []


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_status_with_primary(cijoe, device, be_opts, cli_args):
    """A held runtime reports what it holds, and exits zero once it is ready"""

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads uPCIe's shared segment; other backends are opaque to it"
        )

    err, doc = _status(cijoe, shm_id)

    assert not err, "status exits non-zero while a ready server holds the runtime"
    assert doc["server_running"] is True
    assert doc["ready"] is True

    assert doc["attached"] >= 1

    assert doc["controllers"], "a running server reports no controllers"
    for ctrlr in doc["controllers"]:
        assert ctrlr["readable"] is True
        assert ctrlr["initialized"] is True

        # The admin queue is excluded by subtraction, so an unsigned wrap
        # would surface as a huge count rather than a negative one
        assert ctrlr["nsq_used"] < 65536, "queue count looks like an unsigned wrap"
        assert ctrlr["ncq_used"] == ctrlr["nsq_used"]

        if "nsq_total" in ctrlr:
            assert ctrlr["nsq_used"] <= ctrlr["nsq_total"]
            assert ctrlr["ncq_used"] <= ctrlr["ncq_total"]


def _ctrlr_of(doc, uri):
    """The entry for `uri` in a status document, or None"""

    for ctrlr in doc.get("controllers") or []:
        if ctrlr["uri"] == uri:
            return ctrlr
    return None


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_queue_count_tracks_a_secondary(cijoe, device, be_opts, cli_args):
    """
    The reported queue count follows what a client allocates.

    A stale or double-counted value still looks plausible, so what pins this is
    the count moving with `--nqueues` rather than merely being in range: a
    client asking for N takes exactly N, and gives them all back when it exits.

    N and no more, because this client submits asynchronously. The sync queue
    pair is allocated on first synchronous I/O and this never does any; admin
    carries its own PRP scratch, so asking the controller about itself at open
    no longer drags one in.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads uPCIe's shared segment; other backends are opaque to it"
        )

    uri = device["uri"]
    nqueues = 2

    _, doc = _status(cijoe, shm_id)
    before = _ctrlr_of(doc, uri)
    assert before, f"{uri} is not among the controllers the server holds"
    baseline = before["nsq_used"]

    # 'setsid' and the closed stdin for the same reason MprocServer.start uses
    # them: backgrounding alone leaves this holding the transport's channel when
    # the target is remote, and the client never runs at all
    cijoe.run(
        f"setsid xnvmeperf run {uri} --be {be_opts['be']} --shm_id {shm_id}"
        f" --iopattern randread --iosize 4096 --qdepth 8 --nqueues {nqueues}"
        f" --runtime 20 --cpulist 0 < /dev/null > /tmp/qcount.out 2>&1 &"
    )

    peak = baseline
    for _ in range(30):
        sleep(1)
        _, doc = _status(cijoe, shm_id)
        ctrlr = _ctrlr_of(doc, uri)
        if ctrlr and ctrlr["nsq_used"] > baseline:
            peak = ctrlr["nsq_used"]
            break
    else:
        cijoe.run("cat /tmp/qcount.out")
        pytest.fail("the client never showed up in the queue count")

    assert peak == baseline + nqueues, f"expected {baseline} + {nqueues}, got {peak}"
    assert ctrlr["ncq_used"] == ctrlr["nsq_used"]

    for _ in range(60):
        sleep(1)
        _, doc = _status(cijoe, shm_id)
        ctrlr = _ctrlr_of(doc, uri)
        if ctrlr and ctrlr["nsq_used"] == baseline:
            break
    else:
        still = ctrlr["nsq_used"] if ctrlr else "the controller is gone"
        pytest.fail(f"queues were not released; still {still}, expected {baseline}")


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_admin_costs_no_ioqueue(cijoe, device, be_opts, cli_args):
    """
    Asking the controller about itself takes no I/O queue.

    Admin is a single queue and it is shared, whereas an I/O queue is dedicated
    to the client holding it and is what the server has fewest of. A client
    that only issues admin commands should cost none.

    The count is sampled while such clients run, not after: a borrowed queue is
    handed back on exit and leaves nothing behind to find, so an after-the-fact
    reading passes whether or not one was taken.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads what the uPCIe runtime holds; other backends are opaque"
        )

    uri = device["uri"]

    _, doc = _status(cijoe, shm_id)
    before = _ctrlr_of(doc, uri)
    assert before, f"{uri} is not among the controllers the server holds"
    baseline = before["nsq_used"]

    # 'setsid' and the closed stdin for the same reason MprocServer.start uses
    # them, and a marker file rather than a wait, since the loop is backgrounded
    # on the target rather than here
    cijoe.run("rm -f /tmp/admin_loop.done")
    cijoe.run(
        "setsid sh -c 'for i in $(seq 60); do"
        f" xnvme info {uri} --be {be_opts['be']} --shm_id {shm_id} >/dev/null 2>&1;"
        " done; touch /tmp/admin_loop.done'"
        " < /dev/null > /tmp/admin_loop.out 2>&1 &"
    )

    peak = baseline
    for _ in range(90):
        _, doc = _status(cijoe, shm_id)
        ctrlr = _ctrlr_of(doc, uri)
        if ctrlr:
            peak = max(peak, ctrlr["nsq_used"])
        err, _ = cijoe.run("test -f /tmp/admin_loop.done")
        if not err:
            break
        sleep(0.2)
    else:
        cijoe.run("cat /tmp/admin_loop.out")
        pytest.fail("the admin clients never finished")

    assert peak == baseline, (
        f"an admin-only client took an I/O queue: {baseline} -> {peak}."
        " Admin carries its own PRP scratch and should borrow no queue"
    )


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_totals_come_from_the_controller(cijoe, device, be_opts, cli_args):
    """
    The totals are read per controller rather than assumed.

    A constant would satisfy every other assertion here, so this checks the
    reported ceiling against what the controller answers for itself.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads uPCIe's shared segment; other backends are opaque to it"
        )

    _, doc = _status(cijoe, shm_id)
    ctrlr = _ctrlr_of(doc, device["uri"])
    assert ctrlr, f"{device['uri']} is not among the controllers the server holds"

    if "nsq_total" not in ctrlr:
        pytest.skip(reason="the controller did not report its queue allocation")

    err, state = cijoe.run(f"xnvme feature-get {cli_args} --fid 0x7 --sel 0")
    assert not err, "could not ask the controller for Number of Queues"

    # NSQA and NCQA are zero-based; the totals are those plus one
    m = re.search(r"nsqa:\s*(\d+),\s*ncqa:\s*(\d+)", state.output())
    assert m, f"no nqueues feature in output: {state.output()[-400:]}"

    assert ctrlr["nsq_total"] == int(m.group(1)) + 1
    assert ctrlr["ncq_total"] == int(m.group(2)) + 1


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_status_does_not_disturb_election(cijoe, device, be_opts, cli_args):
    """
    Probing must not disturb the role-election lock.

    This hammers the probe against a lock that is already held, so what it
    guards is that probing neither steals nor breaks a held role. The sharper
    hazard, a probe taking the lock during the window where it is still free
    and demoting a concurrent elector, needs a probe racing a starting server
    and is not covered here.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads uPCIe's shared segment; other backends are opaque to it"
        )

    # Wrapped in 'sh -c' so it stays a simple command: a configuration that
    # sets 'cijoe.run.env' has the transport prefix every command with
    # assignments, and those are a syntax error ahead of a compound one
    err, _ = cijoe.run(
        "sh -c 'for i in $(seq 1 200); do homi status"
        f" --shm_id {shm_id} >/dev/null || exit 1; done'"
    )
    assert not err, "status stopped reporting the server while being polled"

    err, doc = _status(cijoe, shm_id)
    assert not err
    assert doc["server_running"] is True


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_primary_terminates_promptly(cijoe, device, be_opts, cli_args):
    """
    A server asked to stop does stop, and does so quickly.

    Closing a controller walks the queue-id bitmap to reap what is still
    allocated. A bound the loop counter cannot represent makes that walk
    endless, and the server then outlives its signal: every later testcase
    still passes, and the run instead stalls at session teardown, far from
    the change that caused it. Bounding it here names it.

    This leaves no server behind on purpose; the device fixture starts a
    fresh one for whichever testcase runs next.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads uPCIe's shared segment; other backends are opaque to it"
        )

    assert MprocServer.is_running(cijoe), "no server to stop"

    # Generous next to a teardown of about a second
    budget = 15

    start = time()
    MprocServer.stop(cijoe)
    elapsed = time() - start

    assert elapsed < budget, f"server took {elapsed:.1f}s to terminate"

    _, doc = _status(cijoe, shm_id)
    assert doc["server_running"] is False, "status still reports a live server"


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_runtime_is_reachable_by_its_name(cijoe, device, be_opts, cli_args):
    """
    A runtime is one socket, named for the identifier clients pass.

    That name is the whole rendezvous: a client derives it from --shm_id and
    finds the server or does not. A rename that misses it leaves clients
    unable to attach while everything else looks healthy.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(reason="Only uPCIe serves clients over a socket")

    err, _ = cijoe.run(f"test -S /tmp/xnvme-homi-{shm_id}.sock")
    assert not err, "a running server has no socket at the name clients use"


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_a_killed_primary_leaves_nothing_to_clean_up(cijoe, device, be_opts, cli_args):
    """
    A server that dies takes its rendezvous with it.

    The arrangement this replaced left a segment that outlived its creator and
    read exactly like a live runtime, so telling debris from a server was
    something status had to do. A socket cannot be left behind in that state:
    the process holding it is the only thing that answers on it.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(reason="Only uPCIe serves clients over a socket")

    assert MprocServer.is_running(cijoe), "no server to kill"

    # SIGKILL rather than SIGTERM: the point is a server that had no chance
    # to clean up after itself
    cijoe.run("pkill -9 -x homi")
    MprocServer.forget()

    for _ in range(30):
        sleep(1)
        if not MprocServer.is_running(cijoe):
            break
    else:
        pytest.fail("the server survived SIGKILL")

    err, doc = _status(cijoe, shm_id)

    assert err, "status exits zero with no server running"
    assert doc["server_running"] is False
    assert doc["ready"] is False
    assert doc["controllers"] == []


def _held_uris():
    """
    Every controller the server was started over

    Plain block devices only. The client used here reads 4 KiB at an
    arbitrary offset, which a zoned controller refuses outright and which does
    not divide a controller formatted with protection information in an
    extended LBA, where the block carries its metadata inline. The shapes being
    covered are about which controller a client reaches, not about what it
    does once it is there.

    Taken from the configuration rather than from what the server reports,
    because reporting is itself served over the socket: a server that cannot
    serve a controller does not list it either, and a test that enumerated
    from status would quietly shrink to the controllers that still work.
    """

    return [
        d["uri"]
        for d in cijoe_config_get_all_devices(["pcie"])
        if "nvm" in (d.get("labels") or [])
        and not any(lbl.startswith("pi") for lbl in (d.get("labels") or []))
    ]


def _used(cijoe, shm_id):
    """Queues in use per controller, as the server reports them"""

    _, doc = _status(cijoe, shm_id)

    return {c["uri"]: c["nsq_used"] for c in (doc.get("controllers") or [])}


def _attached(cijoe, shm_id):
    """
    Clients the server is serving

    The count that says control plane happened. Queues in use cannot: they are the
    controller's, so a client that bypassed the server and opened the
    controller itself still shows up in them.
    """

    _, doc = _status(cijoe, shm_id)

    return doc.get("attached", 0)


def _start_consumer(cijoe, uri, be, shm_id, seconds):
    """
    Start a client that holds a queue for a while

    'setsid' and the closed stdin for the same reason MprocServer.start uses
    them: backgrounding alone leaves this holding the transport's channel when
    the target is remote, and the client never runs at all.
    """

    tag = uri.replace(":", "-").replace(".", "-")

    return cijoe.run(
        f"setsid xnvmeperf run {uri} --be {be} --shm_id {shm_id}"
        f" --iopattern randread --iosize 4096 --qdepth 8 --nqueues 1"
        f" --runtime {seconds} --cpulist 0 < /dev/null > /tmp/perm-{tag}.out 2>&1 &"
    )


def _wait_until(cijoe, shm_id, predicate, timeout):
    """Poll the server's own account of itself until it says what is wanted"""

    used = {}
    for _ in range(timeout):
        used = _used(cijoe, shm_id)
        if predicate(used):
            return True, used
        sleep(1)

    return False, used


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_io_lands_on_the_controller_it_was_asked_for(cijoe, device, be_opts, cli_args):
    """
    A client holding several controllers submits each one's I/O on that one.

    An attached client builds every controller it opens from what the server
    published. Where that description comes from whichever controller the
    client happened to open first, every device's I/O is submitted on that one:
    the commands complete, the throughput looks plausible, and the rest of the
    controllers sit idle.

    So this asserts on where the queues appear rather than on whether the I/O
    succeeded. The server counts them per controller, and a client submitting
    to the wrong one cannot make that count look right.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(
            reason="status reads what the uPCIe runtime holds; others are opaque"
        )

    uris = _held_uris()
    if len(uris) < 2:
        pytest.skip(reason="Requires two controllers to tell one from the other")

    pair = uris[:2]

    cijoe.run(
        "setsid xnvmeperf run " + " ".join(pair) + f" --be {be_opts['be']}"
        f" --shm_id {shm_id} --iopattern randread --iosize 4096 --qdepth 8"
        " --nqueues 1 --runtime 30 --cpulist 0 < /dev/null > /tmp/spread.out 2>&1 &"
    )

    # Two failures look alike in the counts, so tell them apart: a client that
    # never reached the server leaves every controller at zero, which says
    # nothing about placement
    attached, used = _wait_until(
        cijoe, shm_id, lambda u: any(u.get(uri, 0) > 0 for uri in pair), 30
    )
    if not attached:
        cijoe.run("cat /tmp/spread.out")
    assert attached, f"the client never took a queue from the server; saw {used}"

    ok, used = _wait_until(
        cijoe, shm_id, lambda u: all(u.get(uri, 0) > 0 for uri in pair), 20
    )

    assert ok, (
        f"a client holding {pair} put queues on {used}. Every controller it"
        " holds should carry its own, rather than one carrying all of them"
    )


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_consumers_come_and_go_across_controllers(cijoe, device, be_opts, cli_args):
    """
    Clients reach the controller they asked for, and give it back

    An server holds several controllers and serves each of them, so the shapes
    worth covering are the ones that tell those apart: one controller at a
    time, the same one twice over, a subset at once, and every one at once.

    What is asserted is the server's own account of its queues, not whether the
    client exited zero. A client that cannot reach the server opens the
    controller directly and succeeds at its I/O, so exit status says nothing
    about whether control plane happened; a queue appearing on the controller
    that was asked for, and on no other, is what says it.

    This is the path that regressed: a server that served only its first
    controller left clients of the rest to open those directly, closing them
    out from under it.
    """

    _require_upcie(cijoe)

    shm_id = get_shm_id()
    if not shm_id:
        pytest.skip(reason="Requires a multi-process server; pass --shm_id")
    if not be_opts["be"].startswith("upcie"):
        pytest.skip(reason="Control plane over the socket is uPCIe's")

    be = be_opts["be"]
    uris = _held_uris()
    assert uris, "a running server reports no controllers"

    base = _used(cijoe, shm_id)

    shapes = [[uri] for uri in uris]
    if len(uris) > 2:
        shapes.append(uris[: len(uris) // 2])
    if len(uris) > 1:
        shapes.append(uris)

    for shape in shapes:
        wanted = set(shape)

        # Re-read rather than carry one baseline through: the previous shape's
        # clients are gone by here, but reusing a number taken before any of
        # them ran turns a slow departure into a failure of the next shape.
        attached_base = _attached(cijoe, shm_id)

        for uri in shape:
            _start_consumer(cijoe, uri, be, shm_id, 8)

        served = False
        for _ in range(30):
            if _attached(cijoe, shm_id) >= attached_base + len(shape):
                served = True
                break
            sleep(1)

        assert served, (
            f"the server never reported serving {len(shape)} more clients for"
            f" {sorted(wanted)}; it reports {_attached(cijoe, shm_id)} against a"
            f" baseline of {attached_base}. A client that cannot reach the"
            " server opens the controller itself and its I/O still succeeds, so"
            " this count is what says control plane happened"
        )

        used = _used(cijoe, shm_id)

        for uri in uris:
            if uri not in wanted:
                assert used.get(uri, 0) == base.get(uri, 0), (
                    f"{uri} gained a queue while only {sorted(wanted)} were asked"
                    " for; clients are reaching the wrong controller"
                )

        returned, used = _wait_until(
            cijoe,
            shm_id,
            lambda u: all(u.get(x, 0) == base.get(x, 0) for x in uris),
            60,
        )
        assert returned, (
            f"queues were not released after {sorted(wanted)} went away;"
            f" the server reports {used}, expected {base}"
        )
