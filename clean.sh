SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)

fusermount -zu ~/mounts/waydroid
fusermount -zu ~/mounts/maiuna
fusermount -zu "$SCRIPT_DIR/test/mount"

proxy_launcher="$(pgrep -f "adb shell")"
if [[ -n "$proxy_launcher" ]]; then
    echo "killing proxy launcher process: ${proxy_launcher}"
    kill -9 "$proxy_launcher"
fi

rm /run/user/$(id -u)/madbfs@*
