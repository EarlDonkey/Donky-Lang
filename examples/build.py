"""Build donky.exe with PyInstaller. Run from project root."""
import subprocess
import sys


def main() -> None:
    print("Running: pyinstaller donky.spec")
    result = subprocess.run(
        [sys.executable, "-m", "PyInstaller", "--clean", "donky.spec"]
    )
    if result.returncode != 0:
        print("Build failed", file=sys.stderr)
        sys.exit(result.returncode)
    print()
    print("Build succeeded.")
    print("Executable: dist/donky/donky.exe")
    print()
    print("To test:")
    print("  dist\\donky\\donky.exe examples\\fib.donk")
    print("  dist\\donky\\donky.exe examples\\robust_notes.donk notes.txt")


if __name__ == "__main__":
    main()