#!/bin/sh

set -u

ROOT_DIR="/oem"
INDEX_NAME=".buildspot.index"
TEMP_ROOT=""
OLD_STTY=""

cleanup() {
    if [ -n "$OLD_STTY" ]; then
        stty "$OLD_STTY" >/dev/null 2>&1 || true
    fi
}

trap cleanup EXIT HUP INT TERM

reply() {
    printf '%s\n' "$1"
}

reply_error() {
    reply "ERR $1"
}

reply_ok() {
    reply "OK $1"
}

usage() {
    cat <<EOF
Usage: $0 [--stdio] [--root /oem]
EOF
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --stdio)
                shift
                ;;
            --root)
                ROOT_DIR="$2"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                reply_error "ARG unsupported:$1"
                exit 1
                ;;
        esac
    done
}

require_tools() {
    command -v rz >/dev/null 2>&1 || {
        reply_error "ENV missing-rz"
        exit 1
    }
    command -v sz >/dev/null 2>&1 || {
        reply_error "ENV missing-sz"
        exit 1
    }
    command -v sha256sum >/dev/null 2>&1 || {
        reply_error "ENV missing-sha256sum"
        exit 1
    }
    command -v mktemp >/dev/null 2>&1 || {
        reply_error "ENV missing-mktemp"
        exit 1
    }
}

prepare_environment() {
    mkdir -p "$ROOT_DIR" || {
        reply_error "ENV cannot-create-root:$ROOT_DIR"
        exit 1
    }

    TEMP_ROOT="$ROOT_DIR/.buildspot-tmp"
    mkdir -p "$TEMP_ROOT" || {
        reply_error "ENV cannot-create-temp:$TEMP_ROOT"
        exit 1
    }

    if [ -t 0 ]; then
        OLD_STTY=$(stty -g 2>/dev/null || true)
        stty raw -echo -ixon -ixoff 2>/dev/null || true
    fi
}

is_valid_path() {
    case "$1" in
        ""|/*|../*|*/../*|*/..|..)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

find_single_received_file() {
    find "$1" -maxdepth 1 -type f | head -n 1
}

send_index() {
    index_file="$ROOT_DIR/$INDEX_NAME"
    temp_index=$(mktemp "$TEMP_ROOT/index.XXXXXX") || {
        reply_error "INDEX mktemp-failed"
        return 1
    }

    if [ -f "$index_file" ]; then
        cp -f "$index_file" "$temp_index" || {
            rm -f "$temp_index"
            reply_error "INDEX copy-failed"
            return 1
        }
    else
        : > "$temp_index"
    fi

    reply "SEND INDEX"
    if ! sz -b "$temp_index"; then
        rm -f "$temp_index"
        reply_error "INDEX send-failed"
        return 1
    fi

    rm -f "$temp_index"
    reply_ok "INDEX"
    return 0
}

receive_file() {
    file_mode="$1"
    expected_sha="$2"
    expected_size="$3"
    relpath="$4"

    if ! is_valid_path "$relpath"; then
        reply_error "PUT invalid-path:$relpath"
        return 1
    fi

    target="$ROOT_DIR/$relpath"
    target_dir=$(dirname "$target")
    temp_dir=$(mktemp -d "$TEMP_ROOT/file.XXXXXX") || {
        reply_error "PUT mktemp-failed:$relpath"
        return 1
    }

    reply "READY PUT $relpath"

    if ! (cd "$temp_dir" && rz -b -E); then
        rm -rf "$temp_dir"
        reply_error "PUT receive-failed:$relpath"
        return 1
    fi

    received=$(find_single_received_file "$temp_dir")
    if [ -z "$received" ] || [ ! -f "$received" ]; then
        rm -rf "$temp_dir"
        reply_error "PUT missing-file:$relpath"
        return 1
    fi

    actual_sha=$(sha256sum "$received" | awk '{print $1}')
    actual_size=$(wc -c < "$received" | tr -d ' ')

    if [ "$actual_sha" != "$expected_sha" ]; then
        rm -rf "$temp_dir"
        reply_error "PUT sha-mismatch:$relpath"
        return 1
    fi

    if [ "$actual_size" != "$expected_size" ]; then
        rm -rf "$temp_dir"
        reply_error "PUT size-mismatch:$relpath"
        return 1
    fi

    mkdir -p "$target_dir" || {
        rm -rf "$temp_dir"
        reply_error "PUT mkdir-failed:$relpath"
        return 1
    }

    temp_target="$target.part"
    mv -f "$received" "$temp_target" || {
        rm -rf "$temp_dir"
        reply_error "PUT stage-failed:$relpath"
        return 1
    }

    chmod "$file_mode" "$temp_target" 2>/dev/null || true
    mv -f "$temp_target" "$target" || {
        rm -rf "$temp_dir"
        reply_error "PUT commit-failed:$relpath"
        return 1
    }

    rm -rf "$temp_dir"
    reply_ok "FILE $relpath"
    return 0
}

receive_index() {
    expected_sha="$1"
    expected_size="$2"
    temp_dir=$(mktemp -d "$TEMP_ROOT/indexrx.XXXXXX") || {
        reply_error "INDEX mktemp-failed"
        return 1
    }

    reply "READY INDEX"

    if ! (cd "$temp_dir" && rz -b -E); then
        rm -rf "$temp_dir"
        reply_error "INDEX receive-failed"
        return 1
    fi

    received=$(find_single_received_file "$temp_dir")
    if [ -z "$received" ] || [ ! -f "$received" ]; then
        rm -rf "$temp_dir"
        reply_error "INDEX missing-file"
        return 1
    fi

    actual_sha=$(sha256sum "$received" | awk '{print $1}')
    actual_size=$(wc -c < "$received" | tr -d ' ')
    if [ "$actual_sha" != "$expected_sha" ]; then
        rm -rf "$temp_dir"
        reply_error "INDEX sha-mismatch"
        return 1
    fi
    if [ "$actual_size" != "$expected_size" ]; then
        rm -rf "$temp_dir"
        reply_error "INDEX size-mismatch"
        return 1
    fi

    mv -f "$received" "$ROOT_DIR/$INDEX_NAME" || {
        rm -rf "$temp_dir"
        reply_error "INDEX commit-failed"
        return 1
    }

    rm -rf "$temp_dir"
    reply_ok "INDEX_UPDATE"
    return 0
}

process_command() {
    line="$1"
    cmd=${line%% *}

    case "$cmd" in
        HELLO)
            IFS=' ' read -r _ version <<EOF
$line
EOF
            if [ "$version" = "1" ]; then
                reply_ok "HELLO 1"
            else
                reply_error "HELLO unsupported-version:$version"
                return 1
            fi
            ;;
        GET_INDEX)
            send_index
            ;;
        PUT)
            IFS=' ' read -r _ file_mode expected_sha expected_size relpath <<EOF
$line
EOF
            if [ -z "${relpath:-}" ]; then
                reply_error "PUT missing-path"
                return 1
            fi
            receive_file "$file_mode" "$expected_sha" "$expected_size" "$relpath"
            ;;
        PUT_INDEX)
            IFS=' ' read -r _ expected_sha expected_size <<EOF
$line
EOF
            receive_index "$expected_sha" "$expected_size"
            ;;
        DONE)
            reply "BYE"
            return 2
            ;;
        *)
            reply_error "CMD unknown:$cmd"
            return 1
            ;;
    esac

    return $?
}

main() {
    parse_args "$@"
    require_tools
    prepare_environment

    reply "BUILDSPOT/1 READY"

    while IFS= read -r line; do
        [ -z "$line" ] && continue
        process_command "$line"
        status=$?
        if [ "$status" -eq 2 ]; then
            exit 0
        fi
    done

    reply_error "SESSION eof"
    exit 1
}

main "$@"