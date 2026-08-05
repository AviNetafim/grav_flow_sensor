"""Tkinter GUI for the gravimetric flow sensor test bench."""

import tkinter as tk
import tkinter.font as tkfont
import time
from tkinter import ttk

from tcp_com import TcpCom


ESP32_IP = "192.168.4.1"
PORT = 502
SOCKET_TIMEOUT = 3


class FlowSensorGUI:
    """Main application window for configuring and displaying a flow test."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Gravimetric Flow Sensor Test Bench")
        self.root.geometry("800x700")
        self.root.minsize(600, 500)
        self.root.resizable(True, True)
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        self.tc = TcpCom(ESP32_IP, PORT, SOCKET_TIMEOUT)
        self.tc.print_com = False

        self._configure_styles()

        self.test_method = tk.StringVar(value="weight")
        self.weight_target = tk.StringVar()
        self.vessel_volume = tk.StringVar()
        self.weight_timeout = tk.StringVar()
        self.volume_timeout = tk.StringVar()

        self.test_time = tk.StringVar(value="--")
        self.flow = tk.StringVar(value="--")
        self.error_message = tk.StringVar()

        self._build_ui()
        self._update_method_fields()

    def _configure_styles(self) -> None:
        """Increase widget text and padding for a larger, clearer interface."""
        for font_name in ("TkDefaultFont", "TkTextFont"):
            font = tkfont.nametofont(font_name)
            font.configure(size=max(12, round(font.cget("size") * 1.3)))

        style = ttk.Style(self.root)
        style.configure("TButton", padding=(12, 9))
        style.configure("TRadiobutton", padding=(6, 5))
        style.configure("TEntry", padding=(6, 5))
        style.configure("TLabelframe", padding=6)

    def _build_ui(self) -> None:
        container = ttk.Frame(self.root, padding=16)
        container.grid(row=0, column=0, sticky="nsew")
        container.columnconfigure(0, weight=1)
        container.columnconfigure(1, weight=1)
        container.columnconfigure(2, weight=1)
        container.rowconfigure(0, weight=1)
        container.rowconfigure(1, weight=4)
        container.rowconfigure(2, weight=3)
        container.rowconfigure(3, weight=1)

        method_frame = ttk.LabelFrame(container, text="Test method", padding=10)
        method_frame.grid(
            row=0, column=0, columnspan=3, sticky="nsew", pady=(0, 12)
        )
        method_frame.columnconfigure(0, weight=1)
        method_frame.columnconfigure(1, weight=1)
        method_frame.rowconfigure(0, weight=1)

        ttk.Radiobutton(
            method_frame,
            text="Weight",
            value="weight",
            variable=self.test_method,
            command=self._update_method_fields,
        ).grid(row=0, column=0, padx=(0, 20))
        ttk.Radiobutton(
            method_frame,
            text="Volume",
            value="volume",
            variable=self.test_method,
            command=self._update_method_fields,
        ).grid(row=0, column=1)

        inputs = ttk.LabelFrame(container, text="Test settings", padding=10)
        inputs.grid(row=1, column=0, columnspan=3, sticky="nsew", pady=(0, 12))
        inputs.columnconfigure(3, weight=1)
        for row in range(4):
            inputs.rowconfigure(row, weight=1)

        ttk.Label(inputs, text="Weight target:").grid(
            row=0, column=0, sticky="w", pady=4
        )
        self.weight_entry = ttk.Entry(inputs, textvariable=self.weight_target, width=16)
        self.weight_entry.grid(row=0, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(inputs, text="g (0–80)").grid(row=0, column=2, sticky="w")

        ttk.Label(inputs, text="Vessel volume:").grid(row=1, column=0, sticky="w", pady=4)
        self.volume_entry = ttk.Entry(inputs, textvariable=self.vessel_volume, width=16)
        self.volume_entry.grid(row=1, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(inputs, text="cc (0–100)").grid(row=1, column=2, sticky="w")

        ttk.Label(inputs, text="Weight timeout:").grid(row=2, column=0, sticky="w", pady=4)
        self.weight_timeout_entry = ttk.Entry(inputs, textvariable=self.weight_timeout, width=16)
        self.weight_timeout_entry.grid(row=2, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(inputs, text="ds (1–999)").grid(row=2, column=2, sticky="w")

        ttk.Label(inputs, text="Volume timeout:").grid(row=3, column=0, sticky="w", pady=4)
        self.volume_timeout_entry = ttk.Entry(inputs, textvariable=self.volume_timeout, width=16)
        self.volume_timeout_entry.grid(row=3, column=1, padx=8, pady=4, sticky="w")
        ttk.Label(inputs, text="ds (1–999)").grid(row=3, column=2, sticky="w")

        results = ttk.LabelFrame(container, text="Test results", padding=10)
        results.grid(row=2, column=0, columnspan=3, sticky="nsew", pady=(0, 12))
        results.columnconfigure(3, weight=1)
        for row in range(3):
            results.rowconfigure(row, weight=1)

        self._add_display_row(results, 0, "Test time:", self.test_time, "ds")
        self._add_display_row(results, 1, "Flow:", self.flow, "")
        self._add_display_row(
            results, 2, "Error message:", self.error_message, "", width=32
        )

        ttk.Button(container, text="Start Test", command=self.run_test).grid(row=3, column=0, padx=(0, 8), sticky="nsew")
        ttk.Button(container, text="Get Samples", command=self.get_samples).grid(row=3, column=1, sticky="nsew")
        ttk.Button(container, text="Exit", command=self.root.destroy).grid(row=3, column=2, padx=(8, 0), sticky="nsew")

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
        weight_selected = self.test_method.get() == "weight"
        self.weight_entry.configure(state="normal" if weight_selected else "disabled")
        self.weight_timeout_entry.configure(state="normal" if weight_selected else "disabled")
        self.volume_entry.configure(state="disabled" if weight_selected else "normal")
        self.volume_timeout_entry.configure(state="disabled" if weight_selected else "normal")
        self.error_message.set("")

    def _get_active_settings(self) -> tuple[float, int] | None:
        """Validate and return the selected target and timeout."""
        is_weight = self.test_method.get() == "weight"
        target_text = self.weight_target.get() if is_weight else self.vessel_volume.get()
        timeout_text = (self.weight_timeout.get() if is_weight else self.volume_timeout.get())
        target_name = "Weight target" if is_weight else "Vessel volume"
        target_limit = 80.0 if is_weight else 100.0

        try:
            target = float(target_text)
        except ValueError:
            self.error_message.set(f"{target_name} must be a number.")
            return None

        try:
            timeout = int(timeout_text)
        except ValueError:
            self.error_message.set("Timeout must be an integer.")
            return None

        if not 0 < target < target_limit:
            self.error_message.set(f"{target_name} must be greater than 0 and less than {target_limit:g}.")
            return None
        if not 0 < timeout < 1000:
            self.error_message.set("Timeout must be greater than 0 and less than 1000 ds.")
            return None

        self.error_message.set("")
        return target, timeout

    def update_test_result(self, elapsed_time: int) -> None:
        """Update displays with a time value supplied by the sensor client."""
        settings = self._get_active_settings()
        if settings is None:
            return

        target, timeout = settings
        if elapsed_time >= timeout:
            self.test_time.set(str(elapsed_time))
            self.flow.set("--")
            self.error_message.set("Test timed out.")
            return

        self.test_time.set(str(elapsed_time))
        self.flow.set(f"{elapsed_time / target:.3f}")
        self.error_message.set("")

    def run_test(self) -> None:
        """Start a test. Add flow-sensor client code here."""
        settings = self._get_active_settings()
        if settings is None:
            return
        target, _ = settings
        mode = 0 if self.test_method.get() == "weight" else 1
        error_code = self.tc.write(
            rtu=1, address=0, index=0, size=1, payload=[mode]
        )
        if error_code != 0:
            self.error_message.set("Error while setting test mode on controller.")
            return

        if mode == 0:                                                               # weight mode
            target_10mg = round(target * 100)
            print(f"Setting weight target to {target_10mg} (10 mg units)")
            error_code = self.tc.write(
                rtu=1, address=1, index=0, size=1, payload=[target_10mg]
            )
            if error_code != 0:
                self.error_message.set(
                    "Error while setting weight target on controller."
                )
                return

        error_code = self.tc.write(
            rtu=1, address=2, index=0, size=1, payload=[1]
        )
        if error_code != 0:
            self.error_message.set("Error while starting test on controller.")
            return

        if mode == 1:
            _, timeout = settings
            deadline = time.monotonic() + timeout / 10
            self.root.after(
                1000, self._poll_volume_test, target, timeout, deadline
            )

    def _poll_volume_test(
        self, volume: float, timeout: int, deadline: float
    ) -> None:
        if time.monotonic() >= deadline:
            self.error_message.set("volume test timeout")
            return

        error_code, payload = self.tc.read(
            rtu=1, address=3, index=0, size=2
        )
        if error_code != 0 or len(payload) < 2:
            self.error_message.set("Error while reading volume test state.")
            return

        if payload[0] != 0:
            self.root.after(
                1000, self._poll_volume_test, volume, timeout, deadline
            )
            return

        time_to_fill = payload[1]
        self.test_time.set(str(time_to_fill))
        if time_to_fill <= 0 or time_to_fill >= timeout:
            self.flow.set("--")
            self.error_message.set("volume test timeout")
            return

        self.flow.set(f"{volume / time_to_fill * 36:.3f}")
        self.error_message.set("")

    def get_samples(self) -> None:
        """Get sensor samples. Add flow-sensor client code here."""
        # TODO: Request samples from the flow-sensor client.
        pass


def main() -> None:
    root = tk.Tk()
    FlowSensorGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
