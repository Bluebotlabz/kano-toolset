#!/usr/bin/env python

# webapp.py
#
# Copyright (C) 2014 Kano Computing Ltd.
# License: http://www.gnu.org/licenses/gpl-2.0.txt GNU General Public License v2
#

from gi.repository import Gtk, Gdk, GObject, WebKit
import sys
import re
import os
import urllib
import warnings
import subprocess

import thread
import atexit

from kano.window import gdk_window_settings

def asynchronous_gtk_message(fun):

    def worker((function, args, kwargs)):
        apply(function, args, kwargs)

    def fun2(*args, **kwargs):
        GObject.idle_add(worker, (fun, args, kwargs))

    return fun2


def atexit_pipe_cleanup(pipe_file):
    os.unlink (pipe_file)


def thr_inject_javascript(browser, pipe_file):
    '''
    This function reads from a pipe, a plain message interpreted as Javascript code.
    It then injects that code into the Webkit browser instance.
    From a bash script test it like this:

    $ echo "alert(\"Hello Kano\")" > /tmp/webapp.pipe

    TODO: collect and return synchronous error level? what about pipe security?
    '''
    if os.path.exists (pipe_file):
        os.unlink (pipe_file)

    os.mkfifo (pipe_file)
    while True:
        f = open (pipe_file, 'r')
        pipe_data = f.read().strip('\n')
        asynchronous_gtk_message (browser.execute_script)(pipe_data)
        f.close()


class WebApp(object):
    _index = None
    _title = "Application"

    # Window properties
    _x = None
    _y = None
    _width = None
    _height = None
    _centered = False
    _maximized = False
    _decoration = True
    _taskbar = True
    _app_icon = None

    _zenity = None

    _pipe = True

    def run(self):
        warnings.simplefilter("ignore")

        zenity_cmd = ["zenity", "--progress", "--no-cancel",
                      "--title=Loading",
                      "--text=Loading...",
                      "--width=300", "--height=90", "--auto-close",
                      "--timeout=10", "--auto-kill"]

        self._pipe_name = '/tmp/webapp.pipe'

        self._zenity = subprocess.Popen(zenity_cmd, stdin=subprocess.PIPE)
        zin = self._zenity.stdin
        zin.write("20\n")

        self._view = view = WebKit.WebView()
        view.connect('navigation-policy-decision-requested',
                     self._api_handler)
        view.connect('close-web-view', self._close)
        view.connect('onload-event', self._onload)

        # FIXME: The Inspector cannot be closed once opened.
        # The following line should be commented out for release until we fix this.
        # view.get_settings().set_property("enable-developer-extras", True)

        if hasattr(self.__class__, "_focus_in"):
            view.connect('focus-in-event', self._focus_in)

        if hasattr(self.__class__, "_focus_out"):
            view.connect('focus-out-event', self._focus_out)

        if hasattr(self.__class__, "_download"):
            view.connect('download-requested', self._download)

        zin.write("40\n")

        splitter = Gtk.VPaned()
        sw = Gtk.ScrolledWindow()
        sw.add(view)
        splitter.add1(sw)

        inspector = view.get_inspector()
        inspector.connect("inspect-web-view", self._activate_inspector, splitter)

        zin.write("50\n")

        self._win = win = Gtk.Window(Gtk.WindowType.TOPLEVEL)
        win.set_title(self._title)
        win.connect("destroy", Gtk.main_quit)

        if self._app_icon is not None:
            if os.path.exists(self._app_icon):
                win.set_icon_from_file(self._app_icon)
            else:
                win.set_icon_name(self._app_icon)

        if self._taskbar is False:
            Gtk.Window.set_skip_taskbar_hint(win, True)

        zin.write("70\n")

        win.add(splitter)
        win.realize()
        win.show_all()

        gdk_window_settings(win.get_window(), self._x, self._y,
                            self._width, self._height, self._decoration,
                            self._maximized, self._centered)

        zin.write("90\n")

        view.open(self._index)

        zin.write("99\n")

        # Start a thread that injects Javascript code coming from a filesystem pipe.
        if self._pipe == True:
            atexit.register (atexit_pipe_cleanup, self._pipe_name)
            thread.start_new_thread (thr_inject_javascript, (self._view, self._pipe_name))

        Gtk.main()

    def _activate_inspector(self, inspector, target_view, splitter):
        inspector_view = WebKit.WebView()
        splitter.add2(inspector_view)
        return inspector_view

    def _onload(self, wv, frame, user_data=None):
        if self._zenity:
            try:
                self._zenity.stdin.write("100\n")
            except:
                pass
            del self._zenity

    def exit(self):
        sys.exit(0)

    def error(self, msg):
        sys.stderr.write("Error: %s\n" % msg)

    def chooseFile(self, default_dir=None):
        dialog = Gtk.FileChooserDialog(
            "Open File",
            buttons=(Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
                     Gtk.STOCK_OPEN, Gtk.ResponseType.OK))

        dialog.set_default_response(Gtk.ResponseType.OK)

        filter = Gtk.FileFilter()
        filter.set_name("XML Files")
        filter.add_pattern("*.xml")
        dialog.add_filter(filter)

        if default_dir is not None:
            dialog.set_current_folder(os.path.expanduser(default_dir))

        path = ""

        response = dialog.run()
        if response == Gtk.ResponseType.OK:
            path = dialog.get_filename()
        elif response == Gtk.ResponseType.CANCEL:
            self.error("No files selected.")

        dialog.destroy()

        return path

    def readFile(self, path):
        try:
            with open(path, "r") as f:
                return f.read()
        except:
            self.error("Unable to open file '%s'." % path)
            return ""

    def _close(self, view, data=None):
        sys.exit(0)

    def _parse_api_call(self, call_str):
        call_re = r"#api:(\w+)(\[\d+\])?(/[^/]*)*$"
        call_match = re.search(call_re, call_str)

        name = call_match.group(1)
        call = [name]
        timestamp = call_match.group(2)
        if timestamp is not None:
            call.append(timestamp[1:-1])
        else:
            call.append(None)

        args = re.sub(r"^#api:[^/]*/?", r"", call_match.group(0))

        if len(args) > 0:
            if args[-1] == "/":
                args = args[:-1]
            call += map(urllib.unquote, args.split("/"))

        return call

    def _api_handler(self, view, frame, request, action, decision, data=None):
        uri = action.get_original_uri()

        # Not an api call, let webkit handle it
        if re.search("#api:", uri) is None:
            return False

        func_data = self._parse_api_call(uri)

        name = func_data[0]
        timestamp = func_data[1]
        args = func_data[2:]

        try:
            func = getattr(self, name)
        except AttributeError:
            self.error("API method '%s' doesn't exist!" % name)
            return True

        if len(args) > 0:
            retval = func(*args)
        else:
            retval = func()

        if timestamp is not None:
            if retval is None:
                retval = "null"
            elif type(retval) == int or type(retval) == float:
                retval = str(retval)
            elif type(retval) == str:
                retval = "\"" + urllib.quote(retval, "") + "\""

            script = "backend.trigger_cb(\"%s\", %s, %s);"
            view.execute_script(script % (name, timestamp, retval))

        return True
