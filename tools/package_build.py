"""Package explicit game binaries/bundles only; never include discs, saves, settings or build inputs."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def licenses(directory):
    directory.mkdir(parents=True, exist_ok=True)
    for name, source in [("SDL", "third_party/SDL/LICENSE.txt"), ("ImGui", "third_party/imgui/LICENSE.txt")]:
        shutil.copyfile(ROOT / source, directory / (name + ".txt"))


def package(platform, build, output, revision):
    output.mkdir(parents=True, exist_ok=True)
    name = "UltimateUJL-" + platform
    with tempfile.TemporaryDirectory(prefix="ujl-package-") as temp:
        stage = Path(temp)
        if platform == "ios-arm64":
            app = stage / "Payload/UltimateUJL.app"
            shutil.copytree(build / "UltimateUJL.app", app)
            for p in app.rglob("*"):
                if p.suffix.lower() in {".bin", ".iso", ".cue", ".img", ".ccd", ".sub", ".chd", ".mcd", ".ujlreplay"} or p.name in {"ujl.ini", "Documents", "score_traces"}:
                    raise ValueError("User data/disc file must not be packaged: " + str(p.relative_to(app)))
            # Sideloading tools apply the user's own signature. Remove the linker's ad-hoc signature too.
            binary = app / "UltimateUJL"
            probe = subprocess.run(["codesign", "-d", str(binary)], capture_output=True)
            if probe.returncode == 0:
                subprocess.run(["codesign", "--remove-signature", str(binary)], check=True)
            shutil.rmtree(app / "_CodeSignature", ignore_errors=True)
            (app / "embedded.mobileprovision").unlink(missing_ok=True)
            licenses(app / "Licenses")
            result = output / (name + ".ipa")
            with zipfile.ZipFile(result, "w", zipfile.ZIP_DEFLATED) as z:
                for p in sorted(stage.rglob("*")):
                    if p.is_file():
                        z.write(p, p.relative_to(stage).as_posix())
        else:
            folder = stage / name; folder.mkdir()
            executable = "ujl.exe" if platform == "windows-x64" else "ujl"
            shutil.copy2(build / executable, folder / executable)
            (folder / executable).chmod(0o755)
            if platform != "windows-x64":
                launcher = folder / ("Play UJL.command" if platform.startswith("macos-") else "play-ujl.sh")
                launcher.write_text('#!/bin/sh\ncd -- "$(dirname -- "$0")"\nexec ./ujl "$@"\n', encoding="utf-8")
                launcher.chmod(0o755)
            licenses(folder / "Licenses")
            (folder / "README.txt").write_text(
                f"Ultimate Um Jammer Lammy\nBuild: {revision}\n\n"
                "Run ujl and select your own JP disc image in the setup wizard.\n"
                "The USA disc is optional and supplies English/censored assets. No disc assets are included.\n"
                "Keep saves, score_traces and ujl.ini when replacing an older executable.\n"
                "macOS builds are not notarized. Linux needs an OpenGL-capable desktop and audio drivers.\n",
                encoding="utf-8")
            if platform == "windows-x64":
                result = output / (name + ".zip")
                with zipfile.ZipFile(result, "w", zipfile.ZIP_DEFLATED) as z:
                    for p in sorted(folder.rglob("*")):
                        if p.is_file():
                            z.write(p, p.relative_to(stage).as_posix())
            else:
                result = output / (name + ".tar.gz")
                with tarfile.open(result, "w:gz") as tar:
                    def permissions(info):
                        executable = info.name.endswith(("/ujl", ".command", ".sh"))
                        info.mode = 0o755 if info.isdir() or executable else 0o644
                        return info
                    tar.add(folder, arcname=name, filter=permissions)
    if result.suffix in {".zip", ".ipa"}:
        with zipfile.ZipFile(result) as z:
            if z.testzip() is not None:
                raise ValueError("Packaged archive failed CRC verification")
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("platform", choices=["windows-x64", "linux-x64", "macos-arm64", "macos-x64", "ios-arm64"])
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--out", type=Path, default=Path("build/artifacts"))
    p.add_argument("--revision", default="local")
    args = p.parse_args()
    print(package(args.platform, args.build, args.out, args.revision))


if __name__ == "__main__":
    main()
