#!/bin/sh
socat -d -d pty EXEC:"./bt-bridge $1 0x12"
SOCAT_PID=$!
echo $SOCAT_PID
PTS_NAME=$(ls -la /proc/$SOCAT_PID/fd | grep -oe '/dev/pts/[1-9][0-9]*')
echo $PTS_NAME
#$PAPARAZZI_HOME/sw/ground/segment/tmtc/link -d $PTS_NAME
