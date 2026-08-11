"""Expose the host Apache Thrift compiler as @thrift_compiler//:thrift.

Full hermetic Thrift compiler builds need flex/bison + a large CMake graph;
M4/M5 keep the C++ *runtime* hermetic via @apache_thrift and use a host (or
CI-installed) thrift binary for codegen only. Prefer Thrift 0.24.x to match
@apache_thrift and the checked-in aster/rpc/gen-cpp stubs.
"""

def _host_thrift_repository_impl(rctx):
    thrift = rctx.which("thrift")
    if thrift != None:
        rctx.symlink(thrift, "thrift")
    else:
        # Defer failure to codegen actions so //aster/... stays buildable
        # without a Thrift compiler on PATH.
        rctx.file(
            "thrift",
            content = """#!/usr/bin/env bash
set -euo pipefail
echo "error: Apache Thrift compiler not found on PATH when @thrift_compiler was configured." >&2
echo "  Install 0.24.x (e.g. 'brew install thrift') or run scripts/ci-install-thrift.sh," >&2
echo "  then re-run Bazel (local repo picks up PATH changes automatically)." >&2
exit 1
""",
            executable = True,
        )

    rctx.file(
        "BUILD.bazel",
        content = """
package(default_visibility = ["//visibility:public"])

exports_files(["thrift"])
""",
    )

host_thrift_repository = repository_rule(
    implementation = _host_thrift_repository_impl,
    local = True,
    environ = ["PATH"],
    doc = "Wraps the host `thrift` binary for Aster client/server codegen genrules.",
)
