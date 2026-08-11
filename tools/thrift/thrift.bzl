"""Macros for generating Apache Thrift stubs from //aster/rpc:aster.thrift."""

def aster_thrift_stubs(
        name,
        thrift_gen,
        outs,
        idl = "//aster/rpc:thrift_idl",
        visibility = None,
        tags = None,
        **kwargs):
    """Run `thrift --gen <thrift_gen> -out <tmp>` and copy declared outputs.

    Args:
      name: genrule / filegroup name (consumers depend on this target).
      thrift_gen: value for `--gen` (e.g. `py`, `go:skip_remote`, `cpp:no_skeleton`).
      outs: map of bazel output path -> path relative to thrift `-out` directory.
      idl: label of the .thrift file (or single-file filegroup).
      visibility: target visibility.
      tags: extra tags (always includes `requires-thrift`).
    """
    if type(outs) != "dict" or not outs:
        fail("outs must be a non-empty dict of {bazel_out: thrift_relative_path}")

    out_paths = sorted(outs.keys())
    copy_lines = []
    for bazel_out in out_paths:
        src = outs[bazel_out]
        copy_lines.append(
            "mkdir -p \"$$(dirname \"$(location %s)\")\" && cp \"$$TMP/%s\" \"$(location %s)\"" % (
                bazel_out,
                src,
                bazel_out,
            ),
        )

    all_tags = ["requires-thrift"]
    if tags:
        all_tags = all_tags + tags

    native.genrule(
        name = name,
        srcs = [idl],
        outs = out_paths,
        tools = ["@thrift_compiler//:thrift"],
        cmd = """
set -euo pipefail
THRIFT="$(location @thrift_compiler//:thrift)"
IDL="$(location %s)"
TMP="$$(mktemp -d)"
trap 'rm -rf "$$TMP"' EXIT
"$$THRIFT" --gen %s -out "$$TMP" "$$IDL"
%s
""" % (idl, thrift_gen, "\n".join(copy_lines)),
        visibility = visibility,
        tags = all_tags,
        **kwargs
    )
