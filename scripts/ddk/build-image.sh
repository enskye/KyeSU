#!/usr/bin/env bash
# Build the DDK image with clang added, the one build.sh uses for the LKM.
# Re-run it after bumping DDK_RELEASE or LLVM_VERSION; otherwise never.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

KMI="${KMI:-android16-6.12}"
DDK_RELEASE="${DDK_RELEASE:-20260828}"
LLVM_VERSION="${LLVM_VERSION:-22}"
TAG="${KSU_IMAGE:-localhost/kyesu-ddk:$KMI}"

podman build \
  --build-arg "DDK_IMAGE=ghcr.io/ylarod/ddk-min:${KMI}-${DDK_RELEASE}" \
  --build-arg "LLVM_VERSION=$LLVM_VERSION" \
  -t "$TAG" -f Containerfile .

podman run --rm "$TAG" clang --version | head -1
echo "built $TAG"
