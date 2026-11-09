"""Command line client for the grav_sensor_flow_4 server. Runs on Linux and Windows.

python flow_client.py cal <test_weight>        start calibration, test_weight in 10 mg units (1250 = 12.5 g)
python flow_client.py w2t [samples_count]      start weight to time, 10-5000 samples
python flow_client.py w2g [weight_increment]   start weight to increment, 500-4000 x 10 mg
python flow_client.py vol                      start volume test
python flow_client.py get                      show and save the results of the last test

cal, w2t, w2g and vol join the server's wifi, start the test and disconnect without waiting
for it. The server LED is green while its wifi is ready, and blinks while a test runs. The
server turns its wifi off during cal, w2t and w2g. Run get when the LED is green again.

Every command first joins the server's wifi and waits until it is ready, for up to
WIFI_READY_TIMEOUT_S: on Windows like esp32_ap_connect.bat (netsh), on Linux with nmcli.
Both need a saved wifi profile named ESP32_AP.

An omitted w2t or w2g argument comes from samples_counts or target in flow_sensor_cfg.json,
which is read from the folder of this script (or of the exe built by PyInstaller). The csv
files are written there too. Standard library only.
"""

import argparse
import csv
import ctypes
import json
import pathlib
import re
import socket
import subprocess
import sys
import time


ESP32_IP = "192.168.4.1"
PORT = 502
SOCKET_TIMEOUT_S = 3
RTU = 1

AP_SSID = "ESP32_AP"                        # also the name of the saved wifi profile
WIFI_INTERFACE = "Wi-Fi"                    # Windows adapter name, used if netsh wlan show interfaces has no Name line
AP_SUBNET_PREFIX = "192.168.4."             # PC address given by the server's DHCP
WIFI_READY_TIMEOUT_S = 60                   # wait this long for the server's wifi, e.g. at the end of a test
JOIN_REQUEST_PERIOD_S = 5                   # time between join requests while the server's wifi is not found
NETWORK_COMMAND_TIMEOUT_S = 15
RECONNECT_TIMEOUT_S = 30                    # get repeats a read this long after the connection drops

# Folder of the script, or of the exe when built by PyInstaller: a one-file exe runs from a
# temporary folder, so __file__ is not where the exe and its json file are.
APPLICATION_DIR = pathlib.Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
CONFIG_FILE = APPLICATION_DIR / "flow_sensor_cfg.json"

MODE_ADDRESS = 0
TARGET_ADDRESS = 1
TEST_ADDRESS = 2
VOL_STAT_ADDRESS = 3
VOL_STAT_SIZE = 2                           # [test_state, time2fill_ds]
WEIGHT_STAT_ADDRESS = 4
WEIGHT_STAT_SIZE = 5                        # [test_state, value, count, samples available to read, time_ds]
SAMPLES_ADDRESS = 5
SAMPLES_POINTER_ADDRESS = 6
CONFIG_ADDRESS = 7
LAST_TEST_ADDRESS = 8
LAST_TEST_SIZE = 2                          # [mode, target]
REG_SAMPLES = 32                            # samples register size in words

MODE_WEIGHT2TIME = 0
MODE_WEIGHT2INC = 1
MODE_VOLUME = 2
MODE_CALIBRATE = 3
NO_TEST = 0xFFFF
MODE_COMMANDS = {MODE_WEIGHT2TIME: "w2t", MODE_WEIGHT2INC: "w2g", MODE_VOLUME: "vol", MODE_CALIBRATE: "cal"}
TEST_START = 1

STATE_RUNNING = 0                           # pending: no result yet
STATE_DONE = 1
STATE_NAMES = {STATE_RUNNING: "running", STATE_DONE: "done", 2: "timeout", 3: "error"}

MAX_SAMPLES = 5000                          # server samples array size
SAMPLES_PER_S = 10
MIN_SAMPLES_COUNT = 10
MIN_INCREMENT = 500
MAX_INCREMENT = 4000

FUNC_READ = 0x04
FUNC_WRITE = 0x06
CODE_INDEX = 7                              # completion code follows the 7 byte header


class ComError(Exception):
    """Connection failed or the server did not answer correctly."""


class ServerError(Exception):
    """Server answered with a non zero completion code."""


class NetworkToolError(Exception):
    """nmcli or netsh is missing or reported an error."""


class ConfigError(Exception):
    """flow_sensor_cfg.json is missing or holds a bad value."""


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_command(func, address, size, payload=()):
    """Build a command, index is always 0. Words and CRC are little-endian."""
    cmd = bytearray([RTU, func, address, 0, 0, size & 0xFF, size >> 8])
    for word in payload:
        cmd.extend([word & 0xFF, (word >> 8) & 0xFF])
    crc = crc16_ccitt_false(cmd)
    cmd.extend([crc & 0xFF, crc >> 8])
    return bytes(cmd)


class ServerConnection:
    """One TCP connection to the server. Sends every command once, never retries."""

    def __init__(self):
        self.sock = None

    def __enter__(self):
        try:
            self.sock = socket.create_connection((ESP32_IP, PORT), SOCKET_TIMEOUT_S)
        except OSError as exc:
            raise ComError(f"cannot connect to {ESP32_IP}:{PORT}: {exc}") from exc
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.sock.close()

    def _receive(self, size, received=b""):
        """Read until size bytes arrived, TCP may split the response."""
        received = bytearray(received)
        try:
            while len(received) < size:
                chunk = self.sock.recv(size - len(received))
                if not chunk:
                    raise ComError("connection closed by server")
                received.extend(chunk)
        except OSError as exc:
            raise ComError(f"no response from server: {exc}") from exc
        return bytes(received)

    def _transact(self, cmd, payload_size):
        """Send cmd, check the response and return its payload bytes."""
        try:
            self.sock.sendall(cmd)
        except OSError as exc:
            raise ComError(f"send failed: {exc}") from exc

        resp = self._receive(CODE_INDEX + 1)
        if resp[:3] != cmd[:3]:
            raise ComError(f"response does not match command: {resp.hex(' ')}")
        if resp[CODE_INDEX] != 0:                               # error response length depends on the error
            raise ServerError(f"server completion code {resp[CODE_INDEX]}")

        resp = self._receive(CODE_INDEX + 1 + payload_size + 2, resp)
        crc = crc16_ccitt_false(resp[:-2])
        if resp[-2] != crc & 0xFF or resp[-1] != crc >> 8:
            raise ComError(f"response CRC error: {resp.hex(' ')}")
        return resp[CODE_INDEX + 1:-2]

    def write_words(self, address, values):
        self._transact(build_command(FUNC_WRITE, address, len(values), values), 0)

    def write_word(self, address, value):
        self.write_words(address, [value])

    def read_words(self, address, size):
        data = self._transact(build_command(FUNC_READ, address, size), size * 2)
        return [data[i] + 256 * data[i + 1] for i in range(0, len(data), 2)]


# ----- joining the server's wifi -----

def run_network_command(*command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                timeout=NETWORK_COMMAND_TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise NetworkToolError(f"{command[0]} failed: {exc}") from exc
    if result.returncode != 0:
        raise NetworkToolError(f"{command[0]} failed: {(result.stderr or result.stdout).strip()}")
    return result.stdout


class Guid(ctypes.Structure):
    _fields_ = [("data1", ctypes.c_uint32), ("data2", ctypes.c_uint16), ("data3", ctypes.c_uint16),
                ("data4", ctypes.c_uint8 * 8)]


class WlanInterfaceInfo(ctypes.Structure):
    _fields_ = [("guid", Guid), ("description", ctypes.c_uint16 * 256), ("state", ctypes.c_int32)]


class WlanInterfaceList(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("index", ctypes.c_uint32), ("first", WlanInterfaceInfo * 1)]


def windows_request_scan():
    """Ask Windows to scan, as opening wifi settings does. Returns at once, needs no administrator prompt.

    netsh connect only finds networks of the last scan, and netsh has no rescan command, so
    without this the server's wifi is not seen soon after it returns.
    """
    try:
        wlanapi = ctypes.windll.wlanapi
    except OSError:
        return
    handle = ctypes.c_void_p()
    version = ctypes.c_uint32()
    if wlanapi.WlanOpenHandle(2, None, ctypes.byref(version), ctypes.byref(handle)) != 0:
        return
    interfaces = ctypes.POINTER(WlanInterfaceList)()
    if wlanapi.WlanEnumInterfaces(handle, None, ctypes.byref(interfaces)) == 0:
        first = ctypes.cast(interfaces.contents.first, ctypes.POINTER(WlanInterfaceInfo))
        for i in range(interfaces.contents.count):
            wlanapi.WlanScan(handle, ctypes.byref(first[i].guid), None, None, None)
        wlanapi.WlanFreeMemory(interfaces)
    wlanapi.WlanCloseHandle(handle, None)


def windows_interfaces_text():
    return run_network_command("netsh", "wlan", "show", "interfaces")


def windows_interface():
    match = re.search(r"^\s*Name\s*: (.+?)\s*$", windows_interfaces_text(), re.MULTILINE)
    return match.group(1) if match else WIFI_INTERFACE


def windows_ap_joined():
    return re.search(rf"^\s*SSID\s*: {re.escape(AP_SSID)}\s*$", windows_interfaces_text(), re.MULTILINE) is not None


def has_ap_address():
    """True if the PC has an address in the server's subnet, i.e. joined and DHCP done."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((ESP32_IP, PORT))                     # sends nothing, only selects the route
            return probe.getsockname()[0].startswith(AP_SUBNET_PREFIX)
    except OSError:
        return False


def ap_ready():
    """True if the PC is on AP_SSID and has an address from the server's DHCP."""
    return has_ap_address() and (sys.platform != "win32" or windows_ap_joined())


def request_join():
    """Ask the OS to join AP_SSID, as esp32_ap_connect.bat does on Windows. Fails while it is not visible."""
    if sys.platform == "win32":
        interface = windows_interface()
        try:                                                    # needs an administrator prompt, normally already on
            run_network_command("netsh", "wlan", "set", "autoconfig", "enabled=yes", f"interface={interface}")
        except NetworkToolError:
            pass
        windows_request_scan()
        run_network_command("netsh", "wlan", "connect", f"name={AP_SSID}", f"ssid={AP_SSID}",
                            f"interface={interface}")           # returns at once, does not wait for the join
    else:
        run_network_command("nmcli", "--wait", str(JOIN_REQUEST_PERIOD_S), "connection", "up", AP_SSID)


def show_connection():
    """Show the current connection, as the batch file does."""
    if sys.platform == "win32":
        for line in windows_interfaces_text().splitlines():
            if re.match(r"^\s*(Name|State|SSID|Signal)\s*:", line):
                print(line.rstrip())
        return
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((ESP32_IP, PORT))
        print(f"Connected to {AP_SSID}, PC address {probe.getsockname()[0]}")


def wait_for_server_wifi():
    """Join AP_SSID and wait until it is ready, for up to WIFI_READY_TIMEOUT_S. Return True if ready."""
    started = time.monotonic()
    last_request = float("-inf")
    last_error = None
    waited = False
    while True:
        try:
            if ap_ready():
                break
            if time.monotonic() - last_request >= JOIN_REQUEST_PERIOD_S:
                last_request = time.monotonic()
                request_join()
                last_error = None
        except NetworkToolError as exc:                         # e.g. server's wifi is off during a test
            last_error = exc
        elapsed = time.monotonic() - started
        if elapsed >= WIFI_READY_TIMEOUT_S:
            print(f"\n{AP_SSID} not ready after {WIFI_READY_TIMEOUT_S} s" + (f": {last_error}" if last_error else ""))
            return False
        print(f"\rWaiting for {AP_SSID}: {elapsed:3.0f} s / {WIFI_READY_TIMEOUT_S} s", end="", flush=True)
        waited = True
        time.sleep(1)
    if waited:
        print()
    try:
        show_connection()
    except (NetworkToolError, OSError):
        pass                                                    # only information
    return True


def run_with_reconnect(operation):
    """Return operation(connection) on a new connection, repeated on ComError until RECONNECT_TIMEOUT_S.

    Only for operations that can be sent twice.
    """
    deadline = time.monotonic() + RECONNECT_TIMEOUT_S
    while True:
        try:
            with ServerConnection() as server:
                return operation(server)
        except ComError:
            if time.monotonic() >= deadline or not wait_for_server_wifi():
                raise
            time.sleep(1)


# ----- configuration -----

def to_signed16(word):
    return word - 65536 if word >= 32768 else word


def load_config():
    try:
        with CONFIG_FILE.open(encoding="utf-8") as config_file:
            config = json.load(config_file)
        for name in ("vol_timeout_low", "vol_timeout_high", "weight_timeout", "stable_time"):
            if not 0 <= int(config[name]) <= 32767:
                raise ValueError(f"{name} must fit int16 and not be negative")
        if not 0 < int(config["cal_div"]) <= 32767:
            raise ValueError("cal_div must fit int16 and be positive")
        if not -32768 <= int(config["cal_offset"]) <= 32767:
            raise ValueError("cal_offset must fit int16")
        if int(config["weight_timeout"]) * SAMPLES_PER_S > MAX_SAMPLES:
            raise ValueError(f"weight_timeout must be at most {MAX_SAMPLES // SAMPLES_PER_S} s, samples array size is {MAX_SAMPLES}")
        for name in ("samples_file", "calibration_file"):
            if not str(config[name]).strip():
                raise ValueError(f"{name} is empty")
        int(config["target"])
        int(config["samples_counts"])
    except (OSError, KeyError, TypeError, ValueError) as exc:
        raise ConfigError(f"{CONFIG_FILE.name}: {exc!r}") from exc
    return config


def config_words(config):
    return [int(config["vol_timeout_low"]), int(config["vol_timeout_high"]), int(config["weight_timeout"]),
            int(config["stable_time"]), int(config["cal_div"]), int(config["cal_offset"]) & 0xFFFF, 0, 0]


def data_file(config, name):
    return APPLICATION_DIR / f"{str(config[name]).strip()}.csv"


# ----- test commands: start and disconnect -----

def start_test(config, mode, target=0):
    """Write config, mode, target and test registers, then disconnect. Return 0 if the server acknowledged."""
    if not wait_for_server_wifi():
        return 1
    try:
        with ServerConnection() as server:
            server.write_words(CONFIG_ADDRESS, config_words(config))
            server.write_word(MODE_ADDRESS, mode)
            server.write_word(TARGET_ADDRESS, target)
            try:
                server.write_word(TEST_ADDRESS, TEST_START)     # never sent twice, wifi may already be turning off
            except ComError as exc:
                print(f"Start not acknowledged, the test may be running: {exc}")
                return 1
    except ComError as exc:
        print(f"Server not reached, test not started: {exc}")
        return 1
    except ServerError as exc:
        print(f"Start refused, a test may be running: {exc}")
        return 1
    if mode == MODE_VOLUME:
        print("Started, server LED blinks during the test. Run get when it is green again.")
    else:
        print("Started, server wifi turns off and its LED blinks during the test. Run get when it is green again.")
    return 0


def cmd_cal(args, config):
    if not 0 <= args.test_weight <= 0xFFFF:
        print("test_weight must be 0-65535")
        return 1
    print(f"Calibration with test weight {args.test_weight} x 10 mg")
    return start_test(config, MODE_CALIBRATE, args.test_weight)


def cmd_w2t(args, config):
    samples_count = int(config["samples_counts"]) if args.samples_count is None else args.samples_count
    if not MIN_SAMPLES_COUNT <= samples_count <= MAX_SAMPLES:
        print(f"samples_count must be {MIN_SAMPLES_COUNT}-{MAX_SAMPLES}, got {samples_count}")
        return 1
    print(f"Weight to time: {samples_count} samples, approx {samples_count / SAMPLES_PER_S:.0f} s")
    return start_test(config, MODE_WEIGHT2TIME, samples_count)


def cmd_w2g(args, config):
    increment = int(config["target"]) if args.weight_increment is None else args.weight_increment
    if not MIN_INCREMENT <= increment <= MAX_INCREMENT:
        print(f"weight_increment must be {MIN_INCREMENT}-{MAX_INCREMENT}, got {increment}")
        return 1
    print(f"Weight to increment: {increment} x 10 mg, timeout {int(config['weight_timeout'])} s")
    return start_test(config, MODE_WEIGHT2INC, increment)


def cmd_vol(args, config):
    print("Volume test")
    return start_test(config, MODE_VOLUME)


# ----- get: results of the last test -----

def read_samples(count):
    """Read count samples of the last weight test, in 10 mg units."""
    samples = []
    for pointer in range(0, count, REG_SAMPLES):
        def read_segment(server):
            server.write_word(SAMPLES_POINTER_ADDRESS, pointer)
            return server.read_words(SAMPLES_ADDRESS, REG_SAMPLES)
        words = run_with_reconnect(read_segment)                # can be repeated after a drop
        samples.extend(to_signed16(word) for word in words[:count - pointer])
        print(f"\rReading samples: {len(samples)} / {count}", end="", flush=True)
    print()
    return samples


def save_samples(config, samples):
    samples_file = data_file(config, "samples_file")
    with samples_file.open("w", newline="", encoding="utf-8") as csv_file:
        csv.writer(csv_file).writerows([sample] for sample in samples)
    print(f"Saved {len(samples)} readings to {samples_file.name}")


def save_calibration(config, test_weight, raw_weight):
    """Append [test_weight, raw_weight], unless it is the last row already (get run twice)."""
    calibration_file = data_file(config, "calibration_file")
    row = [str(test_weight), str(raw_weight)]
    try:
        with calibration_file.open(newline="", encoding="utf-8") as csv_file:
            rows = list(csv.reader(csv_file))
    except FileNotFoundError:
        rows = []
    if rows and rows[-1] == row:
        print(f"Already in {calibration_file.name}")
        return
    with calibration_file.open("a", newline="", encoding="utf-8") as csv_file:
        csv.writer(csv_file).writerow(row)
    print(f"Appended to {calibration_file.name}")


def get_weight_result(config, mode, target, stat):
    state, value, count, available, time_ds = stat
    if mode == MODE_CALIBRATE:
        if state != STATE_DONE:
            return 1
        raw_weight = value + (count << 16)
        if raw_weight >= 1 << 31:                               # signed 32 bits
            raw_weight -= 1 << 32
        print(f"test_weight = {target} x 10 mg, average raw weight = {raw_weight}")
        save_calibration(config, target, raw_weight)
        return 0

    samples = read_samples(available) if available > 0 else []
    if mode == MODE_WEIGHT2TIME:
        print(f"Samples = {count}, time = {time_ds / 10:.1f} s")
        if state == STATE_DONE:
            print(f"Average weight = {to_signed16(value)} x 10 mg ({to_signed16(value) / 100:.2f} g)")
    else:
        print(f"Target increment = {target} x 10 mg, last weight = {to_signed16(value)} x 10 mg, samples = {available}")
        if state == STATE_DONE:
            print(f"Time from tare to target = {time_ds / 10:.1f} s ({count} samples)")
        elif time_ds > 0:
            print(f"Target not reached, {time_ds / 10:.1f} s from tare to timeout")
        else:
            print("No flow sensed")
    if samples:
        save_samples(config, samples)
    return 0 if state == STATE_DONE else 1


def cmd_get(args, config):
    if not wait_for_server_wifi():
        return 1
    try:
        mode, target = run_with_reconnect(lambda server: server.read_words(LAST_TEST_ADDRESS, LAST_TEST_SIZE))
        if mode == NO_TEST:
            print("No test since the server started")
            return 1
        stat_address, stat_size = (VOL_STAT_ADDRESS, VOL_STAT_SIZE) if mode == MODE_VOLUME else \
            (WEIGHT_STAT_ADDRESS, WEIGHT_STAT_SIZE)
        stat = run_with_reconnect(lambda server: server.read_words(stat_address, stat_size))
        print(f"Last test: {MODE_COMMANDS.get(mode, f'mode {mode}')}, status: {STATE_NAMES.get(stat[0], f'unknown ({stat[0]})')}")
        if stat[0] == STATE_RUNNING:
            print("Run get again when the server LED is green")
            return 1
        if mode == MODE_VOLUME:
            if stat[0] == STATE_DONE:
                print(f"Time from low to high sensor = {stat[1] / 10:.1f} s")
            return 0 if stat[0] == STATE_DONE else 1
        return get_weight_result(config, mode, target, stat)
    except (ComError, ServerError) as exc:
        print(f"\nResults not read: {exc}")
        return 1
    except OSError as exc:
        print(f"File not written: {exc}")
        return 1


def main():
    parser = argparse.ArgumentParser(description="grav_sensor_flow_4 client")
    commands = parser.add_subparsers(dest="command", required=True)
    cal = commands.add_parser("cal", help="start calibration with a known weight")
    cal.add_argument("test_weight", type=int, help="test weight in 10 mg units, 1250 = 12.5 g")
    cal.set_defaults(func=cmd_cal)
    w2t = commands.add_parser("w2t", help="start weight to time")
    w2t.add_argument("samples_count", type=int, nargs="?", help=f"{MIN_SAMPLES_COUNT}-{MAX_SAMPLES}, default samples_counts in config")
    w2t.set_defaults(func=cmd_w2t)
    w2g = commands.add_parser("w2g", help="start weight to increment")
    w2g.add_argument("weight_increment", type=int, nargs="?", help=f"{MIN_INCREMENT}-{MAX_INCREMENT} x 10 mg, default target in config")
    w2g.set_defaults(func=cmd_w2g)
    commands.add_parser("vol", help="start volume test").set_defaults(func=cmd_vol)
    commands.add_parser("get", help="show and save the results of the last test").set_defaults(func=cmd_get)
    args = parser.parse_args()
    try:
        return args.func(args, load_config())
    except ConfigError as exc:
        print(f"Configuration error: {exc}")
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
