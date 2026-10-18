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

# Normally nothing is left running: the helper closes Notebooks when you
# leave it (memory is tight) and the app restores your place from state.js.
# Clear out anything left over anyway, e.g. after an update.
for PID in $(ps | grep "mesquite -l $APP_ID" | grep -v grep | awk '{print $1}'); do
    kill "$PID" 2>/dev/null
done

if [ -f "$RUN/helper.pid" ]; then
    kill "$(cat "$RUN/helper.pid")" 2>/dev/null
    sleep 1
fi

cp "$HELPER" "$RUN/notebooks_helper" && chmod +x "$RUN/notebooks_helper"

(cd / && exec "$RUN/notebooks_helper" "$RUN/helper.log") &
echo $! > "$RUN/helper.pid"
log "helper started (pid $!)"

# Open the app only after this script has finished: when a Library script
# exits, the Kindle returns to the Library, which would close an app started
# while the script was still running (the "flash and nothing" on launch).
(
    sleep 2
    lipc-set-prop com.lab126.appmgrd start "app://$APP_ID"
    log "app started"
) </dev/null >/dev/null 2>&1 &

log "launcher done"
