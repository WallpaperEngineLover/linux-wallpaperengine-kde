#!/usr/bin/env bash
# Distribution userlands without docker or root: pulls an image's layers from a registry and runs commands in it
# under proot. Used to build the portable release on an old glibc and to smoke test tarballs locally.
#
# Usage:
#   tools/distro_rootfs.sh fetch <image[:tag]> <dir>     e.g. debian:12, fedora:44, opensuse/tumbleweed, ghcr.io/...
#   tools/distro_rootfs.sh run <dir> [-u] <cmd...>       -u runs as the calling user instead of fake root
#
# PROOT points at the proot binary (default: proot from PATH, a static build works best), BINDS adds "-b src:dst" pairs.
# Whiteout files of later layers are applied, other OCI features (hardlinks across layers etc.) are not.
set -euo pipefail

PROOT="${PROOT:-proot}"

# whether no component of $2 below $1 is a symlink
plain_path() {
    local root="$1" path="$2"
    while [ "$path" != "$root" ] && [ "$path" != . ] && [ "$path" != / ]; do
        [ -L "$path" ] && return 1
        path="$(dirname "$path")"
    done
    return 0
}

fetch() {
    local image="$1" dir="$2" repo tag token manifest digest registry=registry-1.docker.io
    repo="${image%%:*}"
    tag="${image#*:}"
    [ "$tag" = "$image" ] && tag=latest
    if [[ "$repo" == ghcr.io/* ]]; then
        registry=ghcr.io
        repo="${repo#ghcr.io/}"
        token=$(curl -fsSL "https://ghcr.io/token?scope=repository:$repo:pull" | jq -r .token)
    else
        [[ "$repo" == */* ]] || repo="library/$repo"
        token=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$repo:pull" |
            jq -r .token)
    fi
    local accept="application/vnd.oci.image.index.v1+json,application/vnd.docker.distribution.manifest.list.v2+json"
    accept+=",application/vnd.oci.image.manifest.v1+json,application/vnd.docker.distribution.manifest.v2+json"
    manifest=$(curl -fsSL -H "Authorization: Bearer $token" -H "Accept: $accept" \
        "https://$registry/v2/$repo/manifests/$tag")
    if jq -e '.manifests' <<<"$manifest" >/dev/null; then
        digest=$(jq -r '[.manifests[] | select(.platform.architecture == "amd64" and .platform.os == "linux")][0].digest' <<<"$manifest")
        manifest=$(curl -fsSL -H "Authorization: Bearer $token" -H "Accept: $accept" \
            "https://$registry/v2/$repo/manifests/$digest")
    fi

    mkdir -p "$dir"
    local layer
    for layer in $(jq -r '.layers[].digest' <<<"$manifest"); do
        echo "layer $layer" >&2
        curl -fsSL -H "Authorization: Bearer $token" "https://$registry/v2/$repo/blobs/$layer" -o "$dir/.layer"
        if [[ "$layer" == sha256:* ]] && [ "$(sha256sum "$dir/.layer" | cut -d' ' -f1)" != "${layer#sha256:}" ]; then
            echo "layer $layer: digest mismatch, the download is corrupt" >&2
            exit 1
        fi
        # whiteouts: .wh.name deletes name from the lower layers, .wh..wh..opq empties the directory
        local wh
        while IFS= read -r wh; do
            wh="${wh#./}"
            case "/$wh/" in */../*) continue ;; esac
            local base="${wh##*/}" parent target="$dir"
            parent="$(dirname "$wh")"
            if [ "$parent" != . ]; then target="$dir/$parent"; fi
            # a symlink (var/run -> /run) would point rm at the host's files
            if ! plain_path "$dir" "$target"; then
                echo "skipping whiteout $wh, its directory goes through a symlink" >&2
                continue
            fi
            if [ "$base" = ".wh..wh..opq" ]; then
                find "$target" -mindepth 1 ! -path "$dir/.layer" -delete 2>/dev/null || true
            else
                rm -rf "${target:?}/${base#.wh.}"
            fi
        done < <(tar -tf "$dir/.layer" 2>/dev/null | grep -E '(^|/)\.wh\.' || true)
        # mknod fails without root, hide that noise
        tar -xf "$dir/.layer" -C "$dir" --exclude='*.wh.*' --no-same-owner 2>&1 |
            { grep -v -e 'Cannot mknod' -e 'Exiting with failure status' || true; } >&2 || true
        rm -f "$dir/.layer"
    done
    chmod -R u+w "$dir"
    cp /etc/resolv.conf "$dir/etc/resolv.conf" 2>/dev/null || true
    echo "$image" >"$dir/.image"
}

run() {
    local dir="$1" user=(-0) home=/root
    shift
    if [ "${1:-}" = -u ]; then
        user=(-i "$(id -u):$(id -g)")
        home=/tmp
        shift
    fi
    # shellcheck disable=SC2086
    exec "$PROOT" "${user[@]}" -r "$dir" -b /dev -b /proc -b /sys -b /etc/resolv.conf ${BINDS:-} -w / \
        /usr/bin/env -i HOME="$home" PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        TERM=xterm LANG=C.UTF-8 "$@"
}

case "${1:-}" in
    fetch) fetch "$2" "$3" ;;
    run) shift; run "$@" ;;
    *) sed -n '2,12p' "$0"; exit 1 ;;
esac
