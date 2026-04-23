#!/usr/bin/env python3
"""Demo GNU Radio flowgraph for HFSDR WebUSB source."""

import os
import signal
import sys

from PyQt5 import Qt
from gnuradio import blocks
from gnuradio import filter
from gnuradio import gr
from gnuradio import qtgui
from gnuradio.filter import firdes
from gnuradio.fft import window
import sip

_flowgraph_dir = os.path.dirname(os.path.abspath(__file__))
_gr_python_dir = os.path.abspath(
    os.path.join(_flowgraph_dir, "..", "gr-hfsdr-lib", "python")
)
if _gr_python_dir not in sys.path:
    sys.path.insert(0, _gr_python_dir)
from hfsdr_source import hfsdr_source


class hfsdr_webusb_demo(gr.top_block, Qt.QWidget):
    def __init__(self):
        gr.top_block.__init__(self, "HFSDR WebUSB Demo", catch_exceptions=True)
        Qt.QWidget.__init__(self)

        self.setWindowTitle("HFSDR WebUSB Demo")
        self.layout = Qt.QVBoxLayout(self)

        self.sample_rate = 192000

        self.src = hfsdr_source(
            sample_rate=self.sample_rate,
            vid=0xCAFE,
            pid=0x4031,
            interface=4,
            endpoint_in=0x85,
            transfer_bytes=4096,
            usb_timeout_ms=200,
            buffer_ms=250,
        )

        self.dc_block = filter.dc_blocker_cc(64, True)
        self.gain = blocks.multiply_const_cc(8.0)

        self.freq = qtgui.freq_sink_c(
            4096,
            window.WIN_BLACKMAN_hARRIS,
            0,
            self.sample_rate,
            "HFSDR Spectrum",
            1,
        )
        self.freq.set_update_time(0.1)
        self.freq.enable_axis_labels(True)

        self.time = qtgui.time_sink_c(2048, self.sample_rate, "HFSDR IQ", 1)
        self.time.set_update_time(0.1)
        self.time.enable_axis_labels(True)

        self.layout.addWidget(sip.wrapinstance(self.freq.qwidget(), Qt.QWidget))
        self.layout.addWidget(sip.wrapinstance(self.time.qwidget(), Qt.QWidget))

        self.connect((self.src, 0), (self.dc_block, 0))
        self.connect((self.dc_block, 0), (self.gain, 0))
        self.connect((self.gain, 0), (self.freq, 0))
        self.connect((self.gain, 0), (self.time, 0))


def main():
    app = Qt.QApplication(sys.argv)
    tb = hfsdr_webusb_demo()
    tb.start()
    tb.show()

    def _stop(*_args):
        tb.stop()
        tb.wait()
        app.quit()

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    timer = Qt.QTimer()
    timer.start(200)
    timer.timeout.connect(lambda: None)
    app.exec_()


if __name__ == "__main__":
    main()
