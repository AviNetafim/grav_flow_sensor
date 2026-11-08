"""Command line client for the grav_sensor_flow_3 server. Runs on Linux and Windows.

python flow_client.py cal <test_weight>        calibrate, append [test_weight, raw_weight] to calibration file
python flow_client.py w2t [samples_count]      weight to time, 10-5000 samples
python flow_client.py w2g [weight_increment]   weight to increment, 500-4000 x 10 mg
python flow_client.py vol                      volume test
python flow_client.py get                      save samples of last weight test to csv file
python flow_client.py wifi-release             Windows: let Windows manage wifi again

test_weight and weight_increment are in 10 mg units (1250 = 12.5 g). An omitted w2t or
w2g argument comes from samples_counts or target in flow_sensor_cfg.json, which is read
from the folder of this script. The csv files are written there too. Standard library only.

Every command first joins the server's wifi if the PC is not on it, using nmcli on Linux
and netsh on Windows. A command that fails because the connection dropped is sent again
after joining again, for up to RECONNECT_TIMEOUT_S. Starting a test is never sent twice.

The server turns wifi off during cal, w2t and w2g, so these commands wait for it to return.
While waiting (and during vol), the PC is switched back to the server's wifi if the OS
moved to another network.

The server answers the Windows internet probes, so Windows sees its wifi as having
internet and normally stays on it. No administrator prompt or IT change is needed.

Windows administrator prompt only, optional: wifi automatic configuration is also turned
off while on the server's wifi, so Windows cannot move at all. It stays off after a
successful w2t or w2g (get follows). It is turned on again while waiting, since Windows
must scan to find the server when its wifi returns, after every other command, after any
failure, and by wifi-release.
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
JOIN_TIMEOUT_S = 10                         # nmcli gives up joining after this time, netsh is not asked again before it
NETWORK_COMMAND_TIMEOUT_S = 15
RECONNECT_TIMEOUT_S = 30                    # a command that can be sent again is retried this long
WIFI_INTERFACE = "Wi-Fi"                    # Windows adapter name, used if netsh wlan show interfaces has no Name line
AP_SUBNET_PREFIX = "192.168.4."             # PC address given by the server's DHCP

APPLICATION_DIR = pathlib.Path(__file__).resolve().parent
CONFIG_FILE = APPLICATION_DIR / "flow_sensor_cfg.json"

MODE_ADDRESS = 0
TARGET_ADDRESS = 1
TEST_ADDRESS = 2
VOL_STAT_ADDRESS = 3
VOL_STAT_SIZE = 2
WEIGHT_STAT_ADDRESS = 4
WEIGHT_STAT_SIZE = 4                        # [test_state, value, count, samples available to read]
SAMPLES_ADDRESS = 5
SAMPLES_POINTER_ADDRESS = 6
CONFIG_ADDRESS = 7
REG_SAMPLES = 32                            # samples register size in words

MODE_WEIGHT2TIME = 0
MODE_WEIGHT2INC = 1
MODE_VOLUME = 2
MODE_CALIBRATE = 3
TEST_START = 1

STATE_PENDING = 0
STATE_NAMES = {1: "done", 2: "timeout", 3: "error"}

MAX_SAMPLES = 5000                          # server samples array size
SAMPLES_PER_S = 10
MIN_SAMPLES_COUNT = 10
MIN_INCREMENT = 500
MAX_INCREMENT = 4000

CAL_TIMEOUT_S = 60                          # server needs up to approx 15 s (wifi off, 25 samples, wifi on), margin for PC to rejoin
TIMEOUT_MARGIN_S = 30                       # margin to restart wifi and reconnect, client timeout > server timeout
WEIGHT_POLL_PERIOD_S = 4                    # time between connection attempts
VOL_POLL_PERIOD_S = 2

FUNC_READ = 0x04
FUNC_WRITE = 0x06
HEADER_SIZE = 7
CODE_INDEX = 7                              # completion code follows the header


class ComError(Exception):
    """Connection failed or the server did not answer correctly."""


class ServerError(Exception):
    """Server answered with a non zero completion code."""


class NetworkToolError(Exception):
    """nmcli or netsh is missing or reported an error."""


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


class ReconnectingConnection:
    """Server connection that is opened again, joining the server's wifi if needed, when it drops.

    Only for times the server's wifi is on. run repeats a failed operation, run_once never does.
    """

    def __init__(self):
        self.connection = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self._close()

    def _close(self):
        if self.connection is not None:
            self.connection.__exit__(None, None, None)
            self.connection = None

    def run(self, operation):
        """Return operation(connection), repeated on ComError until RECONNECT_TIMEOUT_S. ServerError is not repeated."""
        deadline = time.monotonic() + RECONNECT_TIMEOUT_S
        last_status = None
        while True:
            try:
                if self.connection is None:
                    status = join_ap()
                    if status is not None and status != last_status:
                        print(f"\n{status}")
                    last_status = status
                    self.connection = ServerConnection().__enter__()
                return operation(self.connection)
            except ComError:
                self._close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(1)

    def run_once(self, operation):
        """Return operation(connection), sent at most once. Only opening the connection is repeated."""
        self.run(lambda connection: None)
        return operation(self.connection)


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

    netsh only reads the list of the last scan, and has no rescan command. A refused scan is
    not reported, the list is still usable.
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


def ap_network_state():
    """Return (visible, joined) for AP_SSID. Reads the OS network list, does not wait for a scan."""
    ssid = re.escape(AP_SSID)
    if sys.platform == "win32":
        windows_request_scan()
        networks = run_network_command("netsh", "wlan", "show", "networks")
        return (re.search(rf"^SSID \d+\s*: {ssid}\s*$", networks, re.MULTILINE) is not None,
                windows_ap_joined())

    try:
        run_network_command("nmcli", "device", "wifi", "rescan")
    except NetworkToolError:
        pass                                                    # refused while a scan is running, list is still usable
    networks = run_network_command("nmcli", "-t", "-f", "IN-USE,SSID", "device", "wifi", "list", "--rescan", "no")
    return (re.search(rf"^.:{ssid}$", networks, re.MULTILINE) is not None,
            re.search(rf"^\*:{ssid}$", networks, re.MULTILINE) is not None)


def windows_ap_joined():
    interfaces = run_network_command("netsh", "wlan", "show", "interfaces")
    return re.search(rf"^\s*SSID\s*: {re.escape(AP_SSID)}\s*$", interfaces, re.MULTILINE) is not None


def has_ap_address():
    """True if the PC has an address in the server's subnet, i.e. joined and DHCP done."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((ESP32_IP, PORT))                     # sends nothing, only selects the route
            return probe.getsockname()[0].startswith(AP_SUBNET_PREFIX)
    except OSError:
        return False


def windows_interface():
    interfaces = run_network_command("netsh", "wlan", "show", "interfaces")
    match = re.search(r"^\s*Name\s*: (.+?)\s*$", interfaces, re.MULTILINE)
    return match.group(1) if match else WIFI_INTERFACE


def windows_is_admin():
    return ctypes.windll.shell32.IsUserAnAdmin() != 0


def set_wifi_autoconfig(enabled):
    """Windows: turn wifi automatic configuration (scan, roam, reconnect) on or off.

    Return text to show if it failed, None otherwise. Does nothing on Linux.
    A normal prompt cannot change it, so there it is taken to be on, as Windows sets it.
    """
    if sys.platform != "win32":
        return None
    if not windows_is_admin():
        return None                                             # optional, the server's internet probe answers keep Windows on it
    try:
        run_network_command("netsh", "wlan", "set", "autoconfig", f"enabled={'yes' if enabled else 'no'}",
                            f"interface={windows_interface()}")
    except NetworkToolError as exc:
        if enabled:
            return f"Wifi automatic configuration may still be off, run wifi-release again: {exc}"
        return f"Wifi not held on {AP_SSID}, Windows may switch networks: {exc}"
    return None


_last_join_request = float("-inf")             # monotonic time of last netsh connect request


def windows_connect():
    """Ask Windows to join AP_SSID. Connecting is refused while automatic configuration is off."""
    global _last_join_request
    set_wifi_autoconfig(True)                                   # text ignored, a refused connect below reports the problem
    run_network_command("netsh", "wlan", "connect", f"name={AP_SSID}", f"ssid={AP_SSID}",
                        f"interface={windows_interface()}")
    _last_join_request = time.monotonic()


def ap_ready():
    """True if the PC is on AP_SSID and has an address from the server's DHCP."""
    return has_ap_address() and (sys.platform != "win32" or windows_ap_joined())


def join_ap():
    """Join AP_SSID if needed and wait for the address. Return text to show, None if joined.

    Only for times the server's wifi is on: joining a network that is off can drop the current one.
    """
    try:
        if ap_ready():
            return None
        if sys.platform == "win32":                             # netsh returns at once, does not wait for the join
            windows_connect()
            deadline = time.monotonic() + JOIN_TIMEOUT_S
            while not ap_ready():
                if time.monotonic() >= deadline:
                    return f"Could not join {AP_SSID} within {JOIN_TIMEOUT_S} s"
                time.sleep(1)
        else:
            run_network_command("nmcli", "--wait", str(JOIN_TIMEOUT_S), "connection", "up", AP_SSID)
    except NetworkToolError as exc:
        return f"Cannot join {AP_SSID}: {exc}"
    return None


def hold_ap():
    """Join AP_SSID if needed. Windows administrator prompt: also turn automatic configuration off.

    Return text to show, None if all went well.
    """
    status = join_ap()
    if status is not None:
        return status
    return set_wifi_autoconfig(False)


def show(text):
    if text is not None:
        print(text)


def rejoin_ap():
    """Switch to AP_SSID if it is visible and not joined. Return text to show, None if nothing was done.

    Checking first matters: joining a network that is off can drop the current one.
    """
    try:
        visible, joined = ap_network_state()
        if not visible or joined:
            return None
        if sys.platform == "win32":                             # netsh returns at once, does not wait for the join
            if time.monotonic() - _last_join_request < JOIN_TIMEOUT_S:
                return None                                     # previous join request still in progress
            windows_connect()
        else:
            run_network_command("nmcli", "--wait", str(JOIN_TIMEOUT_S), "connection", "up", AP_SSID)
    except NetworkToolError as exc:
        return f"Cannot switch to {AP_SSID}: {exc}"
    return f"Asked OS to switch to {AP_SSID}"


class ConfigError(Exception):
    """flow_sensor_cfg.json is missing or holds a bad value."""


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


def start_test(config, mode, target=None):
    """Write config, mode, target and test registers. Return True if the server acknowledged the start."""
    def write_settings(server):
        server.write_words(CONFIG_ADDRESS, config_words(config))
        server.write_word(MODE_ADDRESS, mode)
        if target is not None:
            server.write_word(TARGET_ADDRESS, target)

    start_sent = False

    def send_start(server):
        nonlocal start_sent
        start_sent = True
        server.write_word(TEST_ADDRESS, TEST_START)

    show(hold_ap())
    try:
        with ReconnectingConnection() as server:
            server.run(write_settings)                          # settings can be written again
            server.run_once(send_start)                         # never twice, wifi may already be turning off
    except ComError as exc:
        if start_sent:
            print(f"Start not acknowledged, the test may be running, let it end before the next command: {exc}")
        else:
            print(f"Server not reached, test not started: {exc}")
        return False
    except ServerError as exc:
        print(f"Start refused: {exc}")
        return False
    return True


def wait_for_result(address, size, timeout_s, period_s, rejoin):
    """Read the stat register until its test state is not pending. Return its words, None on failure."""
    started = time.monotonic()
    next_attempt = started
    last_rejoin_status = None

    def show_elapsed():
        print(f"\rWaiting for server: {time.monotonic() - started:5.0f} s / {timeout_s} s", end="", flush=True)

    while True:
        show_elapsed()
        if time.monotonic() >= next_attempt:
            next_attempt = time.monotonic() + period_s          # a slow attempt delays the next one, never doubles it
            try:
                with ServerConnection() as server:
                    stat = server.read_words(address, size)
            except ComError:                                    # wifi is off during the test, or the OS moved away
                if rejoin:
                    rejoin_status = rejoin_ap()
                    if rejoin_status is not None and rejoin_status != last_rejoin_status:
                        print(f"\n{rejoin_status}")
                    last_rejoin_status = rejoin_status
            except ServerError as exc:
                print(f"\nCould not read test state: {exc}")
                return None
            else:
                if stat[0] != STATE_PENDING:                    # server may still be stopping wifi at start of test
                    show_elapsed()
                    print()
                    return stat
        if time.monotonic() - started >= timeout_s:
            print("\nTimeout: server did not report a result")
            return None
        time.sleep(1)


def wait_for_weight_test(timeout_s):
    show(set_wifi_autoconfig(True))                             # Windows must scan to see the server's wifi return
    return wait_for_result(WEIGHT_STAT_ADDRESS, WEIGHT_STAT_SIZE, timeout_s, WEIGHT_POLL_PERIOD_S, True)


def state_name(state):
    return STATE_NAMES.get(state, f"unknown ({state})")


def cmd_cal(args, config):
    if args.test_weight < 0:
        print("test_weight must not be negative")
        return 1
    if not start_test(config, MODE_CALIBRATE):
        return 1
    print("Calibration started, server turns wifi off for the test")
    stat = wait_for_weight_test(CAL_TIMEOUT_S)
    if stat is None:
        return 1
    if stat[0] != 1:
        print(f"Calibration failed: {state_name(stat[0])}")
        return 1
    raw_weight = stat[1] + (stat[2] << 16)
    if raw_weight >= 1 << 31:                                   # signed 32 bits
        raw_weight -= 1 << 32
    calibration_file = APPLICATION_DIR / f"{str(config['calibration_file']).strip()}.csv"
    try:
        with calibration_file.open("a", newline="", encoding="utf-8") as csv_file:
            csv.writer(csv_file).writerow([args.test_weight, raw_weight])
    except OSError as exc:
        print(f"test_weight = {args.test_weight}, raw_weight = {raw_weight}")
        print(f"Not saved to {calibration_file.name}: {exc}")
        return 1
    print(f"test_weight = {args.test_weight}, raw_weight = {raw_weight}")
    return 0


def cmd_w2t(args, config):
    samples_count = int(config["samples_counts"]) if args.samples_count is None else args.samples_count
    if not MIN_SAMPLES_COUNT <= samples_count <= MAX_SAMPLES:
        print(f"samples_count must be {MIN_SAMPLES_COUNT}-{MAX_SAMPLES}, got {samples_count}")
        return 1
    if not start_test(config, MODE_WEIGHT2TIME, samples_count):
        return 1
    print(f"Weight to time started: {samples_count} samples, server turns wifi off for the test")
    stat = wait_for_weight_test(samples_count // SAMPLES_PER_S + TIMEOUT_MARGIN_S)
    if stat is None:
        return 1
    print(f"Result: {state_name(stat[0])}")
    print(f"Average weight = {to_signed16(stat[1])} x 10 mg")
    print(f"Loop counter   = {stat[2]}")
    show(hold_ap())                                             # Windows stays on the server's wifi for get
    return 0 if stat[0] == 1 else 1


def cmd_w2g(args, config):
    increment = int(config["target"]) if args.weight_increment is None else args.weight_increment
    if not MIN_INCREMENT <= increment <= MAX_INCREMENT:
        print(f"weight_increment must be {MIN_INCREMENT}-{MAX_INCREMENT}, got {increment}")
        return 1
    if not start_test(config, MODE_WEIGHT2INC, increment):
        return 1
    print(f"Weight to increment started: {increment} x 10 mg, server turns wifi off for the test")
    stat = wait_for_weight_test(int(config["weight_timeout"]) + TIMEOUT_MARGIN_S)
    if stat is None:
        return 1
    print(f"Result: {state_name(stat[0])}")
    print(f"Samples from flow start to stop = {stat[2]}")
    show(hold_ap())                                             # Windows stays on the server's wifi for get
    return 0 if stat[0] == 1 else 1


def cmd_vol(args, config):
    if not start_test(config, MODE_VOLUME):
        return 1
    print("Volume test started")
    # the server waits for the low level, then for the high level, each with its own timeout
    timeout_s = int(config["vol_timeout_low"]) + int(config["vol_timeout_high"]) + TIMEOUT_MARGIN_S
    stat = wait_for_result(VOL_STAT_ADDRESS, VOL_STAT_SIZE, timeout_s, VOL_POLL_PERIOD_S, True)  # wifi stays on
    if stat is None:
        return 1
    print(f"Result: {state_name(stat[0])}")
    print(f"Time to fill = {stat[1]} ds")
    return 0 if stat[0] == 1 else 1


def cmd_get(args, config):
    samples_file = APPLICATION_DIR / f"{str(config['samples_file']).strip()}.csv"
    saved = 0

    def read_segment(server, pointer):
        server.write_word(SAMPLES_POINTER_ADDRESS, pointer)
        return server.read_words(SAMPLES_ADDRESS, REG_SAMPLES)

    show(hold_ap())
    try:
        with ReconnectingConnection() as server:
            state, _, _, count = server.run(lambda connection: connection.read_words(WEIGHT_STAT_ADDRESS, WEIGHT_STAT_SIZE))
            if state == STATE_PENDING:                          # existing file is kept in these two cases
                print("No completed weight test, samples are read after it completes")
                return 1
            if not 0 < count <= MAX_SAMPLES:                    # calibrate samples are raw and cannot be read
                print(f"Server has no samples to read (count = {count})")
                return 1
            with samples_file.open("w", newline="", encoding="utf-8") as csv_file:
                writer = csv.writer(csv_file)
                for pointer in range(0, count, REG_SAMPLES):
                    words = server.run(lambda connection: read_segment(connection, pointer))  # repeated after a drop
                    for word in words[:count - pointer]:
                        writer.writerow([to_signed16(word)])    # signed int16 weight in 10 mg
                        saved += 1
                    print(f"\rReading samples: {saved} / {count}", end="", flush=True)
    except (ComError, ServerError, OSError) as exc:
        print(f"\nSamples not read: {exc}")
        if saved > 0:
            print(f"{samples_file.name} is incomplete, it holds {saved} samples")
        return 1
    print(f"\nSaved {saved} samples to {samples_file.name}")
    return 0


def cmd_wifi_release(args):
    if sys.platform != "win32":
        print("Nothing to do, only used on Windows")
        return 0
    if not windows_is_admin():
        print("Run wifi-release from an administrator prompt")
        return 1
    status = set_wifi_autoconfig(True)
    if status is not None:
        print(status)
        return 1
    print("Windows manages wifi again")
    return 0


# Windows: after success these commands leave automatic configuration off so the PC stays on
# the server's wifi for get. Every other command, and every failure, turns it on again.
KEEPS_WIFI_SETTING = {"w2t", "w2g"}


def main():
    parser = argparse.ArgumentParser(description="grav_sensor_flow_3 client")
    commands = parser.add_subparsers(dest="command", required=True)
    cal = commands.add_parser("cal", help="calibrate with a known weight")
    cal.add_argument("test_weight", type=int, help="test weight in 10 mg units, 1250 = 12.5 g")
    cal.set_defaults(func=cmd_cal)
    w2t = commands.add_parser("w2t", help="weight to time")
    w2t.add_argument("samples_count", type=int, nargs="?", help=f"{MIN_SAMPLES_COUNT}-{MAX_SAMPLES}, default samples_counts in config")
    w2t.set_defaults(func=cmd_w2t)
    w2g = commands.add_parser("w2g", help="weight to increment")
    w2g.add_argument("weight_increment", type=int, nargs="?", help=f"{MIN_INCREMENT}-{MAX_INCREMENT} x 10 mg, default target in config")
    w2g.set_defaults(func=cmd_w2g)
    commands.add_parser("vol", help="volume test").set_defaults(func=cmd_vol)
    commands.add_parser("get", help="save samples of last weight test to csv file").set_defaults(func=cmd_get)
    commands.add_parser("wifi-release", help="Windows: turn wifi automatic configuration on again"
                        ).set_defaults(func=cmd_wifi_release)
    args = parser.parse_args()
    result = 1
    try:
        if args.command == "wifi-release":
            result = args.func(args)
        else:
            result = args.func(args, load_config())
    except ConfigError as exc:
        print(f"Configuration error: {exc}")
    except KeyboardInterrupt:
        print("\nInterrupted")
        result = 130
    finally:                                                    # also runs on an unexpected exception
        if args.command != "wifi-release" and not (result == 0 and args.command in KEEPS_WIFI_SETTING):
            show(set_wifi_autoconfig(True))
    return result


if __name__ == "__main__":
    sys.exit(main())
