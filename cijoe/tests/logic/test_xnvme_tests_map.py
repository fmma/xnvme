import pytest

from ..conftest import xnvme_parametrize


@xnvme_parametrize(labels=["dev"], opts=["be"])
def test_mem_map_unmap(cijoe, device, be_opts, cli_args):
    if be_opts["admin"] != "libvfn":
        pytest.skip(reason="Backend does not support memory-mapping")

    err, _ = cijoe.run(f"xnvme_tests_map mem_map_unmap {cli_args} --count 31")
    assert not err


@xnvme_parametrize(labels=["pcie"], opts=["be"])
def test_mptr_unregistered(cijoe, device, be_opts, cli_args):
    """
    Metadata memory the runtime has no record of is refused, not submitted.

    Only uPCIe translates through the registry, and only there is there an
    address to fail to find; the other backends hand the controller something
    usable whatever the caller allocated with.
    """

    if not be_opts["be"].startswith("upcie"):
        pytest.skip(reason="Only uPCIe translates addresses through the registry")

    err, _ = cijoe.run(f"xnvme_tests_map mptr_unregistered {cli_args}")
    assert not err
