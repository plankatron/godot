#!/usr/bin/env python3

if __name__ != "__main__":
    raise SystemExit(f'Utility script "{__file__}" should not be used as a module!')

import argparse
import hashlib
import os
import sys
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../"))

from misc.utility.color import Ansi, color_print, print_error

# NVIDIA DLSS SDK for Linux (NGX direct path).
#
# Nothing from the DLSS SDK is vendored in this repository. The SDK headers (~600 KB), the
# static link library, and the runtime feature libraries (~80 MB) are all fetched on demand
# from NVIDIA's public DLSS SDK repository, which is the authoritative host and is kept up to
# date by NVIDIA:
#   https://github.com/NVIDIA/DLSS
#
# The version is pinned to a tag for reproducibility, so a single change to `dlss_version`
# (plus refreshed checksums) bumps the whole SDK consistently — headers, static library and
# runtime libraries always match.
#
# Check for the latest version: https://github.com/NVIDIA/DLSS/tags
dlss_version = "310.5.3"

# SHA-256 checksums for the binary files at the pinned version. Headers are plain text pinned
# by the tag, so they are not individually checksummed. The `dev` runtime libraries (--dev)
# are different binaries, so their checksums are only enforced for the `rel` channel.
static_lib_sha256 = "7f56ea6504904b189cf77efe0aae47bfc3b08470cf75296f8cff572b701c3c6c"
runtime_lib_sha256 = {
    "libnvidia-ngx-dlss.so": "bb1db36542d5945eb1d47f0765cf37d4d0d8d5fa83d5048d5038021d66fb6d58",
    "libnvidia-ngx-dlssd.so": "a8da1a93b739e8cab586717e24e5ea70214e12ee53a114ce45820462fe3d6127",
}

# SDK headers (channel-independent build-time files). Update this list when bumping the SDK.
headers = [
    "nvsdk_ngx.h",
    "nvsdk_ngx_defs.h",
    "nvsdk_ngx_defs_dlssd.h",
    "nvsdk_ngx_defs_dlssg.h",
    "nvsdk_ngx_helpers.h",
    "nvsdk_ngx_helpers_dlssd.h",
    "nvsdk_ngx_helpers_dlssd_cuda.h",
    "nvsdk_ngx_helpers_dlssd_vk.h",
    "nvsdk_ngx_helpers_dlssg.h",
    "nvsdk_ngx_helpers_dlssg_vk.h",
    "nvsdk_ngx_helpers_vk.h",
    "nvsdk_ngx_params.h",
    "nvsdk_ngx_params_dlssd.h",
    "nvsdk_ngx_params_dlssg.h",
    "nvsdk_ngx_vk.h",
]

# Runtime feature libraries this fork uses (channel-dependent). DLSS-FG (libnvidia-ngx-dlssg.so)
# is also published upstream but is not used by this fork.
runtime_libs = {
    "libnvidia-ngx-dlss.so": "DLSS Super Resolution (DLSS-SR)",
    "libnvidia-ngx-dlssd.so": "DLSS Ray Reconstruction (DLSS-RR)",
}

parser = argparse.ArgumentParser(description="Install the NVIDIA DLSS SDK for Linux.")
parser.add_argument(
    "--dev",
    action="store_true",
    help="Download the development runtime libraries (with extra validation) instead of the release ones.",
)
args = parser.parse_args()

channel = "dev" if args.dev else "rel"

base_url = f"https://github.com/NVIDIA/DLSS/raw/v{dlss_version}"

# Install into the fork's vendored NGX directory, where SConstruct's CPPPATH/LIBPATH and the
# runtime LD_LIBRARY_PATH expect the SDK to live. Paths are resolved relative to this script
# so it works regardless of the current working directory.
ngx_root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../thirdparty/ngx"))
ngx_include_folder = os.path.join(ngx_root, "include")
ngx_lib_folder = os.path.join(ngx_root, "lib", "linux_x86_64")

for folder in (ngx_include_folder, ngx_lib_folder):
    if not os.path.exists(folder):
        os.makedirs(folder)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(repo_path, dest, expected_sha256):
    if os.path.isfile(dest):
        os.remove(dest)
    urllib.request.urlretrieve(f"{base_url}/{repo_path}", dest)
    if expected_sha256 is not None:
        actual = sha256(dest)
        if actual != expected_sha256:
            os.remove(dest)
            print_error(f"Checksum mismatch for {repo_path}:\n  expected {expected_sha256}\n  got      {actual}")
            sys.exit(1)


# SDK headers
color_print(f"{Ansi.BOLD}[1/3] NGX SDK headers")
for name in headers:
    print(f"Downloading include/{name} ...")
    download(f"include/{name}", os.path.join(ngx_include_folder, name), None)
print(f"{len(headers)} headers installed successfully.\n")

# Static link library
color_print(f"{Ansi.BOLD}[2/3] NGX static link library")
print("Downloading libnvsdk_ngx.a ...")
download("lib/Linux_x86_64/libnvsdk_ngx.a", os.path.join(ngx_lib_folder, "libnvsdk_ngx.a"), static_lib_sha256)
print("Verified SHA-256 of libnvsdk_ngx.a.")
print("Static link library installed successfully.\n")

# Runtime feature libraries
color_print(f"{Ansi.BOLD}[3/3] DLSS runtime libraries ({channel})")
for basename, description in runtime_libs.items():
    filename = f"{basename}.{dlss_version}"
    expected = runtime_lib_sha256.get(basename) if channel == "rel" else None
    print(f"Downloading {filename} ...")
    download(f"lib/Linux_x86_64/{channel}/{filename}", os.path.join(ngx_lib_folder, filename), expected)
    if expected is not None:
        print(f"Verified SHA-256 of {filename}. ({description})")
    else:
        print(f"Installed {filename} ({description}); {channel} channel is not checksum-pinned.")
print("Runtime libraries installed successfully.\n")

color_print(f'{Ansi.GREEN}The NVIDIA DLSS SDK {dlss_version} ({channel}) was installed to "{ngx_root}" successfully!')
color_print(f'{Ansi.GREEN}You can now build Godot with DLSS support enabled by running "scons use_ngx_dlss=yes".')
