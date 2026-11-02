#!/usr/bin/env python3
"""E2E: external analysis engines through the shipped cortex.exe (step 8).

Drives the cortex_analysis_* MCP tools of `cortex.exe mcp` with a real
ILSpy and, when given, a real Ghidra:

  - ILSpy: list_types and decompile Cortex.Fixture.dll (also from inside its
    NuGet package); the decompiled C# project is rebuilt with `dotnet build`
    and the members of the rebuilt assembly are compared with the original's
    through the reference reader (tests/fixtures/dotnet/Oracle). Matching
    members show the decompiled code compiles to the same API, not that the
    method bodies are identical.
  - Ghidra: analyze the test target; the exported functions are Ghidra
    functions at the RVAs `cortex.exe inspect` reports, and the decompiled
    CortexDemoApplyDamage contains the health write; cancellation, the cache
    and the --artifact-root boundary.

    python tests/analysis_e2e.py --cortex stage/Cortex/cortex.exe \\
        --ilspycmd PATH [--dotnet PATH] [--ghidra-dir DIR --target EXE] [--runner wine]

Exit code 0 = pass.
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from artifact_mcp_e2e import Mcp, windows_path  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures", "dotnet")


def oracle(dll, dotnet):
    """Reference metadata of an assembly (System.Reflection.Metadata)."""
    run = subprocess.run([dotnet, "run", "-c", "Release", "--project", os.path.join(FIXTURES, "Oracle"), "--", dll],
                         capture_output=True, text=True, encoding="utf-8", timeout=600)
    if run.returncode != 0:
        raise RuntimeError("oracle failed: " + run.stderr[-2000:] + run.stdout[-2000:])
    return json.loads(run.stdout)


RESERVED = ("[module: RefSafetyRules(",)


def strip_reserved_attributes(project):
    """Drops attribute lines only the compiler may emit; returns them."""
    removed = []
    for folder, _, names in os.walk(project):
        for name in names:
            if not name.endswith(".cs"):
                continue
            path = os.path.join(folder, name)
            with open(path, encoding="utf-8") as handle:
                lines = handle.read().split("\n")
            kept = [line for line in lines if not line.strip().startswith(RESERVED)]
            if len(kept) != len(lines):
                removed += [os.path.relpath(path, project) + ": " + line.strip() for line in lines if line not in kept]
                with open(path, "w", encoding="utf-8", newline="") as handle:
                    handle.write("\n".join(kept))
    return removed


def members(document):
    """type -> (extends, fields, methods) by name and signature."""
    out = {}
    for row in document["types"]:
        out[row["name"]] = (row.get("extends"),
                            sorted((f["name"], f["signature"]) for f in row["fields"]),
                            sorted((m["name"], m["signature"]) for m in row["methods"]))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cortex", required=True)
    parser.add_argument("--ilspycmd", required=True)
    parser.add_argument("--dotnet", default="dotnet", help="dotnet that rebuilds the decompiled project (host side)")
    parser.add_argument("--engine-dotnet", default="", help="dotnet that cortex.exe uses to run an ilspycmd.dll")
    parser.add_argument("--ghidra-dir", default="")
    parser.add_argument("--target", default="")
    parser.add_argument("--runner", default="")
    arguments = parser.parse_args()
    runner = shlex.split(arguments.runner)
    cortex = os.path.abspath(arguments.cortex)
    bundle = os.path.dirname(cortex)
    failures = []

    def check(condition, message, detail=None):
        print(("PASS " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)
            if detail is not None:
                print("      " + json.dumps(detail)[:1500])

    root = tempfile.mkdtemp(prefix="cortex-analysis-e2e-")
    outside = tempfile.mkdtemp(prefix="cortex-analysis-outside-")
    try:
        dll = os.path.join(root, "Cortex.Fixture.dll")
        shutil.copyfile(os.path.join(FIXTURES, "Cortex.Fixture.dll"), dll)
        package = os.path.join(root, "Cortex.Fixture.1.4.2.nupkg")
        shutil.copyfile(os.path.join(FIXTURES, "Cortex.Fixture.1.4.2.nupkg"), package)
        shutil.copyfile(dll, os.path.join(outside, "Cortex.Fixture.dll"))
        target = ""
        if arguments.target:
            target = os.path.join(root, os.path.basename(arguments.target))
            shutil.copyfile(arguments.target, target)

        command = runner + [cortex, "mcp", "--tools", "all", "--artifact-root", windows_path(runner, root),
                            "--analysis-cache", windows_path(runner, os.path.join(root, "cache")),
                            "--ilspycmd", arguments.ilspycmd]
        if arguments.engine_dotnet:
            command += ["--dotnet", arguments.engine_dotnet]
        if arguments.ghidra_dir:
            command += ["--ghidra-dir", arguments.ghidra_dir]
        server = Mcp(command, bundle)
        server.initialize()

        failed, result = server.call("cortex_analysis_providers", {})
        providers = {row["name"]: row for row in result.get("providers", [])}
        check(not failed and providers.get("ilspy", {}).get("available") is True, "ilspy detected", result)
        check(all(row.get("linked") is False for row in providers.values()), "engines are external processes")
        if arguments.ghidra_dir:
            check(providers.get("ghidra", {}).get("available") is True, "ghidra detected", providers.get("ghidra"))

        def wait(job, seconds):
            deadline = time.time() + seconds
            while True:
                failed, state = server.call("cortex_analysis_get", {"job_id": job})
                if state.get("state") not in ("queued", "running") or time.time() > deadline:
                    return state
                time.sleep(1)

        # The --artifact-root boundary applies to engines too.
        failed, result = server.call("cortex_analysis_start", {"provider": "ilspy", "feature": "list_types",
                                                               "path": windows_path(runner, os.path.join(outside, "Cortex.Fixture.dll"))})
        check(failed and result.get("error") == "path_outside_artifact_roots", "a file outside --artifact-root is refused", result)
        failed, result = server.call("cortex_analysis_start", {"provider": "ilspy", "feature": "decompile",
                                                               "path": windows_path(runner, dll), "options": {"type": "-o x"}})
        check(failed and result.get("error") == "invalid_options", "engine switches cannot be smuggled in options", result)

        # ILSpy: types, from the DLL and from inside the package.
        expected = json.load(open(os.path.join(FIXTURES, "Cortex.Fixture-expected.json"), encoding="utf-8"))
        names = sorted(t["name"].replace("/", ".") for t in expected["types"])
        for label, args in (("dll", {"path": windows_path(runner, dll)}),
                            ("package entry", {"path": windows_path(runner, package), "child": "lib/net8.0/Cortex.Fixture.dll"})):
            failed, started = server.call("cortex_analysis_start", dict(args, provider="ilspy", feature="list_types"))
            check(not failed and started.get("ok"), "ilspy list_types starts (%s)" % label, started)
            state = wait(started.get("job_id", ""), 300)
            failed, page = server.call("cortex_analysis_get", {"job_id": started.get("job_id", ""), "part": "types", "limit": 500})
            listed = sorted(item["name"] for item in page.get("result", {}).get("items", []))
            check(state.get("state") == "completed" and listed == names, "ilspy types equal the reference reader's (%s)" % label,
                  {"state": state, "listed": listed, "expected": names})

        # ILSpy: the whole project; rebuilt and compared member by member.
        failed, started = server.call("cortex_analysis_start", {"provider": "ilspy", "feature": "decompile", "path": windows_path(runner, dll)})
        state = wait(started.get("job_id", ""), 600)
        check(state.get("state") == "completed", "ilspy decompiles the assembly", state)
        check(state.get("provenance", {}).get("sha256") == state.get("sha256") and "--disable-updatecheck" in state.get("provenance", {}).get("command", []),
              "provenance names the file hash and the exact engine command", state.get("provenance"))
        project = os.path.join(root, "decompiled")
        offset, files = 0, 0
        while offset is not None:
            failed, page = server.call("cortex_analysis_get", {"job_id": started["job_id"], "part": "files", "offset": offset,
                                                               "limit": 100, "max_chars": 1048576})
            result = page.get("result", {})
            for item in result.get("items", []):
                path = os.path.join(project, *item["path"].split("/"))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="") as handle:
                    handle.write(item["text"])
                files += 1
            offset = result.get("next_offset")
        check(files >= 5, "decompiled project written (%d files)" % files)
        # ILSpy writes [module: RefSafetyRules(11)], an attribute the C#
        # compiler reserves for itself (CS8335) and emits again on its own.
        # That one line is removed, and reported; nothing else is edited.
        removed = strip_reserved_attributes(project)
        print("      removed before rebuilding (compiler-reserved): %s" % (removed or "nothing"))
        csproj = [name for name in os.listdir(project) if name.endswith(".csproj")]
        build = subprocess.run([arguments.dotnet, "build", "-c", "Release", "-o", os.path.join(project, "out"),
                                os.path.join(project, csproj[0]) if csproj else project],
                               capture_output=True, text=True, encoding="utf-8", timeout=900)
        rebuilt = os.path.join(project, "out", "Cortex.Fixture.dll")
        check(build.returncode == 0 and os.path.exists(rebuilt), "the decompiled C# project builds", build.stdout[-3000:])
        if os.path.exists(rebuilt):
            original, again = members(expected), members(oracle(rebuilt, arguments.dotnet))
            missing = sorted(set(original) - set(again))
            different = sorted(name for name in set(original) & set(again) if original[name] != again[name])
            check(not missing and not different, "the rebuilt assembly has the original's types and member signatures",
                  {"missing": missing, "different": {name: [original[name], again[name]] for name in different[:3]}})

        failed, again = server.call("cortex_analysis_start", {"provider": "ilspy", "feature": "decompile", "path": windows_path(runner, dll)})
        check(again.get("cache_hit") is True and again.get("state") == "completed", "same file and options: from the cache", again)

        # Ghidra
        if arguments.ghidra_dir and target:
            inspect = subprocess.run(runner + [cortex, "inspect", windows_path(runner, target), "--max", "65536"], cwd=bundle,
                                     capture_output=True, text=True, encoding="utf-8", timeout=300)
            exports = {row["name"]: row["rva"]
                       for row in json.loads(inspect.stdout).get("data", {}).get("exports", {}).get("functions", [])
                       if row.get("name") and not row.get("forwarded_to")}
            failed, started = server.call("cortex_analysis_start", {"provider": "ghidra", "feature": "analyze",
                                                                   "path": windows_path(runner, target),
                                                                   "options": {"decompile_limit": 300}})
            check(not failed and started.get("state") == "running", "ghidra analysis starts", started)
            began = time.time()
            state = wait(started.get("job_id", ""), 3600)
            print("      ghidra took %.0f s" % (time.time() - began))
            check(state.get("state") == "completed", "ghidra analyses the test target", state)
            summary = state.get("result", {}).get("summary", {})
            check(summary.get("executable_sha256") == state.get("sha256"), "Ghidra imported the hashed bytes", summary)
            entries = set()
            offset = 0
            while offset is not None:
                failed, page = server.call("cortex_analysis_get", {"job_id": started["job_id"], "part": "functions",
                                                                   "offset": offset, "limit": 500, "max_chars": 256})
                result = page.get("result", {})
                entries.update(int(item["rva"], 16) for item in result.get("items", []))
                offset = result.get("next_offset")
            code_exports = {name: rva for name, rva in exports.items() if name.startswith("Cortex") and not name.startswith("CortexFake")}
            missing = [name for name, rva in code_exports.items() if int(str(rva), 16) not in entries]
            check(code_exports and not missing, "exported Cortex* functions are Ghidra functions at the same RVA",
                  {"missing": missing, "exports": code_exports})
            rva = code_exports.get("CortexDemoApplyDamage")
            failed, page = server.call("cortex_analysis_get", {"job_id": started["job_id"], "part": "decompiled", "rva": str(rva)})
            items = page.get("result", {}).get("items", [])
            c = items[0].get("c", "") if items else ""
            check("+ 4)" in c and "/ 3" in c, "decompiled CortexDemoApplyDamage shows the health write and the armor third", c)

            failed, slow = server.call("cortex_analysis_start", {"provider": "ghidra", "feature": "analyze",
                                                                "path": windows_path(runner, target), "options": {"decompile_limit": 3}})
            time.sleep(5)
            failed, cancel = server.call("cortex_analysis_cancel", {"job_id": slow.get("job_id", "")})
            state = wait(slow.get("job_id", ""), 120)
            check(state.get("state") == "cancelled", "a running Ghidra job is cancelled", state)
            failed, jobs = server.call("cortex_analysis_jobs", {})
            check(len(jobs.get("jobs", [])) >= 5, "jobs are listed", jobs)
        server.close()
    finally:
        shutil.rmtree(root, ignore_errors=True)
        shutil.rmtree(outside, ignore_errors=True)
    print("analysis E2E: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
