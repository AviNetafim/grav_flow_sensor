"""Tkinter GUI for the gravimetric flow sensor test bench."""

import csv
import json
import math
import tkinter as tk
import tkinter.font as tkfont
import time
from pathlib import Path
from tkinter import ttk

from tcp_com import TcpCom


ESP32_IP = "192.168.4.1"
PORT = 502
SOCKET_TIMEOUT = 3
REG_SAMPLES = 32
SAMPLES_REGISTER_ADDRESS = 5                                    
SAMPLES_POINTER_ADDRESS = 6
CONFIG_REGISTER_ADDRESS = 9
CONFIG_FILE = Path(__file__).with_name("flow_sensor_config.json")
GUI_TIMEOUT_MARGIN_DS = 20


class FlowSensorGUI:
    """Main application window for configuring and displaying a flow test."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Gravimetric Flow Sensor Test Bench")
        self.root.geometry("900x640")
        self.root.minsize(640, 540)
        self.root.resizable(True, True)
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        self.tc = TcpCom(ESP32_IP, PORT, SOCKET_TIMEOUT)
        self.tc.print_com = False

        self._configure_styles()

        self.test_method = tk.IntVar(value=0)
        self.weight_target = tk.StringVar()
        self.flow_range_min = tk.StringVar()
        self.config: dict[str, object] = {}
        self.controller_config: list[int] = []

        self.test_time = tk.StringVar(value="--")
        self.test_progress_time = tk.StringVar(value="--")
        self.flow = tk.StringVar(value="--")
        self.raw_weight = tk.StringVar(value="--")
        self.error_message = tk.StringVar()
        self.weight_count = 0
        self._test_generation = 0
        self._test_active = False

        self._build_ui()
        self.load_configuration()
        self._update_method_fields()

    def _configure_styles(self) -> None:
        """Use compact fonts and padding suitable for a laptop display."""
        for font_name in ("TkDefaultFont", "TkTextFont"):
            font = tkfont.nametofont(font_name)
            font.configure(size=10)

        style = ttk.Style(self.root)
        style.configure("TButton", padding=(8, 5))
        style.configure("TRadiobutton", padding=(4, 3))
        style.configure("TEntry", padding=(4, 3))
        style.configure("TLabelframe", padding=4)

    def _build_ui(self) -> None:
        container = ttk.Frame(self.root, padding=10)
        container.grid(row=0, column=0, sticky="nsew")
        container.columnconfigure(0, weight=1)
        container.columnconfigure(1, weight=1)
        container.columnconfigure(2, weight=1)
        container.columnconfigure(3, weight=1)
        container.rowconfigure(0, weight=1)
        container.rowconfigure(1, weight=4)
        container.rowconfigure(2, weight=3)
        container.rowconfigure(3, weight=1)

        method_frame = ttk.LabelFrame(container, text="Test method", padding=6)
        method_frame.grid(row=0, column=0, columnspan=4, sticky="nsew", pady=(0, 8))
        method_frame.columnconfigure(0, weight=1)
        method_frame.columnconfigure(1, weight=1)
        method_frame.columnconfigure(2, weight=1)
        method_frame.rowconfigure(0, weight=1)

        ttk.Radiobutton(
            method_frame,
            text="Weight",
            value=0,
            variable=self.test_method,
            command=self._update_method_fields,
        ).grid(row=0, column=0, padx=(0, 20))
        ttk.Radiobutton(
            method_frame,
            text="Volume",
            value=1,
            variable=self.test_method,
            command=self._update_method_fields,
        ).grid(row=0, column=1)
        ttk.Radiobutton(
            method_frame,
            text="Calibrate",
            value=2,
            variable=self.test_method,
            command=self._update_method_fields,
        ).grid(row=0, column=2, padx=(20, 0))

        inputs = ttk.LabelFrame(container, text="Test settings", padding=6)
        inputs.grid(row=1, column=0, columnspan=4, sticky="nsew", pady=(0, 8))
        inputs.columnconfigure(3, weight=1)
        for row in range(2):
            inputs.rowconfigure(row, weight=1)

        ttk.Label(inputs, text="Weight target:").grid(row=0, column=0, sticky="w", pady=4)
        self.weight_entry = ttk.Entry(inputs, textvariable=self.weight_target, width=16)
        self.weight_entry.grid(row=0, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(inputs, text="g (0-80)").grid(row=0, column=2, sticky="w")

        ttk.Label(inputs, text="Flow range min:").grid(row=1, column=0, sticky="w", pady=4)
        self.flow_min_entry = ttk.Entry(inputs, textvariable=self.flow_range_min, width=16)
        self.flow_min_entry.grid(row=1, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(inputs, text="l/h (>0)").grid(row=1, column=2, sticky="w")

        results = ttk.LabelFrame(container, text="Test results", padding=6)
        results.grid(row=2, column=0, columnspan=4, sticky="nsew", pady=(0, 8))
        results.columnconfigure(3, weight=1)
        for row in range(5):
            results.rowconfigure(row, weight=1)

        self._add_display_row(results, 0, "Test time:", self.test_time, "ds")
        self._add_display_row(
            results, 1, "Test progress time:", self.test_progress_time, "s"
        )
        self._add_display_row(results, 2, "Flow:", self.flow, "")
        self._add_display_row(results, 3, "Raw weight:", self.raw_weight, "")
        self._add_display_row(
            results, 4, "Error message:", self.error_message, "", width=32
        )

        buttons = ttk.Frame(container)
        buttons.grid(row=3, column=0, columnspan=4, sticky="nsew")
        for column in range(4):
            buttons.columnconfigure(column, weight=1)
        ttk.Button(buttons, text="Start Test", command=self.run_test).grid(row=0, column=0, padx=(0, 8), sticky="nsew")
        self.get_samples_button = ttk.Button(
            buttons, text="Get Samples", command=self.get_samples, state="disabled"
        )
        self.get_samples_button.grid(row=0, column=1, sticky="nsew")
        ttk.Button(buttons, text="Load Configuration", command=self.load_configuration).grid(row=0, column=2, padx=(8, 0), sticky="nsew")
        ttk.Button(buttons, text="Exit", command=self.root.destroy).grid(row=0, column=3, padx=(8, 0), sticky="nsew")

    @staticmethod
    def _add_display_row(
        parent: ttk.LabelFrame,
        row: int,
        label: str,
        variable: tk.StringVar,
        unit: str,
        width: int = 16,
    ) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=4)
        ttk.Entry(
            parent, textvariable=variable, state="readonly", width=width
        ).grid(row=row, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(parent, text=unit).grid(row=row, column=2, sticky="w")

    def _update_method_fields(self) -> None:
        weight_selected = self.test_method.get() == 0
        self.weight_entry.configure(state="normal" if weight_selected else "disabled")
        range_selected = self.test_method.get() in (0, 1)
        self.flow_min_entry.configure(state="normal" if range_selected else "disabled")
        self.error_message.set("")

    @staticmethod
    def _timeout_ds(amount: float, min_flow_lph: float) -> int:
        """Convert water cc/g at l/h to ds, rounded up to whole seconds."""
        return math.ceil(amount * 3.6 / min_flow_lph) * 10

    def load_configuration(self) -> None:
        """Load and validate configuration from disk."""
        try:
            with CONFIG_FILE.open(encoding="utf-8") as config_file:
                config = json.load(config_file)
            volume_0 = float(config["volume_0"])
            volume = float(config["volume"])
            stable_time = int(config["stable_time"])
            cal_div = int(config["cal_div"])
            cal_offset = int(config["cal_offset"])
            samples_file = str(config["samples_file"]).strip()
            flags = [int(config[name]) for name in ("com_prt", "show_sample", "show_regs")]
            if volume_0 <= 0 or volume <= 0 or not 0 <= stable_time <= 65535:
                raise ValueError("volumes must be positive and stable_time must fit uint16")
            if not 0 < cal_div <= 65535 or not -32768 <= cal_offset <= 32767:
                raise ValueError("cal_div or cal_offset exceeds its register range")
            if not samples_file or Path(samples_file).suffix or Path(samples_file).name != samples_file:
                raise ValueError("samples_file must not include an extension")
            if any(flag not in (0, 1) for flag in flags):
                raise ValueError("configuration flags must be 0 or 1")
        except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
            self.error_message.set(f"Configuration load error: {exc}")
            return

        self.config = config
        static_values = [
            stable_time,
            cal_div,
            cal_offset & 0xFFFF,
            *flags,
        ]
        error_code = self.tc.write(
            rtu=1,
            address=CONFIG_REGISTER_ADDRESS,
            index=3,
            size=len(static_values),
            payload=static_values,
        )
        if error_code != 0:
            self.error_message.set("Configuration loaded, but controller update failed.")
            return
        self.error_message.set("")

    def _get_active_settings(self) -> tuple[float, int] | None:
        """Validate inputs and return the measured amount and timeout."""
        is_weight = self.test_method.get() == 0
        target_text = self.weight_target.get() if is_weight else str(self.config.get("volume", ""))
        target_name = "Weight target" if is_weight else "Configured volume"
        target_limit = 80.0 if is_weight else 65535.0

        try:
            target = float(target_text)
        except ValueError:
            self.error_message.set(f"{target_name} must be a number.")
            return None

        try:
            min_flow = float(self.flow_range_min.get())
        except ValueError:
            self.error_message.set("Flow range min must be a number.")
            return None

        if not 0 < target < target_limit:
            self.error_message.set(f"{target_name} must be greater than 0 and less than {target_limit:g}.")
            return None
        if min_flow <= 0:
            self.error_message.set("Flow range min must be greater than 0.")
            return None
        timeout = self._timeout_ds(target, min_flow)
        if timeout > 65535:
            self.error_message.set("Calculated timeout exceeds the controller limit.")
            return None

        self.error_message.set("")
        return target, timeout

    def _build_controller_config(self) -> list[int] | None:
        try:
            min_flow = float(self.flow_range_min.get())
            weight_text = self.weight_target.get().strip()
            weight = float(weight_text) if weight_text else 0.0
            if min_flow <= 0 or weight < 0:
                raise ValueError("flow range min must be positive")
            values = [
                self._timeout_ds(float(self.config["volume_0"]), min_flow),
                self._timeout_ds(float(self.config["volume"]), min_flow),
                self._timeout_ds(weight, min_flow) if weight else 0,
                int(self.config["stable_time"]),
                int(self.config["cal_div"]),
                int(self.config["cal_offset"]) & 0xFFFF,
                int(self.config["com_prt"]),
                int(self.config["show_sample"]),
                int(self.config["show_regs"]),
            ]
            if any(value < 0 or value > 65535 for value in values):
                raise ValueError("a value exceeds the controller register limit")
        except (KeyError, TypeError, ValueError) as exc:
            self.error_message.set(f"Configuration error: {exc}")
            return None
        return values

    def _update_controller_configuration(self) -> bool:
        """Write the nine active words of the controller configuration register."""
        values = self._build_controller_config()
        if values is None:
            return False
        error_code = self.tc.write(
            rtu=1, address=CONFIG_REGISTER_ADDRESS, index=0, size=9, payload=values
        )
        if error_code != 0:
            self.error_message.set("Error while downloading controller configuration.")
            return False
        self.controller_config = values
        self.error_message.set("")
        return True

    def run_test(self) -> None:
        """Start a test. Add flow-sensor client code here."""
        mode = self.test_method.get()                                       # get test mode (0=weight, 1=volume, 2=calibrate)
        if mode == 2:
            target, timeout = 0.0, 0
        else:
            settings = self._get_active_settings()
            if settings is None:
                return
            target, timeout = settings
            if not self._update_controller_configuration():
                return
        self._test_generation += 1
        test_generation = self._test_generation
        self._test_active = False
        self.weight_count = 0
        self.get_samples_button.configure(state="disabled")
        self.test_time.set("--")
        self.test_progress_time.set("0.0")
        self.flow.set("--")
        self.raw_weight.set("--")
        self.error_message.set("")
        error_code = self.tc.write(                                                 # set test mode on controller        
            rtu=1, address=0, index=0, size=1, payload=[mode]
        )
        if error_code != 0:
            self.error_message.set("Error while setting test mode on controller.")
            return

        if mode == 0:                                                               # weight mode
            target_10mg = round(target * 100)
            print(f"Setting weight target to {target_10mg} (10 mg units)")
            error_code = self.tc.write(                                             # write weight target to controller
                rtu=1, address=1, index=0, size=1, payload=[target_10mg]
            )
            if error_code != 0:
                self.error_message.set(
                    "Error while setting weight target on controller."
                )
                return

        error_code = self.tc.write(                                                 # start test on controller      
            rtu=1, address=2, index=0, size=1, payload=[1]
        )
        if error_code != 0:
            self.error_message.set("Error while starting test on controller.")
            return

        if mode == 2:                                                             # calibrate mode, read raw load-cell value after 500 ms
            self.root.after(500, self._read_calibration_raw, test_generation)
            return

        self._test_active = True
        started_at = time.monotonic()
        poll_timeout = timeout + GUI_TIMEOUT_MARGIN_DS
        if mode == 1:
            poll_timeout += self.controller_config[0]
        self.root.after(
            100, self._update_test_progress, test_generation, started_at, poll_timeout
        )

        deadline = started_at + poll_timeout / 10
        if mode == 0:
            self.root.after(
                1000,
                self._poll_weight_test,
                target,
                timeout,
                deadline,
                test_generation,
            )
        else:
            self.root.after(
                1000,
                self._poll_volume_test,
                target,
                timeout,
                deadline,
                test_generation,
            )

    def _read_calibration_raw(self, test_generation: int) -> None:
        if test_generation != self._test_generation:
            return

        error_code, payload = self.tc.read(
            rtu=1, address=5, index=0, size=2
        )
        if error_code != 0 or len(payload) < 2:
            self.error_message.set("Error while reading raw load-cell value.")
            return

        raw_unsigned = payload[0] | (payload[1] << 16)
        raw_value = raw_unsigned - (1 << 32) if raw_unsigned & (1 << 31) else raw_unsigned
        self.raw_weight.set(str(raw_value))
        self.error_message.set("")

    def _update_test_progress(
        self, test_generation: int, started_at: float, timeout: int
    ) -> None:
        if test_generation != self._test_generation or not self._test_active:
            return

        elapsed_seconds = min(time.monotonic() - started_at, timeout / 10)
        self.test_progress_time.set(f"{elapsed_seconds:.1f}")
        if elapsed_seconds >= timeout / 10:
            self._test_active = False
            return
        self.root.after(
            100, self._update_test_progress, test_generation, started_at, timeout
        )

    def _poll_weight_test(
        self,
        weight: float,
        timeout: int,
        deadline: float,
        test_generation: int,
    ) -> None:
        if test_generation != self._test_generation:
            return
        if time.monotonic() >= deadline:
            self._test_active = False
            self.error_message.set("weight_timeout")
            return

        error_code, payload = self.tc.read(
            rtu=1, address=4, index=0, size=3
        )
        if error_code != 0 or len(payload) < 3:
            self._test_active = False
            self.error_message.set("Error while reading weight test state.")
            return

        if payload[0] != 0:                                                         # keep polling until test is complete (=0)
            self.root.after(
                1000,
                self._poll_weight_test,
                weight,
                timeout,
                deadline,
                test_generation,
            )
            return

        self._test_active = False
        time_to_fill = payload[1]
        self.weight_count = payload[2]
        if self.weight_count > 0:
            self.get_samples_button.configure(state="normal")
        if time_to_fill <= 0 or time_to_fill >= timeout:
            self.flow.set("--")
            self.error_message.set("weight test timeout")
            return

        self.test_time.set(str(time_to_fill))
        self.flow.set(f"{weight / time_to_fill * 36:.3f}")
        self.error_message.set("")

    def _poll_volume_test(
        self,
        volume: float,
        timeout: int,
        deadline: float,
        test_generation: int,
    ) -> None:
        if test_generation != self._test_generation:
            return
        if time.monotonic() >= deadline:
            self._test_active = False
            self.error_message.set("volume test timeout")
            return

        error_code, payload = self.tc.read(
            rtu=1, address=3, index=0, size=2
        )
        if error_code != 0 or len(payload) < 2:
            self._test_active = False
            self.error_message.set("Error while reading volume test state.")
            return

        if payload[0] != 0:
            self.root.after(
                1000,
                self._poll_volume_test,
                volume,
                timeout,
                deadline,
                test_generation,
            )
            return

        self._test_active = False
        time_to_fill = payload[1]
        self.test_time.set(str(time_to_fill))
        if time_to_fill <= 0 or time_to_fill >= timeout:
            self.flow.set("--")
            self.error_message.set("volume test timeout")
            return

        self.flow.set(f"{volume / time_to_fill * 36:.3f}")
        self.error_message.set("")

    def get_samples(self) -> None:
        """Download the completed weight-test samples and append them to CSV."""
        if self._test_active or self.weight_count <= 0:
            return

        samples: list[int] = []
        for pointer in range(0, self.weight_count, REG_SAMPLES):
            error_code = self.tc.write(
                rtu=1,
                address=SAMPLES_POINTER_ADDRESS,
                index=0,
                size=1,
                payload=[pointer],
            )
            if error_code != 0:
                self.error_message.set("Error while setting the samples pointer.")
                return

            error_code, payload = self.tc.read(
                rtu=1,
                address=SAMPLES_REGISTER_ADDRESS,
                index=0,
                size=REG_SAMPLES,
            )
            if error_code != 0 or len(payload) < REG_SAMPLES:
                self.error_message.set("Error while reading weight samples.")
                return

            remaining = self.weight_count - pointer
            samples.extend(payload[: min(REG_SAMPLES, remaining)])

        try:
            samples_path = CONFIG_FILE.with_name(f"{self.config['samples_file']}.csv")
            with samples_path.open("a", newline="", encoding="utf-8") as csv_file:
                csv.writer(csv_file).writerows((sample,) for sample in samples)
        except OSError as exc:
            self.error_message.set(f"Error while writing samples CSV: {exc}")
            return

        self.error_message.set("")


def main() -> None:
    root = tk.Tk()
    FlowSensorGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
