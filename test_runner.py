#!/usr/bin/env python3
"""
Test runner for cl3d PBRT export validation.

Usage:
  python test_runner.py generate  - Generate PBRT reference images (stored in test/)
  python test_runner.py test      - Run cl3d and compare to reference images
"""

import argparse
import os
import sys
import subprocess
import shutil
from datetime import datetime
from pathlib import Path

import numpy as np
from PIL import Image
from skimage.metrics import structural_similarity as ssim

# Paths (relative to script location)
SCRIPT_DIR = Path(__file__).parent.resolve()
TEST_DIR = SCRIPT_DIR / "test"
TEMP_DIR = SCRIPT_DIR / "_temp"
BIN_DIR = SCRIPT_DIR / "bin" / "Debug"

CL3D_EXE = BIN_DIR / "cl3d.exe"
PBRT_EXE = Path("D:/git/pbrt-v4/build/Release/pbrt.exe")
IMGTOOL_EXE = Path("D:/git/pbrt-v4/build/Release/imgtool.exe")


def run_command(cmd, cwd=None, timeout=300):
    """Run a command and return success status."""
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Timeout"
    except Exception as e:
        return False, "", str(e)


def load_image_as_array(path):
    """Load an image and convert to numpy array for SSIM."""
    img = Image.open(path).convert('RGB')
    return np.array(img)


def calculate_ssim(img1_path, img2_path):
    """Calculate SSIM between two images."""
    img1 = load_image_as_array(img1_path)
    img2 = load_image_as_array(img2_path)

    # Resize if dimensions don't match
    if img1.shape != img2.shape:
        img2_pil = Image.open(img2_path).convert('RGB')
        img2_pil = img2_pil.resize((img1.shape[1], img1.shape[0]), Image.LANCZOS)
        img2 = np.array(img2_pil)

    score = ssim(img1, img2, channel_axis=2, data_range=255)
    return score


# =============================================================================
# Part 1: Generate PBRT reference images
# =============================================================================

def generate_reference(cfg_path, work_dir):
    """Generate PBRT reference image for a single config."""
    cfg_name = cfg_path.stem
    print(f"  Generating reference for: {cfg_name}")

    # Copy config to work dir
    cfg_copy = work_dir / cfg_path.name
    shutil.copy(cfg_path, cfg_copy)

    # 1. Generate PBRT scene
    print(f"    [1/3] Exporting PBRT scene...")
    success, _, err = run_command(
        [str(CL3D_EXE), "-generate-ref", str(cfg_copy)],
        cwd=work_dir
    )
    if not success:
        print(f"    ERROR: cl3d -generate-ref failed: {err}")
        return False

    pbrt_file = work_dir / f"{cfg_name}.pbrt"
    if not pbrt_file.exists():
        print(f"    ERROR: PBRT file not found: {pbrt_file}")
        return False

    # 2. Render with PBRT
    print(f"    [2/3] Rendering with PBRT...")
    pbrt_exr = work_dir / f"{cfg_name}_ref.exr"
    success, _, err = run_command(
        [str(PBRT_EXE), str(pbrt_file), "--outfile", str(pbrt_exr)],
        cwd=work_dir,
        timeout=600
    )
    if not success:
        print(f"    ERROR: PBRT render failed: {err}")
        return False

    if not pbrt_exr.exists():
        print(f"    ERROR: PBRT output not found: {pbrt_exr}")
        return False

    # 3. Convert EXR to PNG
    print(f"    [3/3] Converting to PNG...")
    pbrt_png = work_dir / f"{cfg_name}_ref.png"
    success, _, err = run_command(
        [str(IMGTOOL_EXE), "convert", "--outfile", str(pbrt_png), str(pbrt_exr)],
        cwd=work_dir
    )
    if not success:
        print(f"    ERROR: EXR conversion failed: {err}")
        return False

    # Copy reference PNG back to test directory
    dest_png = cfg_path.parent / f"{cfg_name}_ref.png"
    shutil.copy(pbrt_png, dest_png)
    print(f"    Reference saved: {dest_png}")

    # Optionally copy PBRT file too
    dest_pbrt = cfg_path.parent / f"{cfg_name}.pbrt"
    shutil.copy(pbrt_file, dest_pbrt)

    return True


def cmd_generate():
    """Generate PBRT reference images for all tests."""
    print("=" * 60)
    print("Generating PBRT Reference Images")
    print("=" * 60)

    # Check prerequisites
    if not CL3D_EXE.exists():
        print(f"ERROR: cl3d.exe not found at {CL3D_EXE}")
        return 1
    if not PBRT_EXE.exists():
        print(f"ERROR: pbrt.exe not found at {PBRT_EXE}")
        return 1
    if not TEST_DIR.exists():
        print(f"ERROR: Test directory not found at {TEST_DIR}")
        return 1

    # Find all .cfg files
    cfg_files = list(TEST_DIR.glob("*.cfg"))
    if not cfg_files:
        print(f"No .cfg files found in {TEST_DIR}")
        return 1

    print(f"Found {len(cfg_files)} config(s)")

    # Create temp work directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    work_dir = TEMP_DIR / f"gen_{timestamp}"
    work_dir.mkdir(parents=True, exist_ok=True)
    print(f"Work directory: {work_dir}")
    print()

    # Generate references
    results = {}
    for cfg_path in sorted(cfg_files):
        success = generate_reference(cfg_path, work_dir)
        results[cfg_path.stem] = success
        print()

    # Summary
    print("=" * 60)
    print("GENERATION SUMMARY")
    print("=" * 60)

    succeeded = sum(1 for s in results.values() if s)
    failed = len(results) - succeeded

    for name, success in sorted(results.items()):
        status = "OK" if success else "FAILED"
        print(f"  {name}: {status}")

    print()
    print(f"Generated: {succeeded}/{len(results)}")

    if failed > 0:
        print(f"Failed: {failed}")
        return 1

    return 0


# =============================================================================
# Part 2: Run cl3d and compare to reference images
# =============================================================================

def run_test(cfg_path, output_dir):
    """Run cl3d and compare to reference image."""
    cfg_name = cfg_path.stem
    print(f"  Testing: {cfg_name}")

    # Check for reference image
    ref_png = cfg_path.parent / f"{cfg_name}_ref.png"
    if not ref_png.exists():
        print(f"    ERROR: Reference image not found: {ref_png}")
        print(f"    Run 'python test_runner.py generate' first")
        return None

    # Copy config to output dir
    cfg_copy = output_dir / cfg_path.name
    shutil.copy(cfg_path, cfg_copy)

    # 1. Run cl3d -test to generate screenshot
    print(f"    [1/2] Rendering with cl3d...")
    success, _, err = run_command(
        [str(CL3D_EXE), "-test", str(cfg_copy)],
        cwd=output_dir
    )
    if not success:
        print(f"    ERROR: cl3d -test failed: {err}")
        return None

    # The output is {cfg_name}_test_out.tga
    cl3d_tga = output_dir / f"{cfg_name}_test_out.tga"
    if not cl3d_tga.exists():
        print(f"    ERROR: cl3d output not found: {cl3d_tga}")
        return None

    # Convert TGA to PNG
    cl3d_png = output_dir / f"{cfg_name}_cl3d.png"
    img = Image.open(cl3d_tga)
    img.save(cl3d_png)

    # 2. Calculate SSIM against reference
    print(f"    [2/2] Comparing to reference...")
    try:
        score = calculate_ssim(cl3d_png, ref_png)
        print(f"    SSIM: {score:.4f}")

        # Copy reference to output for easy comparison
        shutil.copy(ref_png, output_dir / f"{cfg_name}_ref.png")

        return score
    except Exception as e:
        print(f"    ERROR: SSIM calculation failed: {e}")
        return None


def cmd_test():
    """Run cl3d tests and compare to reference images."""
    print("=" * 60)
    print("cl3d Test Runner")
    print("=" * 60)

    # Check prerequisites
    if not CL3D_EXE.exists():
        print(f"ERROR: cl3d.exe not found at {CL3D_EXE}")
        return 1
    if not TEST_DIR.exists():
        print(f"ERROR: Test directory not found at {TEST_DIR}")
        return 1

    # Find all .cfg files
    cfg_files = list(TEST_DIR.glob("*.cfg"))
    if not cfg_files:
        print(f"No .cfg files found in {TEST_DIR}")
        return 1

    print(f"Found {len(cfg_files)} test(s)")

    # Create output directory with timestamp
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = TEMP_DIR / f"test_{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {output_dir}")
    print()

    # Run tests
    results = {}
    for cfg_path in sorted(cfg_files):
        score = run_test(cfg_path, output_dir)
        results[cfg_path.stem] = score
        print()

    # Summary
    print("=" * 60)
    print("RESULTS SUMMARY")
    print("=" * 60)
    print(f"{'Test':<30} {'SSIM':>10}")
    print("-" * 42)

    for name, score in sorted(results.items()):
        if score is not None:
            print(f"{name:<30} {score:>10.4f}")
        else:
            print(f"{name:<30} {'FAILED':>10}")

    # Overall stats
    valid_scores = [s for s in results.values() if s is not None]
    if valid_scores:
        avg_ssim = sum(valid_scores) / len(valid_scores)
        print("-" * 42)
        print(f"{'Average':<30} {avg_ssim:>10.4f}")
        print(f"{'Tests passed':<30} {len(valid_scores):>10}/{len(results)}")

    print()
    print(f"Results saved to: {output_dir}")

    return 0 if len(valid_scores) == len(results) else 1


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="cl3d PBRT export test runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Commands:
  generate    Generate PBRT reference images (stored in test/)
  test        Run cl3d and compare to reference images

Examples:
  python test_runner.py generate   # Generate reference images first
  python test_runner.py test       # Then run tests against them
"""
    )
    parser.add_argument(
        "command",
        choices=["generate", "test"],
        help="Command to run"
    )

    args = parser.parse_args()

    if args.command == "generate":
        return cmd_generate()
    elif args.command == "test":
        return cmd_test()
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())
