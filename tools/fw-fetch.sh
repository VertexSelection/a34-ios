#!/bin/bash
# Download one Samsung firmware package with samloader-rs and make sure it is complete.
#
#   fw-fetch.sh MODEL REGION VERSION OUT_DIR
#
# samloader-rs 2.2.0 can finalise a download that still has unwritten gaps (it
# counts a connection the server closed early as finished). This wrapper checks
# the result for gaps, rebuilds the resume data with fw-repair.py when it finds
# any, lets samloader fetch only what is missing, and finally tests the zip.
set -u
MODEL=$1; REGION=$2; VERSION=$3; DIR=$4
HERE=$(cd "$(dirname "$0")" && pwd)
SAM="$HERE/samloader/samloader"
REP="$HERE/fw-repair.py"
LOG="$DIR/download.log"
mkdir -p "$DIR"

say() { echo "[$(date +%H:%M:%S)] $*"; }

# If a download into this directory is already running, let it finish first.
while pgrep -af "samloader download" | grep -qF -- "-d $DIR"; do sleep 10; done

for attempt in 1 2 3 4 5 6 7 8 9 10; do
    ZIP=$(ls "$DIR"/*.zip 2>/dev/null | head -1)
    PART=$(ls "$DIR"/*.zip.part 2>/dev/null | head -1)

    if [ -n "$ZIP" ] && [ -z "$PART" ]; then
        if python3 -I "$REP" holes "$ZIP"; then
            say "no gaps; testing zip contents"
            if unzip -tq "$ZIP"; then
                rm -f "$ZIP.broken"
                say "OK $ZIP"
                exit 0
            fi
            say "zip test FAILED; discarding and starting over"
            rm -f "$ZIP"
            continue
        fi
        say "attempt $attempt: finalised with gaps; rebuilding resume data"
        TMP=$(mktemp -d "$DIR/.fresh.XXXXXX")
        "$SAM" download -m "$MODEL" -r "$REGION" -v "$VERSION" -d "$TMP" >"$TMP/log" 2>&1 &
        SP=$!
        FRESH=""
        for i in $(seq 1 120); do
            FRESH=$(ls "$TMP"/*.zip.part 2>/dev/null | head -1)
            [ -n "$FRESH" ] && python3 -I "$REP" info "$FRESH" >/dev/null 2>&1 && break
            FRESH=""
            sleep 0.5
        done
        kill -INT "$SP" 2>/dev/null; sleep 1; kill -KILL "$SP" 2>/dev/null; wait "$SP" 2>/dev/null
        if [ -z "$FRESH" ]; then
            say "could not get a fresh footer; see $TMP/log"
            exit 2
        fi
        if ! python3 -I "$REP" rebuild "$ZIP" "$FRESH" "$ZIP.part"; then
            say "rebuild failed"
            rm -rf "$TMP"
            exit 3
        fi
        mv -f "$ZIP" "$ZIP.broken"
        rm -rf "$TMP"
        continue
    fi

    say "attempt $attempt: running samloader"
    [ -n "$PART" ] && python3 -I "$REP" info "$PART" 2>/dev/null | tail -1
    "$SAM" download -m "$MODEL" -r "$REGION" -v "$VERSION" -d "$DIR" >>"$LOG" 2>&1
    say "samloader exit=$?"
done
say "gave up after 10 attempts"
exit 1
