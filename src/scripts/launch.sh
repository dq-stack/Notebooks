#!/bin/sh
#
# Started by the "Notebooks" Library tile. Registers the app with the
# Kindle's app manager (once), starts the helper and opens the app.
#
# The helper runs from a copy in /tmp: USB drive mode kills anything using
# /mnt/us, and a killed helper would just mean saves stop working.
#

APP_ID="com.dqstack.notebooks"
BASE="/mnt/us/extensions/notebooks"
RUN="/tmp/notebooks"
LOG="$RUN/launch.log"
APPREG="/var/local/appreg.db"

mkdir -p "$RUN" /mnt/us/notebooks

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') $1" >> "$LOG"
}

if [ -e /lib/ld-linux-armhf.so.3 ]; then
    HELPER="$BASE/bin/notebooks_helper_hf"
else
    HELPER="$BASE/bin/notebooks_helper"
fi

if [ ! -f "$HELPER" ]; then
    log "ERROR: $HELPER missing"
    exit 1
fi

# The page lives in notebooks-app/: mesquite names the app's storage folder
# after it, so a generic name like "app" could share settings with another
# app. Test builds before 1.0.0 used app/; remove our own leftover copy.
rm -rf "$BASE/app"

# Register (or update) the app. Values are constants, never user input.
sqlite3 "$APPREG" <<EOF
INSERT OR IGNORE INTO interfaces(interface) VALUES('application');
INSERT OR IGNORE INTO handlerIds(handlerId) VALUES('$APP_ID');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('$APP_ID','lipcId','$APP_ID');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('$APP_ID','command','/usr/bin/mesquite -l $APP_ID -c file://$BASE/notebooks-app/');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('$APP_ID','supportedOrientation','U');
EOF

VERSION="$(cat "$BASE/VERSION" 2>/dev/null)"
APP_PIDS="$(ps | grep "mesquite -l $APP_ID" | grep -v grep | awk '{print $1}')"

# An open Notebooks window is normally kept, so you come back to the same
# notebook, page or half-written note. It's only closed after an update,
# because the old window would keep running the old code.
if [ -n "$APP_PIDS" ] && [ "$(cat "$RUN/running_version" 2>/dev/null)" != "$VERSION" ]; then
    log "closing window from version $(cat "$RUN/running_version" 2>/dev/null) (now $VERSION)"
    lipc-set-prop com.lab126.appmgrd stop "app://$APP_ID" >/dev/null 2>&1
    sleep 1

    for PID in $APP_PIDS; do
        kill "$PID" 2>/dev/null
    done
fi

echo "$VERSION" > "$RUN/running_version"

# Start the helper unless the same version is already running.
HELPER_PID="$(cat "$RUN/helper.pid" 2>/dev/null)"

if [ -n "$HELPER_PID" ] && kill -0 "$HELPER_PID" 2>/dev/null &&
    [ "$(cat "$RUN/helper_version" 2>/dev/null)" = "$VERSION" ]; then
    log "helper already running (pid $HELPER_PID)"
else
    if [ -n "$HELPER_PID" ]; then
        kill "$HELPER_PID" 2>/dev/null
        sleep 1
    fi

    cp "$HELPER" "$RUN/notebooks_helper" && chmod +x "$RUN/notebooks_helper"

    (cd / && exec "$RUN/notebooks_helper" "$RUN/helper.log") &
    echo $! > "$RUN/helper.pid"
    echo "$VERSION" > "$RUN/helper_version"
    log "helper started (pid $!)"
fi

# Open the app only after this script has finished: when a Library script
# exits, the Kindle returns to the Library, which would close an app started
# while the script was still running (the "flash and nothing" on launch).
(
    sleep 2
    lipc-set-prop com.lab126.appmgrd start "app://$APP_ID"
    log "app started"
) </dev/null >/dev/null 2>&1 &

log "launcher done"
