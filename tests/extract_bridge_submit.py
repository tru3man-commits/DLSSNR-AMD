"""Extract complete production definitions for the CPU fault-injection test.

Only the OS/GPU API and the surrounding Device declaration are test doubles.
Deliberately fail if a definition disappears instead of testing stale copies.
"""
import pathlib
import re
import sys

root, out = map(pathlib.Path, sys.argv[1:])
source = (root / "windows/src/pe/nr_pe_bridge.cpp").read_text()
header = (root / "windows/src/pe/nr_pe_bridge.hpp").read_text()


def definition(start, end="\n}"):
    begin = source.index(start)
    finish = source.index(end, begin) + len(end)
    return source[begin:finish] + "\n"


globals_ = re.findall(r"^uint64_t g_(?:begins|submits)\{\};$", source, re.M)
(out / "bridge_job.inc").write_text(
    definition("struct Job {", "\n};")
    + "\n".join(globals_)
    + "\n" + re.search(r"^constexpr uint64_t kStaleAfter = \d+;$", source, re.M)[0] + "\n"
)
# Take the actual member declarations, including per-device counters when present.
begin = header.index("    std::mutex jobs_mutex_;")
end = header.index("    // One shared fence", begin)
(out / "bridge_members.inc").write_text(header[begin:end])
(out / "bridge_methods.inc").write_text("\n".join(definition(start) for start in (
    "bool Device::submit_keyed(", "void Device::drop_locked(",
    "void Device::discard(", "void Device::cancel(", "bool Device::submit(",
)))
(out / "bridge_clock.inc").write_text(
    "uint64_t& clock() { return "
    + ("g_begins" if globals_ else "begins_") + "; }\n"
)
