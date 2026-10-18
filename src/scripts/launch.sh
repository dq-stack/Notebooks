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

# Register (or update) the app. Values are constants, never user input.
sqlite3 "$APPREG" <<EOF
INSERT OR IGNORE INTO interfaces(interface) VALUES('application');
INSERT OR IGNORE INTO handlerIds(handlerId) VALUES('$APP_ID');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('$APP_ID','lipcId','$APP_ID');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('$APP_ID','command','/usr/bin/mesquite -l $APP_ID -c file://$BASE/app/');
INSERT OR REPLACE INTO properties(handlerId,name,value) VALUES('$APP_ID','supportedOrientation','U');
EOF

# Restart the helper so a new version (or a stuck one) is always replaced.
if [ -f "$RUN/helper.pid" ]; then
    kill "$(cat "$RUN/helper.pid")" 2>/dev/null
    sleep 1
fi

cp "$HELPER" "$RUN/notebooks_helper" && chmod +x "$RUN/notebooks_helper"

(cd / && exec "$RUN/notebooks_helper" "$RUN/helper.log") &
echo $! > "$RUN/helper.pid"
log "helper started (pid $!)"

# Give it a moment to write the first data.js, then open the app.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -f "$RUN/data.js" ] && break
    sleep 0.3
done

lipc-set-prop com.lab126.appmgrd start "app://$APP_ID"
log "app started"
