#!/bin/sh
# Fetch the pinned ITU-T H.264 and WebM/libvpx VP9 conformance vectors.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
MANIFEST=${MANIFEST:-$SCRIPT_DIR/conformance-vectors.tsv}
VECTOR_DIR=${VECTOR_DIR:-$SCRIPT_DIR/vectors}
DOWNLOAD_DIR=$VECTOR_DIR/.downloads

mkdir -p "$VECTOR_DIR" "$DOWNLOAD_DIR"

checksum()
{
    sha256sum "$1" | awk '{print $1}'
}

verify()
{
    [ -f "$1" ] && [ "$(checksum "$1")" = "$2" ]
}

tab=$(printf '\t')
while IFS="$tab" read -r codec output download url download_sha member payload_sha decode_path risk <&3; do
    case $codec in
        ''|'#'*) continue ;;
    esac

    : "$decode_path"
    payload=$VECTOR_DIR/$output
    cached=$DOWNLOAD_DIR/$download

    if verify "$payload" "$payload_sha"; then
        echo "ok    $codec/$output"
        continue
    fi

    if ! verify "$cached" "$download_sha"; then
        part=$cached.part
        rm -f "$part"
        echo "fetch $url"
        curl --fail --location --retry 3 --output "$part" "$url"
        if ! verify "$part" "$download_sha"; then
            echo "error: checksum mismatch for $download" >&2
            rm -f "$part"
            exit 1
        fi
        mv "$part" "$cached"
    fi

    part=$payload.part
    rm -f "$part"
    if [ "$member" = - ]; then
        cp "$cached" "$part"
    else
        unzip -p "$cached" "$member" >"$part"
    fi
    if ! verify "$part" "$payload_sha"; then
        echo "error: checksum mismatch for extracted $output" >&2
        rm -f "$part"
        exit 1
    fi
    mv "$part" "$payload"
    echo "ok    $codec/$output ($risk)"
done 3<"$MANIFEST"
