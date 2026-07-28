#!/usr/bin/python3
"""Local two-peer GStreamer WebRTC transport for the hardware encode gate."""

import argparse
import json
import sys
from pathlib import Path

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstWebRTC", "1.0")
from gi.repository import GLib, Gst, GstWebRTC  # noqa: E402


class PeerGate:
    def __init__(self, args):
        self.args = args
        self.loop = GLib.MainLoop()
        self.failed = None
        self.offer_started = False
        self.stream_scheduled = False
        self.stream_started = False
        self.push_started = False
        self.finish_scheduled = False
        self.frame_index = 0
        self.received_access_units = 0
        self.candidates = {"sender": 0, "receiver": 0}
        self.pending_candidates = {"sender": [], "receiver": []}
        self.remote_ready = {"sender": False, "receiver": False}
        self.connection_states = {"sender": "new", "receiver": "new"}
        self.ice_states = {"sender": "new", "receiver": "new"}
        self.sdp = {"offer": "", "answer": ""}
        self.dtls = {"sender": [], "receiver": []}
        self.frame_file = open(args.input, "rb")

        missing = [
            name
            for name in (
                "appsrc",
                args.encoder,
                "h264parse",
                "rtph264pay",
                "webrtcbin",
                "queue",
                "capsfilter",
                "rtph264depay",
                "identity",
                "filesink",
                "nicesrc",
                "nicesink",
                "dtlssrtpenc",
                "dtlssrtpdec",
                "srtpenc",
                "srtpdec",
            )
            if Gst.ElementFactory.find(name) is None
        ]
        if missing:
            raise RuntimeError(
                "required GStreamer elements are unavailable: "
                + ", ".join(missing)
            )

        sender_description = (
            "appsrc name=negotiation-source is-live=true format=time "
            "caps=application/x-rtp,media=video,encoding-name=H264,"
            "clock-rate=90000,payload=96,packetization-mode=(string)1 "
            "! webrtcbin name=sender bundle-policy=max-bundle latency=0"
        )
        self.sender_pipeline = Gst.parse_launch(sender_description)
        self.sender = self.sender_pipeline.get_by_name("sender")
        self.negotiation_source = self.sender_pipeline.get_by_name(
            "negotiation-source"
        )
        negotiation_pad = self.negotiation_source.get_static_pad("src")
        self.sender_sink_pad = negotiation_pad.get_peer()
        if self.sender_sink_pad is None:
            raise RuntimeError("negotiation source is not linked to webrtcbin")
        self.source = None
        self.send_bin = None
        self.stream_pts_offset = 0

        self.receiver_pipeline = Gst.Pipeline.new("receiver-pipeline")
        self.receiver = Gst.ElementFactory.make("webrtcbin", "receiver")
        self.receiver.set_property(
            "bundle-policy", GstWebRTC.WebRTCBundlePolicy.MAX_BUNDLE
        )
        self.receiver.set_property("latency", 0)
        self.receiver_pipeline.add(self.receiver)

        self.sender.connect("on-negotiation-needed", self.on_negotiation)
        self.sender.connect(
            "on-ice-candidate", self.on_ice_candidate, "sender"
        )
        self.receiver.connect(
            "on-ice-candidate", self.on_ice_candidate, "receiver"
        )
        self.receiver.connect("pad-added", self.on_receiver_pad)
        for name, peer in (
            ("sender", self.sender),
            ("receiver", self.receiver),
        ):
            peer.connect(
                "notify::connection-state",
                self.on_connection_state,
                name,
            )
            peer.connect(
                "notify::ice-connection-state",
                self.on_ice_state,
                name,
            )

        for pipeline in (self.sender_pipeline, self.receiver_pipeline):
            bus = pipeline.get_bus()
            bus.add_signal_watch()
            bus.connect("message", self.on_bus_message)

    def log(self, message):
        print(message, flush=True)

    def fail(self, message):
        if self.failed is None:
            self.failed = message
            print(f"FAIL  {message}", file=sys.stderr, flush=True)
            GLib.idle_add(self.loop.quit)

    def on_bus_message(self, _bus, message):
        if message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            self.fail(f"GStreamer error: {error.message}; {debug or ''}")
        elif message.type == Gst.MessageType.WARNING:
            error, debug = message.parse_warning()
            self.log(f"warning: {error.message}; {debug or ''}")

    def on_negotiation(self, _peer):
        if self.offer_started:
            return
        self.offer_started = True
        self.log("offer-requested")
        promise = Gst.Promise.new_with_change_func(
            self.on_offer_created, None, None
        )
        self.sender.emit("create-offer", None, promise)

    def on_offer_created(self, promise, _unused, _unused2):
        if promise.wait() != Gst.PromiseResult.REPLIED:
            self.fail("create-offer promise did not reply")
            return
        reply = promise.get_reply()
        offer = reply.get_value("offer")
        self.sdp["offer"] = offer.sdp.as_text()
        self.log("offer-created")
        local = Gst.Promise.new()
        self.sender.emit("set-local-description", offer, local)
        local.interrupt()
        remote = Gst.Promise.new_with_change_func(
            self.on_receiver_offer_set, None, None
        )
        self.receiver.emit("set-remote-description", offer, remote)

    def on_receiver_offer_set(self, promise, _unused, _unused2):
        if promise.wait() != Gst.PromiseResult.REPLIED:
            self.fail("receiver set-remote-description did not reply")
            return
        self.remote_ready["receiver"] = True
        self.flush_candidates("receiver")
        answer = Gst.Promise.new_with_change_func(
            self.on_answer_created, None, None
        )
        self.receiver.emit("create-answer", None, answer)

    def on_answer_created(self, promise, _unused, _unused2):
        if promise.wait() != Gst.PromiseResult.REPLIED:
            self.fail("create-answer promise did not reply")
            return
        reply = promise.get_reply()
        answer = reply.get_value("answer")
        self.sdp["answer"] = answer.sdp.as_text()
        self.log("answer-created")
        local = Gst.Promise.new()
        self.receiver.emit("set-local-description", answer, local)
        local.interrupt()
        remote = Gst.Promise.new_with_change_func(
            self.on_sender_answer_set, None, None
        )
        self.sender.emit("set-remote-description", answer, remote)

    def on_sender_answer_set(self, promise, _unused, _unused2):
        if promise.wait() != Gst.PromiseResult.REPLIED:
            self.fail("sender set-remote-description did not reply")
            return
        self.remote_ready["sender"] = True
        self.flush_candidates("sender")
        self.log("answer-applied")

    def on_ice_candidate(self, _peer, mline_index, candidate, source_name):
        self.candidates[source_name] += 1
        target_name = "receiver" if source_name == "sender" else "sender"
        if self.remote_ready[target_name]:
            self.add_candidate(target_name, mline_index, candidate)
        else:
            self.pending_candidates[target_name].append(
                (mline_index, candidate)
            )

    def add_candidate(self, target_name, mline_index, candidate):
        target = self.sender if target_name == "sender" else self.receiver
        target.emit("add-ice-candidate", mline_index, candidate)

    def flush_candidates(self, target_name):
        for mline_index, candidate in self.pending_candidates[target_name]:
            self.add_candidate(target_name, mline_index, candidate)
        self.pending_candidates[target_name].clear()

    def on_connection_state(self, peer, _property, name):
        state = peer.get_property("connection-state")
        self.connection_states[name] = state.value_nick
        self.log(f"connection-state {name}={state.value_nick}")
        if state == GstWebRTC.WebRTCPeerConnectionState.FAILED:
            self.fail(f"{name} peer connection failed")
            return
        if (
            not self.stream_started
            and not self.stream_scheduled
            and all(
                value == "connected"
                for value in self.connection_states.values()
            )
        ):
            self.stream_scheduled = True
            self.log("both-peers-connected")
            GLib.idle_add(self.start_stream)

    def on_ice_state(self, peer, _property, name):
        state = peer.get_property("ice-connection-state")
        self.ice_states[name] = state.value_nick
        self.log(f"ice-state {name}={state.value_nick}")
        if state == GstWebRTC.WebRTCICEConnectionState.FAILED:
            self.fail(f"{name} ICE connection failed")

    def on_receiver_pad(self, _peer, pad):
        if pad.get_direction() != Gst.PadDirection.SRC:
            return
        caps = pad.get_current_caps() or pad.query_caps(None)
        if not caps or caps.get_size() == 0:
            self.fail("receiver pad has no caps")
            return
        structure = caps.get_structure(0)
        if (
            structure.get_name() != "application/x-rtp"
            or structure.get_string("encoding-name") != "H264"
        ):
            self.fail(f"receiver negotiated unexpected caps: {caps.to_string()}")
            return

        elements = [
            Gst.ElementFactory.make("queue", "receive-queue"),
            Gst.ElementFactory.make("rtph264depay", "receive-depay"),
            Gst.ElementFactory.make("h264parse", "receive-parser"),
            Gst.ElementFactory.make("capsfilter", "receive-caps"),
            Gst.ElementFactory.make("identity", "receive-counter"),
            Gst.ElementFactory.make("filesink", "receive-output"),
        ]
        if any(element is None for element in elements):
            self.fail("failed to construct receiver chain")
            return
        queue, depay, parser, caps_filter, counter, output = elements
        caps_filter.set_property(
            "caps",
            Gst.Caps.from_string(
                "video/x-h264,stream-format=byte-stream,alignment=au"
            ),
        )
        counter.set_property("signal-handoffs", True)
        counter.connect("handoff", self.on_received_access_unit)
        output.set_property("location", str(self.args.output))
        output.set_property("sync", False)
        for element in elements:
            self.receiver_pipeline.add(element)
        for first, second in zip(elements, elements[1:]):
            if not first.link(second):
                self.fail(
                    f"failed to link receiver elements "
                    f"{first.get_name()} -> {second.get_name()}"
                )
                return
        for element in elements:
            if not element.sync_state_with_parent():
                self.fail(f"failed to start receiver element {element.get_name()}")
                return
        result = pad.link(queue.get_static_pad("sink"))
        if result != Gst.PadLinkReturn.OK:
            self.fail(f"failed to link receiver WebRTC pad: {result.value_nick}")
            return
        self.log(f"receiver-pad-linked caps={caps.to_string()}")

    def on_received_access_unit(self, _identity, _buffer):
        self.received_access_units += 1
        if (
            self.received_access_units >= self.args.frames
            and not self.finish_scheduled
        ):
            self.finish_scheduled = True
            GLib.timeout_add(500, self.finish)

    def start_stream(self):
        if self.stream_started or self.failed is not None:
            return GLib.SOURCE_REMOVE
        negotiation_pad = self.negotiation_source.get_static_pad("src")
        self.negotiation_source.set_state(Gst.State.NULL)
        negotiation_pad.unlink(self.sender_sink_pad)
        if not self.sender_pipeline.remove(self.negotiation_source):
            self.fail("failed to remove negotiation source")
            return GLib.SOURCE_REMOVE

        if self.args.encoder == "vah264enc":
            encoder_description = (
                "vah264enc rate-control=cbr bitrate=1000 "
                f"key-int-max={self.args.fps}"
            )
            h264_profile = "high"
        else:
            encoder_description = (
                "openh264enc rate-control=bitrate bitrate=1000000 "
                f"gop-size={self.args.fps}"
            )
            h264_profile = "constrained-high"
        send_description = (
            "appsrc name=source is-live=true format=time block=true "
            f"caps=video/x-raw,format=I420,width={self.args.width},"
            f"height={self.args.height},framerate={self.args.fps}/1 "
            "! queue max-size-buffers=4 "
            f"! {encoder_description} "
            "! h264parse config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au,"
            f"profile={h264_profile} "
            "! rtph264pay aggregate-mode=zero-latency config-interval=-1 "
            f"mtu={self.args.mtu} pt=96 "
            "! application/x-rtp,media=video,encoding-name=H264,"
            "clock-rate=90000,payload=96,packetization-mode=(string)1 "
            "! identity name=send-output"
        )
        try:
            self.send_bin = Gst.parse_bin_from_description(
                send_description, True
            )
        except GLib.Error as error:
            self.fail(f"failed to create encoder bin: {error.message}")
            return GLib.SOURCE_REMOVE
        self.source = self.send_bin.get_by_name("source")
        if self.source is None or not self.sender_pipeline.add(self.send_bin):
            self.fail("failed to add encoder bin to sender pipeline")
            return GLib.SOURCE_REMOVE
        result = self.send_bin.get_static_pad("src").link(
            self.sender_sink_pad
        )
        if result != Gst.PadLinkReturn.OK:
            self.fail(f"failed to attach encoder to WebRTC pad: {result.value_nick}")
            return GLib.SOURCE_REMOVE
        if not self.send_bin.sync_state_with_parent():
            self.fail("failed to start dynamically attached encoder")
            return GLib.SOURCE_REMOVE
        self.stream_started = True
        self.log(f"media-encoder-attached encoder={self.args.encoder}")
        GLib.timeout_add(250, self.begin_pushing)
        return GLib.SOURCE_REMOVE

    def begin_pushing(self):
        if self.push_started or self.failed is not None:
            return GLib.SOURCE_REMOVE
        clock = self.sender_pipeline.get_clock()
        if clock is None:
            self.fail("sender pipeline has no clock")
            return GLib.SOURCE_REMOVE
        self.stream_pts_offset = (
            clock.get_time() - self.sender_pipeline.get_base_time()
        )
        self.push_started = True
        self.log("frame-push-started")
        interval_ms = max(1, round(1000 / self.args.fps))
        GLib.timeout_add(interval_ms, self.push_frame)
        return GLib.SOURCE_REMOVE

    def push_frame(self):
        if self.failed is not None:
            return GLib.SOURCE_REMOVE
        if self.frame_index >= self.args.frames:
            self.source.emit("end-of-stream")
            self.log("source-eos")
            return GLib.SOURCE_REMOVE

        frame_size = self.args.width * self.args.height * 3 // 2
        data = self.frame_file.read(frame_size)
        if len(data) != frame_size:
            self.fail(
                f"raw input ended at frame {self.frame_index}: "
                f"got {len(data)} bytes, expected {frame_size}"
            )
            return GLib.SOURCE_REMOVE
        duration = Gst.SECOND // self.args.fps
        buffer = Gst.Buffer.new_allocate(None, frame_size, None)
        buffer.fill(0, data)
        buffer.pts = self.stream_pts_offset + self.frame_index * duration
        buffer.dts = Gst.CLOCK_TIME_NONE
        buffer.duration = duration
        flow = self.source.emit("push-buffer", buffer)
        if flow != Gst.FlowReturn.OK:
            self.fail(
                f"appsrc rejected frame {self.frame_index}: {flow.value_nick}"
            )
            return GLib.SOURCE_REMOVE
        self.frame_index += 1
        return GLib.SOURCE_CONTINUE

    @staticmethod
    def iter_elements(bin_element):
        iterator = bin_element.iterate_recurse()
        while True:
            result, value = iterator.next()
            if result == Gst.IteratorResult.OK:
                yield value
            elif result == Gst.IteratorResult.RESYNC:
                iterator.resync()
            else:
                break

    def collect_dtls(self, name, peer):
        states = []
        for element in self.iter_elements(peer):
            factory = element.get_factory()
            factory_name = factory.get_name() if factory else ""
            if factory_name not in ("dtlssrtpenc", "dtlssrtpdec"):
                continue
            state = element.get_property("connection-state")
            states.append(
                {
                    "element": element.get_name(),
                    "factory": factory_name,
                    "state": state.value_nick,
                }
            )
        self.dtls[name] = states
        return states

    def finish(self):
        if self.failed is not None:
            return GLib.SOURCE_REMOVE
        for description_name, text in self.sdp.items():
            for marker in (
                "m=video",
                "H264/90000",
                "a=ice-ufrag:",
                "a=fingerprint:",
                "a=setup:",
            ):
                if marker not in text:
                    self.fail(
                        f"{description_name} SDP is missing {marker}"
                    )
                    return GLib.SOURCE_REMOVE
        if any(count == 0 for count in self.candidates.values()):
            self.fail(f"ICE candidate exchange is empty: {self.candidates}")
            return GLib.SOURCE_REMOVE
        if not all(
            state in ("connected", "completed")
            for state in self.ice_states.values()
        ):
            self.fail(f"ICE did not connect: {self.ice_states}")
            return GLib.SOURCE_REMOVE
        for name, peer in (
            ("sender", self.sender),
            ("receiver", self.receiver),
        ):
            states = self.collect_dtls(name, peer)
            if not states or not any(
                item["state"] == "connected" for item in states
            ):
                self.fail(f"{name} has no connected DTLS-SRTP element: {states}")
                return GLib.SOURCE_REMOVE

        audit = {
            "encoder": self.args.encoder,
            "frames_pushed": self.frame_index,
            "access_units_received": self.received_access_units,
            "ice_candidates": self.candidates,
            "connection_states": self.connection_states,
            "ice_states": self.ice_states,
            "dtls": self.dtls,
            "offer_has_fingerprint": "a=fingerprint:" in self.sdp["offer"],
            "answer_has_fingerprint": "a=fingerprint:" in self.sdp["answer"],
        }
        self.args.audit.write_text(
            json.dumps(audit, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        self.log(
            "transport-complete "
            f"frames={self.received_access_units} "
            f"candidates={self.candidates['sender']}/"
            f"{self.candidates['receiver']}"
        )
        self.loop.quit()
        return GLib.SOURCE_REMOVE

    def timeout(self):
        self.fail(
            "peer gate timed out "
            f"connections={self.connection_states} "
            f"ice={self.ice_states} "
            f"pushed={self.frame_index} "
            f"received={self.received_access_units}"
        )
        return GLib.SOURCE_REMOVE

    def run(self):
        self.receiver_pipeline.set_state(Gst.State.PLAYING)
        self.sender_pipeline.set_state(Gst.State.PLAYING)
        GLib.idle_add(self.on_negotiation, self.sender)
        GLib.timeout_add_seconds(self.args.timeout, self.timeout)
        try:
            self.loop.run()
        finally:
            self.sender_pipeline.set_state(Gst.State.NULL)
            self.receiver_pipeline.set_state(Gst.State.NULL)
            self.frame_file.close()
        if self.failed is not None:
            return 1
        if self.received_access_units != self.args.frames:
            print(
                "FAIL  received access-unit count changed: "
                f"{self.received_access_units} != {self.args.frames}",
                file=sys.stderr,
            )
            return 1
        return 0


def positive_integer(value):
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--width", type=positive_integer, required=True)
    parser.add_argument("--height", type=positive_integer, required=True)
    parser.add_argument("--frames", type=positive_integer, required=True)
    parser.add_argument("--fps", type=positive_integer, required=True)
    parser.add_argument("--mtu", type=positive_integer, required=True)
    parser.add_argument("--timeout", type=positive_integer, default=90)
    parser.add_argument(
        "--encoder",
        choices=("vah264enc", "openh264enc"),
        default="vah264enc",
        help="encoder used by the peer; openh264enc is transport-only",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    Gst.init(None)
    try:
        gate = PeerGate(args)
    except (GLib.Error, RuntimeError) as error:
        print(f"FAIL  could not create WebRTC peer gate: {error}", file=sys.stderr)
        return 1
    return gate.run()


if __name__ == "__main__":
    sys.exit(main())
