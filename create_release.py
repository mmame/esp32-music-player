#!/usr/bin/env python3
"""
ESP32 Music Player - OTA Release Helper
Automates the release process for OTA firmware updates
"""

import os
import sys
import re
import json
import shutil
import subprocess
from datetime import datetime
from pathlib import Path

# Colors for terminal output
class Colors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

def print_header(text):
    print(f"\n{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{text}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}\n")

def print_step(step_num, text):
    print(f"{Colors.OKBLUE}{Colors.BOLD}[Step {step_num}]{Colors.ENDC} {text}")

def print_success(text):
    print(f"{Colors.OKGREEN}✓ {text}{Colors.ENDC}")

def print_warning(text):
    print(f"{Colors.WARNING}⚠ {text}{Colors.ENDC}")

def print_error(text):
    print(f"{Colors.FAIL}✗ {text}{Colors.ENDC}")

def get_current_version():
    """Read current version from ota_update.h"""
    ota_header = Path("main/ota_update.h")
    if not ota_header.exists():
        print_error("main/ota_update.h not found!")
        return None
    
    content = ota_header.read_text()
    match = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', content)
    if match:
        return match.group(1)
    return None

def update_version_in_header(new_version):
    """Update version in ota_update.h"""
    ota_header = Path("main/ota_update.h")
    content = ota_header.read_text()
    
    # Replace version
    updated_content = re.sub(
        r'#define\s+FIRMWARE_VERSION\s+"[^"]+"',
        f'#define FIRMWARE_VERSION "{new_version}"',
        content
    )
    
    ota_header.write_text(updated_content)
    print_success(f"Updated main/ota_update.h to version {new_version}")

def update_version_json(new_version, release_notes):
    """Update version.json file"""
    version_data = {
        "version": new_version,
        "release_date": datetime.now().strftime("%Y-%m-%d"),
        "release_notes": release_notes
    }
    
    version_file = Path("version.json")
    version_file.write_text(json.dumps(version_data, indent=2) + "\n")
    print_success(f"Updated version.json")

def validate_version(version):
    """Validate semantic version format"""
    pattern = r'^\d+\.\d+\.\d+$'
    return re.match(pattern, version) is not None

def get_user_input(prompt, default=None):
    """Get user input with optional default value"""
    if default:
        user_input = input(f"{prompt} [{default}]: ").strip()
        return user_input if user_input else default
    return input(f"{prompt}: ").strip()

def confirm(prompt):
    """Ask for yes/no confirmation"""
    while True:
        response = input(f"{prompt} (y/n): ").strip().lower()
        if response in ['y', 'yes']:
            return True
        elif response in ['n', 'no']:
            return False
        print_warning("Please answer 'y' or 'n'")

def check_build_directory():
    """Check if build directory exists"""
    build_dir = Path("build")
    if not build_dir.exists():
        print_warning("Build directory not found")
        return False
    
    firmware_bin = build_dir / "ESP32-8048S050C.bin"
    if not firmware_bin.exists():
        print_warning("Firmware binary (ESP32-8048S050C.bin) not found in build/")
        return False
    
    return True

def prepare_release_files(release_dir):
    """Copy firmware and version.json to release directory"""
    release_path = Path(release_dir)
    release_path.mkdir(exist_ok=True)
    
    # Copy firmware binary
    src_firmware = Path("build/ESP32-8048S050C.bin")
    dst_firmware = release_path / "firmware.bin"
    shutil.copy2(src_firmware, dst_firmware)
    print_success(f"Copied firmware to {dst_firmware}")
    
    # Copy version.json
    src_version = Path("version.json")
    dst_version = release_path / "version.json"
    shutil.copy2(src_version, dst_version)
    print_success(f"Copied version.json to {dst_version}")
    
    return dst_firmware, dst_version

def get_file_size(filepath):
    """Get human-readable file size"""
    size_bytes = Path(filepath).stat().st_size
    for unit in ['B', 'KB', 'MB']:
        if size_bytes < 1024.0:
            return f"{size_bytes:.1f} {unit}"
        size_bytes /= 1024.0
    return f"{size_bytes:.1f} GB"

def main():
    print_header("ESP32 Music Player - OTA Release Helper")
    
    # Check if we're in the right directory
    if not Path("main/ota_update.h").exists():
        print_error("Error: This script must be run from the project root directory")
        sys.exit(1)
    
    # Step 1: Check current version
    print_step(1, "Checking current version")
    current_version = get_current_version()
    if not current_version:
        print_error("Failed to read current version")
        sys.exit(1)
    print(f"   Current version: {Colors.BOLD}{current_version}{Colors.ENDC}")
    
    # Step 2: Get new version
    print_step(2, "Enter new version number")
    while True:
        new_version = get_user_input("New version (semantic: X.Y.Z)", current_version)
        if validate_version(new_version):
            if new_version == current_version:
                if not confirm(f"Version is the same as current ({current_version}). Continue anyway?"):
                    continue
            break
        print_error("Invalid version format! Use semantic versioning (e.g., 1.0.1)")
    
    print(f"   New version: {Colors.OKGREEN}{Colors.BOLD}{new_version}{Colors.ENDC}")
    
    # Step 3: Get release notes
    print_step(3, "Enter release notes")
    release_notes = get_user_input("Release notes", "Bug fixes and improvements")
    
    # Step 4: Update version in code (must be done BEFORE build)
    print_step(4, "Updating firmware version in code")
    if confirm(f"Update main/ota_update.h to version {new_version}?"):
        update_version_in_header(new_version)
        print_warning("Version updated - firmware MUST be rebuilt for this to take effect!")
    else:
        print_error("Skipped version update - binary will have OLD version!")
        if not confirm("Continue anyway?"):
            sys.exit(1)
    
    # Step 5: Build firmware (REQUIRED after version change)
    print_step(5, "Building firmware (REQUIRED - version needs to be compiled in)")
    if confirm("Build firmware with 'idf.py build'?"):
        print("   Building... (this may take a few minutes)")
        try:
            # On Windows, use PowerShell to run idf.py
            if sys.platform == "win32":
                # Try using the espressif commands extension
                result = subprocess.run(
                    ["powershell", "-Command", "idf.py", "build"],
                    capture_output=True,
                    text=True,
                    check=True,
                    shell=True
                )
            else:
                result = subprocess.run(
                    ["idf.py", "build"],
                    capture_output=True,
                    text=True,
                    check=True
                )
            print_success("Build completed successfully")
        except subprocess.CalledProcessError as e:
            print_error("Build failed!")
            print(e.stderr)
            if not confirm("Continue anyway?"):
                sys.exit(1)
        except FileNotFoundError:
            print_warning("'idf.py' not found in PATH")
            print_warning("Please build manually using ESP-IDF commands or VS Code")
            if not confirm("Continue anyway (assuming build already done)?"):
                sys.exit(1)
    else:
        print_warning("Skipped build - make sure you have built the firmware manually!")
    
    # Step 6: Check build directory
    print_step(6, "Verifying build files")
    if not check_build_directory():
        print_error("Build files not found!")
        if not confirm("Continue anyway?"):
            sys.exit(1)
    else:
        firmware_size = get_file_size("build/ESP32-8048S050C.bin")
        print_success(f"Found firmware binary ({firmware_size})")
    
    # Step 7: Update version.json (AFTER build is verified)
    print_step(7, "Updating version.json metadata")
    update_version_json(new_version, release_notes)
    
    # Step 8: Prepare release files
    print_step(7, "Preparing release files")
    release_dir = get_user_input("Release directory", "release")
    
    try:
        firmware_path, version_path = prepare_release_files(release_dir)
        print(f"\n   {Colors.OKGREEN}Release files ready in: {release_dir}/{Colors.ENDC}")
        print(f"   • firmware.bin ({get_file_size(firmware_path)})")
        print(f"   • version.json")
    except Exception as e:
        print_error(f"Failed to prepare release files: {e}")
        sys.exit(1)
    
    # Step 9: Git operations
    print_step(9, "Git operations")
    if confirm("Create git commit for version update?"):
        try:
            subprocess.run(["git", "add", "main/ota_update.h", "version.json"], check=True)
            subprocess.run(["git", "commit", "-m", f"Release version {new_version}"], check=True)
            print_success("Git commit created")
            
            if confirm("Create git tag?"):
                tag_name = f"v{new_version}"
                subprocess.run(["git", "tag", "-a", tag_name, "-m", f"Release {new_version}"], check=True)
                print_success(f"Created git tag: {tag_name}")
                
                if confirm("Push to GitHub?"):
                    subprocess.run(["git", "push"], check=True)
                    subprocess.run(["git", "push", "--tags"], check=True)
                    print_success("Pushed to GitHub")
        except subprocess.CalledProcessError as e:
            print_error(f"Git operation failed: {e}")
        except FileNotFoundError:
            print_error("Git not found")
    
    # Step 10: GitHub release instructions
    print_step(10, "Create GitHub Release")
    print(f"\n   {Colors.BOLD}Manual steps to complete on GitHub:{Colors.ENDC}")
    print(f"   1. Go to: {Colors.OKCYAN}https://github.com/mmame/esp32-music-player/releases{Colors.ENDC}")
    print(f"   2. Click 'Draft a new release'")
    print(f"   3. Choose tag: {Colors.OKGREEN}v{new_version}{Colors.ENDC}")
    print(f"   4. Release title: {Colors.OKGREEN}Release {new_version}{Colors.ENDC}")
    print(f"   5. Description: {Colors.OKGREEN}{release_notes}{Colors.ENDC}")
    print(f"   6. Upload these files:")
    print(f"      • {release_dir}/firmware.bin")
    print(f"      • {release_dir}/version.json")
    print(f"   7. Click 'Publish release'\n")
    
    # Summary
    print_header("Release Summary")
    print(f"Version:       {Colors.OKGREEN}{new_version}{Colors.ENDC}")
    print(f"Release Notes: {release_notes}")
    print(f"Release Files: {release_dir}/")
    print(f"\n{Colors.OKGREEN}✓ Release preparation complete!{Colors.ENDC}")
    print(f"\n{Colors.WARNING}Don't forget to upload files to GitHub release!{Colors.ENDC}\n")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n\n{Colors.WARNING}Release process cancelled by user{Colors.ENDC}")
        sys.exit(1)
    except Exception as e:
        print_error(f"Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
