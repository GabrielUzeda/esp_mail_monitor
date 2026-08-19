#!/run/current-system/sw/bin/bash

PID_FILE="/tmp/esp32_monitor.pid"

PORT="${PORT:-/dev/ttyUSB0}"
BAUD="${BAUD:-115200}"

case "$1" in
    start)
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE")
            if ps -p "$PID" > /dev/null; then
                echo "Monitor already running (PID: $PID)"
                exit 1
            else
                echo "Stale PID file found. Removing."
                rm "$PID_FILE"
            fi
        fi
        echo "Starting monitor on $PORT ($BAUD)..."
        nohup nix-shell -p python3Packages.pyserial --run "python3 monitor.py --port $PORT --baud $BAUD" > /tmp/esp32_monitor.out 2>&1 &
        echo $! > "$PID_FILE"
        echo "Monitor started with PID: $(cat $PID_FILE)"
        ;;
    stop)
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE")
            echo "Stopping monitor (PID: $PID)..."
            kill "$PID"
            rm "$PID_FILE"
            echo "Monitor stopped."
        else
            echo "Monitor is not running."
        fi
        ;;
    status)
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE")
            if ps -p "$PID" > /dev/null; then
                echo "Monitor is running (PID: $PID)."
            else
                echo "Monitor is not running (stale PID file)."
            fi
        else
            echo "Monitor is not running."
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|status}"
        exit 1
        ;;
esac
