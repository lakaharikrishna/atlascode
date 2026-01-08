#!/bin/bash

PROCESS_NAME="FINAL_HES_NEW_DYNAMIC_MERGE"

if [ "$#" -eq 1 ]; then
    val="${1,,}"  # Convert input to lowercase

    case "$val" in
        start | restart)
            case "$(pidof $PROCESS_NAME | wc -w)" in
                0)
                    log_dir="Master_Logs"
                    logfile="$log_dir/$(date +%d%m%Y_%H%M).txt"
                    mkdir -p "$log_dir"
                    ulimit -n 16384
                    stdbuf -o0 ./$PROCESS_NAME >> "$logfile" &
                    echo "Started"
                    ;;
                1)
                    echo "Running"
                    ;;
            esac
            ;;
        stop)
            killall "$PROCESS_NAME"
            echo "Stopped"
            ;;
        status)
            case "$(pidof $PROCESS_NAME | wc -w)" in
                0) echo "Stopped" ;;
                1) echo "Running" ;;
            esac
            ;;
        *)
            echo "Not a valid command"
            ;;
    esac
else
    echo "Invalid Arguments"
fi
