#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#
# SPDX-License-Identifier: GPL-3.0
#
# GNU Radio Python Flow Graph
# Title: HFSDR Basic Spectrum IQ
# Description: HFSDR WebUSB spectrum and IQ time view
# GNU Radio version: 3.10.12.0

from PyQt5 import Qt
from gnuradio import qtgui
from gnuradio import blocks
from gnuradio import filter
from gnuradio import gr
from gnuradio.filter import firdes
from gnuradio.fft import window
import sys
import signal
from PyQt5 import Qt
from argparse import ArgumentParser
from gnuradio.eng_arg import eng_float, intx
from gnuradio import eng_notation
import basic_spectrum_iq_epy_block_0 as epy_block_0  # embedded python block
import sip
import threading



class basic_spectrum_iq(gr.top_block, Qt.QWidget):

    def __init__(self):
        gr.top_block.__init__(self, "HFSDR Basic Spectrum IQ", catch_exceptions=True)
        Qt.QWidget.__init__(self)
        self.setWindowTitle("HFSDR Basic Spectrum IQ")
        qtgui.util.check_set_qss()
        try:
            self.setWindowIcon(Qt.QIcon.fromTheme('gnuradio-grc'))
        except BaseException as exc:
            print(f"Qt GUI: Could not set Icon: {str(exc)}", file=sys.stderr)
        self.top_scroll_layout = Qt.QVBoxLayout()
        self.setLayout(self.top_scroll_layout)
        self.top_scroll = Qt.QScrollArea()
        self.top_scroll.setFrameStyle(Qt.QFrame.NoFrame)
        self.top_scroll_layout.addWidget(self.top_scroll)
        self.top_scroll.setWidgetResizable(True)
        self.top_widget = Qt.QWidget()
        self.top_scroll.setWidget(self.top_widget)
        self.top_layout = Qt.QVBoxLayout(self.top_widget)
        self.top_grid_layout = Qt.QGridLayout()
        self.top_layout.addLayout(self.top_grid_layout)

        self.settings = Qt.QSettings("gnuradio/flowgraphs", "basic_spectrum_iq")

        try:
            geometry = self.settings.value("geometry")
            if geometry:
                self.restoreGeometry(geometry)
        except BaseException as exc:
            print(f"Qt GUI: Could not restore geometry: {str(exc)}", file=sys.stderr)
        self.flowgraph_started = threading.Event()

        ##################################################
        # Variables
        ##################################################
        self.samp_rate = samp_rate = 192000
        self.gain_const = gain_const = 8.0
        self.freq = freq = 7080000

        ##################################################
        # Blocks
        ##################################################

        self.qtgui_waterfall_sink_x_0 = qtgui.waterfall_sink_c(
            4096, #size
            window.WIN_BLACKMAN_hARRIS, #wintype
            freq, #fc
            samp_rate, #bw
            "HFSDR Waterfall", #name
            1, #number of inputs
            None # parent
        )
        self.qtgui_waterfall_sink_x_0.set_update_time(0.10)
        self.qtgui_waterfall_sink_x_0.enable_grid(True)
        self.qtgui_waterfall_sink_x_0.enable_axis_labels(True)



        labels = ['', '', '', '', '',
                  '', '', '', '', '']
        colors = [0, 0, 0, 0, 0,
                  0, 0, 0, 0, 0]
        alphas = [1.0, 1.0, 1.0, 1.0, 1.0,
                  1.0, 1.0, 1.0, 1.0, 1.0]

        for i in range(1):
            if len(labels[i]) == 0:
                self.qtgui_waterfall_sink_x_0.set_line_label(i, "Data {0}".format(i))
            else:
                self.qtgui_waterfall_sink_x_0.set_line_label(i, labels[i])
            self.qtgui_waterfall_sink_x_0.set_color_map(i, colors[i])
            self.qtgui_waterfall_sink_x_0.set_line_alpha(i, alphas[i])

        self.qtgui_waterfall_sink_x_0.set_intensity_range(-140, 10)

        self._qtgui_waterfall_sink_x_0_win = sip.wrapinstance(self.qtgui_waterfall_sink_x_0.qwidget(), Qt.QWidget)

        self.top_layout.addWidget(self._qtgui_waterfall_sink_x_0_win)
        self.epy_block_0 = epy_block_0.blk(sample_rate=samp_rate, vid=0xCAFE, pid=0x4031, interface=4, endpoint_in=0x85, transfer_bytes=4096, usb_timeout_ms=200, buffer_ms=250, scale=(1.0 / 2147483648.0), lo_hz=freq, apply_lo_on_start=True, module_root="C:\\Users\\zunmun\\Documents\\Stuff\\Github\\GROUP PROJECTS\\hfsdr\\")
        self.dc_blocker_xx_0 = filter.dc_blocker_cc(32, True)
        self.blocks_multiply_const_vxx_0 = blocks.multiply_const_cc(gain_const)


        ##################################################
        # Connections
        ##################################################
        self.connect((self.blocks_multiply_const_vxx_0, 0), (self.qtgui_waterfall_sink_x_0, 0))
        self.connect((self.dc_blocker_xx_0, 0), (self.blocks_multiply_const_vxx_0, 0))
        self.connect((self.epy_block_0, 0), (self.dc_blocker_xx_0, 0))


    def closeEvent(self, event):
        self.settings = Qt.QSettings("gnuradio/flowgraphs", "basic_spectrum_iq")
        self.settings.setValue("geometry", self.saveGeometry())
        self.stop()
        self.wait()

        event.accept()

    def get_samp_rate(self):
        return self.samp_rate

    def set_samp_rate(self, samp_rate):
        self.samp_rate = samp_rate
        self.epy_block_0.sample_rate = self.samp_rate
        self.qtgui_waterfall_sink_x_0.set_frequency_range(self.freq, self.samp_rate)

    def get_gain_const(self):
        return self.gain_const

    def set_gain_const(self, gain_const):
        self.gain_const = gain_const
        self.blocks_multiply_const_vxx_0.set_k(self.gain_const)

    def get_freq(self):
        return self.freq

    def set_freq(self, freq):
        self.freq = freq
        self.epy_block_0.lo_hz = self.freq
        self.qtgui_waterfall_sink_x_0.set_frequency_range(self.freq, self.samp_rate)




def main(top_block_cls=basic_spectrum_iq, options=None):

    qapp = Qt.QApplication(sys.argv)

    tb = top_block_cls()

    tb.start()
    tb.flowgraph_started.set()

    tb.show()

    def sig_handler(sig=None, frame=None):
        tb.stop()
        tb.wait()

        Qt.QApplication.quit()

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    timer = Qt.QTimer()
    timer.start(500)
    timer.timeout.connect(lambda: None)

    qapp.exec_()

if __name__ == '__main__':
    main()
