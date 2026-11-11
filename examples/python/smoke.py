#!/usr/bin/env python3
"""Optional real-runtime smoke test; requires Pillow, the SDK, and a local runtime.

No vendor files are downloaded. Timings describe this invocation, not a benchmark
guarantee. The Linux-only recovery check kills only descendants of this process.
"""

import argparse
import ctypes as c
import json
import math
import os
import pathlib
import signal
import statistics
import tempfile
import threading
import time

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as error:
    raise SystemExit("Install the optional smoke-test dependency: python -m pip install Pillow") from error


class Options(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("abi_version", c.c_uint32),
                ("runtime_dir", c.c_char_p), ("ocr_path", c.c_char_p),
                ("worker_path", c.c_char_p), ("temp_dir", c.c_char_p),
                ("protocol", c.c_uint32), ("startup_timeout_ms", c.c_uint32),
                ("max_pending_requests", c.c_uint32)]


class View(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("format", c.c_uint32),
                ("data", c.POINTER(c.c_uint8)), ("data_size", c.c_uint64),
                ("width", c.c_uint32), ("height", c.c_uint32), ("stride", c.c_uint64)]


class Line(c.Structure):
    _fields_ = [("struct_size", c.c_uint32), ("has_confidence", c.c_uint32),
                ("quad", c.c_float * 8), ("confidence", c.c_float),
                ("text", c.c_char_p), ("text_length", c.c_uint64)]


def load_sdk(filename):
    sdk = c.CDLL(str(filename))
    sdk.wco_version.restype = c.c_char_p
    sdk.wco_last_error.restype = c.c_char_p
    sdk.wco_create.argtypes = [c.POINTER(Options), c.POINTER(c.c_void_p)]
    sdk.wco_create.restype = c.c_int32
    sdk.wco_recognize.argtypes = [c.c_void_p, c.POINTER(View), c.c_uint32, c.POINTER(c.c_void_p)]
    sdk.wco_recognize.restype = c.c_int32
    sdk.wco_result_line_count.argtypes = [c.c_void_p]
    sdk.wco_result_line_count.restype = c.c_uint32
    sdk.wco_result_get_line.argtypes = [c.c_void_p, c.c_uint32, c.POINTER(Line)]
    sdk.wco_result_get_line.restype = c.c_int32
    sdk.wco_result_destroy.argtypes = [c.c_void_p]
    sdk.wco_result_destroy.restype = None
    sdk.wco_destroy.argtypes = [c.c_void_p]
    sdk.wco_destroy.restype = None
    return sdk


def choose_font(explicit):
    candidates = [explicit] if explicit else [
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        r"C:\Windows\Fonts\msyh.ttc",
        r"C:\Windows\Fonts\simsun.ttc",
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).is_file():
            return candidate
    raise SystemExit("A Chinese-capable font is required; pass --font /path/to/font.ttf")


def text_image(size, font_path):
    width, height = size
    image = Image.new("RGB", size, "white")
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(font_path, max(24, min(50, width // 25)))
    draw.text((20, 10), "MaaFramework OCR 12345", font=font, fill="black")
    draw.text((20, height // 2), "微信文字识别测试", font=font, fill="black")
    return image


def image_view(image, pixel_format, padded):
    channels = 1 if pixel_format == 4 else 4 if pixel_format == 2 else 3
    image_mode = "L" if pixel_format == 4 else "RGBA" if pixel_format == 2 else "RGB"
    raw_mode = {1: "BGR", 2: "BGRA", 3: "RGB", 4: "L"}[pixel_format]
    converted = image.convert(image_mode)
    x_offset, y_offset = (13, 7) if padded else (0, 0)
    if padded:
        backing = Image.new(image_mode, (image.width + 31, image.height + 17), "white")
        backing.paste(converted, (x_offset, y_offset))
    else:
        backing = converted
    raw = backing.tobytes("raw", raw_mode)
    memory = (c.c_uint8 * len(raw)).from_buffer_copy(raw)
    stride = backing.width * channels
    offset = y_offset * stride + x_offset * channels
    pixels = c.cast(c.byref(memory, offset), c.POINTER(c.c_uint8))
    view = View(c.sizeof(View), pixel_format, pixels, len(raw) - offset,
                image.width, image.height, stride)
    return memory, view


def recognize(sdk, engine, image, pixel_format=3, padded=False, timeout_ms=15000):
    memory, view = image_view(image, pixel_format, padded)
    result = c.c_void_p()
    started = time.perf_counter()
    status = sdk.wco_recognize(engine, c.byref(view), timeout_ms, c.byref(result))
    elapsed_ms = (time.perf_counter() - started) * 1000
    error = sdk.wco_last_error().decode("utf-8", errors="replace")
    lines = []
    try:
        if status == 0:
            for index in range(sdk.wco_result_line_count(result)):
                line = Line()
                line.struct_size = c.sizeof(Line)
                line_status = sdk.wco_result_get_line(result, index, c.byref(line))
                if line_status:
                    raise RuntimeError(f"result getter failed: {line_status}")
                lines.append({"text": c.string_at(line.text, line.text_length).decode("utf-8"),
                              "quad": list(line.quad), "confidence": line.confidence,
                              "has_confidence": bool(line.has_confidence)})
    finally:
        sdk.wco_result_destroy(result)
    # Keep the ctypes backing buffer alive through the synchronous invocation.
    del memory
    return {"status": status, "error": error, "elapsed_ms": round(elapsed_ms, 3),
            "width": image.width, "height": image.height, "format": pixel_format,
            "padded_roi": padded, "lines": lines}


def descendants(parent):
    """Linux process identities, scoped to descendants of the invoking script."""
    if not pathlib.Path("/proc").is_dir():
        return {}
    result = {}
    pending = [parent]
    for ancestor in pending:
        for item in pathlib.Path("/proc").iterdir():
            if not item.name.isdigit():
                continue
            try:
                fields = (item / "stat").read_text().rsplit(")", 1)[1].split()
                if int(fields[1]) != ancestor or fields[0] == "Z":
                    continue
                pid = int(item.name)
                result[pid] = {"name": (item / "comm").read_text().strip(), "parent": ancestor,
                               "start_time": fields[19]}
                pending.append(pid)
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                continue
    return result


def still_running(identities):
    running = []
    for pid, identity in identities.items():
        try:
            fields = pathlib.Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            if fields[0] != "Z" and fields[19] == identity["start_time"]:
                running.append(pid)
        except (FileNotFoundError, ProcessLookupError):
            continue
    return sorted(running)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=pathlib.Path, required=True)
    parser.add_argument("--runtime-dir", type=pathlib.Path, required=True)
    parser.add_argument("--ocr", type=pathlib.Path, help="Defaults to runtime-dir/wxocr on Linux")
    parser.add_argument("--worker", type=pathlib.Path)
    parser.add_argument("--font")
    parser.add_argument("--iterations", type=int, default=30)
    parser.add_argument("--protocol", type=int, choices=(0, 3, 4), default=4)
    parser.add_argument("--timeout-ms", type=int, default=15000)
    parser.add_argument("--exercise-recovery", action="store_true",
                        help="Linux: test worker/vendor crash and an in-flight deadline, then verify recovery")
    parser.add_argument("--output", type=pathlib.Path, help="Optional JSON report filename")
    args = parser.parse_args()
    if args.iterations < 1 or args.timeout_ms < 1:
        parser.error("iterations and timeout must be positive")
    if args.exercise_recovery and os.name != "posix":
        parser.error("--exercise-recovery is currently implemented only on Linux")
    runtime = args.runtime_dir.resolve()
    ocr = args.ocr.resolve() if args.ocr else runtime / "wxocr"
    sdk = load_sdk(args.sdk.resolve())
    font_path = choose_font(args.font)
    report = {"sdk_version": sdk.wco_version().decode(), "runtime_dir": str(runtime),
              "ocr_path": str(ocr), "iterations": args.iterations, "calls": []}
    engine = c.c_void_p()
    failed = False
    tracked_processes = {}
    with tempfile.TemporaryDirectory(prefix="wechatocr-smoke-") as temporary:
        paths = [os.fsencode(runtime), os.fsencode(ocr),
                 os.fsencode(args.worker.resolve()) if args.worker else None, os.fsencode(temporary)]
        options = Options(c.sizeof(Options), 1, *paths, args.protocol, args.timeout_ms, 16)
        started = time.perf_counter()
        create_status = sdk.wco_create(c.byref(options), c.byref(engine))
        report["create_status"] = create_status
        report["create_ms"] = round((time.perf_counter() - started) * 1000, 3)
        report["create_error"] = sdk.wco_last_error().decode("utf-8", errors="replace")
        try:
            if create_status:
                raise RuntimeError(f"engine creation failed: {report['create_error']}")
            tracked_processes.update(descendants(os.getpid()))
            sizes = [(1200, 240), (900, 200), (1400, 280), (640, 180)]
            for index in range(args.iterations):
                image = text_image(sizes[index % len(sizes)], font_path)
                call = recognize(sdk, engine, image, (index % 4) + 1, bool(index % 2), args.timeout_ms)
                text = "\n".join(line["text"] for line in call["lines"])
                call["expected_text_found"] = all(token in text for token in ("MaaFramework", "12345", "微信文字识别测试"))
                call["temporary_files_after_call"] = len(list(pathlib.Path(temporary).rglob("*.png")))
                report["calls"].append(call)
                if call["status"] or not call["expected_text_found"] or call["temporary_files_after_call"]:
                    failed = True
                print(f"{index + 1}/{args.iterations}: status={call['status']} {call['elapsed_ms']:.1f}ms text={text!r}", flush=True)
            blank = recognize(sdk, engine, Image.new("RGB", (600, 160), "white"), timeout_ms=args.timeout_ms)
            report["blank"] = blank
            if blank["status"] or blank["lines"]:
                failed = True
            if args.exercise_recovery:
                old_tree = descendants(os.getpid())
                tracked_processes.update(old_tree)
                workers = [pid for pid, info in old_tree.items() if info["name"] == "wechatocr-worke" or info["name"] == "wechatocr-worker"]
                if len(workers) != 1:
                    raise RuntimeError(f"expected one private worker, found {workers}")
                os.kill(workers[0], signal.SIGKILL)
                broken = recognize(sdk, engine, text_image(sizes[0], font_path), timeout_ms=args.timeout_ms)
                recovered = recognize(sdk, engine, text_image(sizes[0], font_path), timeout_ms=args.timeout_ms)
                remaining = descendants(os.getpid())
                tracked_processes.update(remaining)
                report["recovery"] = {"old_tree": old_tree, "first_call": broken,
                                      "second_call": recovered, "old_pids_still_running": still_running(old_tree)}
                if broken["status"] == 0 or recovered["status"] or report["recovery"]["old_pids_still_running"]:
                    failed = True
                vendor_tree = descendants(os.getpid())
                tracked_processes.update(vendor_tree)
                vendors = [pid for pid, info in vendor_tree.items() if info["name"] == "wxocr"]
                if len(vendors) != 1:
                    raise RuntimeError(f"expected one vendor OCR child, found {vendors}")
                os.kill(vendors[0], signal.SIGKILL)
                vendor_failed = recognize(sdk, engine, text_image(sizes[0], font_path), timeout_ms=args.timeout_ms)
                vendor_recovered = recognize(sdk, engine, text_image(sizes[0], font_path), timeout_ms=args.timeout_ms)
                tracked_processes.update(descendants(os.getpid()))
                report["vendor_crash_recovery"] = {"first_call": vendor_failed,
                                                    "second_call": vendor_recovered,
                                                    "old_pids_still_running": still_running(vendor_tree)}
                if vendor_failed["status"] != 5 or vendor_recovered["status"] or still_running(vendor_tree):
                    failed = True
                timeout_tree = descendants(os.getpid())
                tracked_processes.update(timeout_tree)
                timeout_vendors = [pid for pid, info in timeout_tree.items() if info["name"] == "wxocr"]
                if len(timeout_vendors) != 1:
                    raise RuntimeError(f"expected one vendor OCR child before timeout, found {timeout_vendors}")
                # Pause only our vendor child so the real request remains pending
                # after its PNG is created, regardless of inference speed.
                os.kill(timeout_vendors[0], signal.SIGSTOP)
                observed_png = []
                monitoring_done = threading.Event()

                def monitor_temporary_png():
                    while not monitoring_done.wait(.001):
                        if list(pathlib.Path(temporary).rglob("*.png")):
                            observed_png.append(True)

                observer = threading.Thread(target=monitor_temporary_png)
                observer.start()
                try:
                    timed_out = recognize(sdk, engine, text_image(sizes[0], font_path), timeout_ms=500)
                finally:
                    monitoring_done.set()
                    observer.join()
                png_after_timeout = len(list(pathlib.Path(temporary).rglob("*.png")))
                timeout_recovered = recognize(sdk, engine, text_image(sizes[0], font_path), timeout_ms=args.timeout_ms)
                tracked_processes.update(descendants(os.getpid()))
                report["deadline_recovery"] = {"timed_out_call": timed_out,
                                                "temporary_png_observed_in_flight": bool(observed_png),
                                                "temporary_png_before_retry": png_after_timeout,
                                                "next_call": timeout_recovered,
                                                "old_pids_still_running": still_running(timeout_tree),
                                                "temporary_png_after_recovery": len(list(pathlib.Path(temporary).rglob("*.png")))}
                if timed_out["status"] != 6 or not observed_png or png_after_timeout or timeout_recovered["status"] or still_running(timeout_tree):
                    failed = True
        except Exception as error:
            failed = True
            report["exception"] = str(error)
        finally:
            sdk.wco_destroy(engine)
        report["temporary_files_after_destroy"] = len(list(pathlib.Path(temporary).rglob("*")))
        report["descendants_after_destroy"] = descendants(os.getpid())
        report["tracked_pids_still_running_after_destroy"] = still_running(tracked_processes)
        if report["temporary_files_after_destroy"] or report["descendants_after_destroy"] or report["tracked_pids_still_running_after_destroy"]:
            failed = True
    timings = sorted(call["elapsed_ms"] for call in report["calls"])
    if timings:
        report["all_calls_p50_ms"] = round(statistics.median(timings), 3)
        report["all_calls_p95_ms"] = timings[max(0, math.ceil(len(timings) * .95) - 1)]
        warm = sorted(call["elapsed_ms"] for call in report["calls"][1:])
        if warm:
            report["warm_calls_p50_ms"] = round(statistics.median(warm), 3)
            report["warm_calls_p95_ms"] = warm[max(0, math.ceil(len(warm) * .95) - 1)]
    report["passed"] = not failed
    encoded = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
