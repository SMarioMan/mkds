#!/usr/bin/env bash
# Reproducible Podman build & validation entrypoint for MKDS Simple-Mode 32-Track Mod.
#
# Usage:
#   ./scripts/build_simple32_podman.sh build-image   # Build localhost/mkds-simple32:latest from Containerfile
#   ./scripts/build_simple32_podman.sh build         # Build dlp_harness, C unit tests, D-archives, and patched Host ROM
#   ./scripts/build_simple32_podman.sh audit         # Run static 32-course archive & Guest heap audit
#   ./scripts/build_simple32_podman.sh validate      # Run full 32-track Download Play matrix + regression suite
#   ./scripts/build_simple32_podman.sh all           # Build image (if missing) + build + audit + validate
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE_ROOT="$(cd "${REPO_ROOT}/.." && pwd)"
IMAGE_TAG="${MKDS_SIMPLE32_IMAGE:-localhost/mkds-simple32:latest}"
CMD="${1:-build}"

ensure_image() {
    if ! podman image exists "${IMAGE_TAG}"; then
        echo "[build_simple32_podman] Building Podman image ${IMAGE_TAG} from Containerfile..."
        podman build -t "${IMAGE_TAG}" -f "${REPO_ROOT}/Containerfile" "${REPO_ROOT}"
    fi
}

run_in_podman() {
    podman run --rm \
        -v "${WORKSPACE_ROOT}:/work:Z" \
        -w /work/mkds \
        "${IMAGE_TAG}" \
        "$@"
}

case "${CMD}" in
    build-image)
        podman build -t "${IMAGE_TAG}" -f "${REPO_ROOT}/Containerfile" "${REPO_ROOT}"
        ;;
    build)
        ensure_image
        run_in_podman bash scripts/container_build_all.sh
        ;;
    audit)
        ensure_image
        run_in_podman python3 scripts/simple32_course_audit.py \
            --csv-out reports/simple32_static_audit.csv \
            --json-out reports/simple32_static_audit.json
        ;;
    validate)
        ensure_image
        shift
        run_in_podman bash scripts/container_validate.sh "$@"
        ;;
    all)
        ensure_image
        run_in_podman bash scripts/container_build_all.sh
        run_in_podman python3 scripts/simple32_course_audit.py \
            --csv-out reports/simple32_static_audit.csv \
            --json-out reports/simple32_static_audit.json
        run_in_podman python3 scripts/run_simple32_validation.py
        ;;
    *)
        echo "Usage: $0 {build-image|build|audit|validate|all}" >&2
        exit 2
        ;;
esac
