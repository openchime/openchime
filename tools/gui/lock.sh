# One GUI run on this machine at a time (docs/TESTING.md, "Driving the Win32
# GUI"). The scripts share one Windows desktop, its clients and its saved
# development sign-ins, whatever checkout they run from: a second run's
# `drive.sh kill` stops the first's clients, and two sign-ins at once slow each
# other. So a run takes this lock before it touches any of them and holds it to
# its end; one that finds it taken waits for it, saying so.
#
# Held across nesting: `run.sh` takes it and exports OC_TEST_GUI_LOCKED, and the
# scripts it runs, and `pair.sh` under them, see that and do not wait on
# themselves.
gui_lock() {
  [ -n "${OC_TEST_GUI_LOCKED:-}" ] && return 0
  exec 9>"${OC_TEST_GUI_LOCK:-/tmp/openchime-gui.lock}"
  if ! flock -n 9; then
    echo "gui_lock: another GUI run is going; waiting for it to finish" >&2
    flock 9
  fi
  export OC_TEST_GUI_LOCKED=1
}
