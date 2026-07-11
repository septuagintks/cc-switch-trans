import argparse
import pathlib
import subprocess
import sys
import time


ROOT = pathlib.Path(__file__).resolve().parents[2]
TMP = ROOT / "tmp"
INTEGRATION = ROOT / "tests" / "integration"
sys.path.insert(0, str(INTEGRATION))

import run_windows_system_proxy_integration as system_proxy


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--confirm-system-proxy-mutation", action="store_true")
    parser.add_argument("benchmark_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if sys.platform != "win32":
        raise RuntimeError("the direct-proxy benchmark wrapper only runs on Windows")
    if not args.confirm_system_proxy_mutation:
        raise RuntimeError(
            "refusing to modify the current-user system proxy without "
            "--confirm-system-proxy-mutation"
        )
    benchmark_args = args.benchmark_args
    if benchmark_args and benchmark_args[0] == "--":
        benchmark_args = benchmark_args[1:]
    if not benchmark_args:
        raise RuntimeError("benchmark arguments are required after --")

    TMP.mkdir(parents=True, exist_ok=True)
    nonce = int(time.time() * 1000)
    backup_path = TMP / f"benchmark-proxy-settings-backup-{nonce}.json"
    original_settings = system_proxy.read_proxy_settings()
    system_proxy.write_backup(backup_path, original_settings)
    restored = False
    try:
        system_proxy.set_system_direct()
        time.sleep(0.25)
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tests" / "benchmark" / "run_benchmark.py"),
                *benchmark_args,
            ],
            cwd=ROOT,
            check=True,
        )
    finally:
        restore_error = None
        try:
            system_proxy.restore_system_proxy(original_settings)
            restored = True
        except Exception as ex:
            restore_error = ex
        if restored:
            backup_path.unlink(missing_ok=True)
        else:
            print(
                f"proxy restore failed; backup remains at {backup_path}",
                file=sys.stderr,
            )
            raise RuntimeError(
                "failed to restore current-user system proxy"
            ) from restore_error


if __name__ == "__main__":
    main()
