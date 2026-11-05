#!/usr/bin/env python3
"""Simulates a real XDND drag & drop of files onto a window (by WM_NAME),
acting as the drag *source* the way a file manager would: Enter -> Position
-> (Status) -> Drop -> answer the SelectionRequest with a text/uri-list ->
(Finished). Lets test/clap's GUI run verify the plugin's drop handling end
to end on a headless Xvfb display, where no real mouse drag is possible.

Usage: DISPLAY=:99 xdnd_drop.py <window name> <file> [file...]
Needs python3-xlib.
"""
import sys
import time
import urllib.parse

from Xlib import X, Xatom, display, protocol


def find_window(d, root, name):
    for w in root.query_tree().children:
        try:
            if w.get_wm_name() == name:
                return w
        except Exception:
            pass
        found = find_window(d, w, name)
        if found:
            return found
    return None


def client_message(d, target, msg_type, data):
    ev = protocol.event.ClientMessage(window=target, client_type=msg_type,
                                      data=(32, (data + [0] * 5)[:5]))
    target.send_event(ev, event_mask=0)
    d.flush()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    name, files = sys.argv[1], sys.argv[2:]
    d = display.Display()
    root = d.screen().root
    target = None
    for _ in range(50):  # the plugin window may still be appearing
        target = find_window(d, root, name)
        if target:
            break
        time.sleep(0.1)
    if not target:
        print("window not found:", name)
        return 1

    atom = lambda n: d.intern_atom(n)
    a_enter, a_pos, a_status = atom("XdndEnter"), atom("XdndPosition"), atom("XdndStatus")
    a_drop, a_finished = atom("XdndDrop"), atom("XdndFinished")
    a_sel, a_copy, a_uri = atom("XdndSelection"), atom("XdndActionCopy"), atom("text/uri-list")

    src = root.create_window(0, 0, 1, 1, 0, X.CopyFromParent)
    src.set_selection_owner(a_sel, X.CurrentTime)
    uri_list = "".join("file://" + urllib.parse.quote(f) + "\r\n" for f in files).encode()

    geom = target.translate_coords(root, 0, 0)
    x, y = -geom.x + 400, -geom.y + 300  # somewhere inside the window, root coords

    client_message(d, target, a_enter, [src.id, 5 << 24, a_uri, 0, 0])
    client_message(d, target, a_pos, [src.id, 0, (x << 16) | y, X.CurrentTime, a_copy])

    accepted, finished = False, False
    deadline = time.time() + 5
    dropped = False
    while time.time() < deadline and not finished:
        while d.pending_events():
            ev = d.next_event()
            if ev.type == X.ClientMessage and ev.client_type == a_status:
                accepted = bool(ev.data[1][1] & 1)
                if accepted and not dropped:
                    client_message(d, target, a_drop, [src.id, 0, X.CurrentTime, 0, 0])
                    dropped = True
            elif ev.type == X.ClientMessage and ev.client_type == a_finished:
                finished = True
            elif ev.type == X.SelectionRequest:
                prop = ev.property or ev.target
                ev.requestor.change_property(prop, ev.target, 8, uri_list)
                notify = protocol.event.SelectionNotify(
                    time=ev.time, requestor=ev.requestor, selection=ev.selection,
                    target=ev.target, property=prop)
                ev.requestor.send_event(notify, event_mask=0)
                d.flush()
        time.sleep(0.02)

    print("accepted=%s dropped=%s finished=%s" % (accepted, dropped, finished))
    return 0 if finished else 1


if __name__ == "__main__":
    sys.exit(main())
