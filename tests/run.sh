#!/bin/zsh
# Run a program under the test wine with the native UI on.
#   tests/run.sh [--off] <program> [args...]     e.g. tests/run.sh build/gallery.exe /selftest
# WINE_TREE: the wine tree win32swiftui is installed in (default: the build tree)
# WINEPREFIX: default ~/.wine-w2s
# Stops within TIMEOUT seconds (default 120): TERM first, then wineserver -k.
cd "${0:A:h}/.."
WINE_TREE=${WINE_TREE:-$WINE_BUILD}
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-w2s}
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEDEBUG=${WINEDEBUG:--all}
if [[ $1 == --off ]]; then
    shift
    export WINE_MNC_NATIVE_UI=0
else
    export WINE_MNC_NATIVE_UI=1
fi
TIMEOUT=${TIMEOUT:-120}

"$WINE_TREE/wine" "$@" &
pid=$!
( sleep $TIMEOUT; kill -TERM $pid 2>/dev/null ) &
watchdog=$!
wait $pid
rc=$?
kill $watchdog 2>/dev/null
"$WINE_TREE/server/wineserver" -k 2>/dev/null
exit $rc
