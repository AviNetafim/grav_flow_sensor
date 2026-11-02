"""Command line client for the adc_stability_2 server. Runs on Linux and Windows.

python adc_client.py set-target <grams>     write target weight
python adc_client.py start                  start weight test, wifi turns off
python adc_client.py wait                   wait for test end, show weight_stat
python adc_client.py get-samples <file>     save samples of completed test to csv file
python adc_client.py wifi-release           Windows: let Windows manage wifi again

Each command connects, does its work and disconnects. Standard library only.
While waiting, the PC is switched back to the server's wifi if the OS moved to
another network, using nmcli on Linux and netsh on Windows.

Windows only: set-target, start and get-samples first join the server's wifi if
needed. Windows wifi automatic configuration is then turned off, so Windows cannot
move to a network with internet. It stays off after set-target (start follows) and
after a successful wait (get-samples follows). It is turned on again after start,
since wait needs it to find the server when its wifi returns, after get-samples,
after any failure, and by wifi-release.

Changing automatic configuration needs an administrator prompt. From a normal prompt
every command still joins the server's wifi, but Windows may move to another network
between commands, and wifi-release is refused.
"""

import argparse
import csv
import ctypes
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
WIFI_INTERFACE = "Wi-Fi"                    # Windows adapter name, used if netsh wlan show interfaces has no Name line
AP_SUBNET_PREFIX = "192.168.4."             # PC address given by the server's DHCP

TARGET_WEIGHT_ADDRESS = 1
TEST_ADDRESS = 2
WEIGHT_STAT_ADDRESS = 4
WEIGHT_STAT_SIZE = 3
SAMPLES_ADDRESS = 5
SAMPLES_POINTER_ADDRESS = 6
REG_SAMPLES = 32                            # samples register size in words
TEST_RUN = 1
MAX_TARGET_G = 80

WEIGHT_TEST_TIMEOUT_S = 200                 # server stops test after 180 s, margin to restart wifi and reconnect
RECONNECT_PERIOD_S = 4                      # time between connection attempts

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
        cmd.extend([word & 0xFF, word >> 8])
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

    def write_word(self, address, value):
        self._transact(build_command(FUNC_WRITE, address, 1, [value]), 0)

    def read_words(self, address, size):
        data = self._transact(build_command(FUNC_READ, address, size), size * 2)
        return [data[i] + 256 * data[i + 1] for i in range(0, len(data), 2)]


def read_weight_stat():
    with ServerConnection() as server:
        return server.read_words(WEIGHT_STAT_ADDRESS, WEIGHT_STAT_SIZE)


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
        return None if enabled else f"Not an administrator prompt, Windows may move off {AP_SSID} between commands"
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


def hold_ap():
    """Windows: join AP_SSID if needed, then turn automatic configuration off so Windows stays on it.

    Return text to show, None if all went well. Does nothing on Linux.
    """
    if sys.platform != "win32":
        return None
    try:
        if not (windows_ap_joined() and has_ap_address()):
            windows_connect()
            deadline = time.monotonic() + JOIN_TIMEOUT_S
            while not (windows_ap_joined() and has_ap_address()):
                if time.monotonic() >= deadline:
                    return f"Could not join {AP_SSID} within {JOIN_TIMEOUT_S} s"
                time.sleep(1)
    except NetworkToolError as exc:
        return f"Cannot join {AP_SSID}: {exc}"
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


def cmd_set_target(args):
    if not 0 < args.grams < MAX_TARGET_G:
        print(f"target must be greater than 0 and less than {MAX_TARGET_G} g")
        return 1
    target_10mg = round(args.grams * 100)
    show(hold_ap())
    try:
        with ServerConnection() as server:
            server.write_word(TARGET_WEIGHT_ADDRESS, target_10mg)
    except ComError as exc:
        print(f"Target not set: {exc}")
        return 1
    except ServerError as exc:
        print(f"Target refused: {exc}")
        return 1
    print(f"Target set: {args.grams:g} g ({target_10mg} x 10 mg)")
    return 0


def cmd_start(args):
    show(hold_ap())
    try:
        with ServerConnection() as server:
            server.write_word(TEST_ADDRESS, TEST_RUN)
    except ComError as exc:                                     # no retry, wifi may already be turning off
        print(f"Start not acknowledged, assuming server is disconnected: {exc}")
        return 1
    except ServerError as exc:
        print(f"Start refused: {exc}")
        return 1
    print("Test started, server turns wifi off for the test")
    return 0


def cmd_wait(args):
    started = time.monotonic()
    next_attempt = started
    last_rejoin_status = None
    show(set_wifi_autoconfig(True))                             # Windows must scan to see the server's wifi return

    def show_elapsed():
        elapsed = time.monotonic() - started
        print(f"\rWaiting for server: {elapsed:5.0f} s / {WEIGHT_TEST_TIMEOUT_S} s", end="", flush=True)

    while True:
        show_elapsed()
        if time.monotonic() >= next_attempt:
            next_attempt += RECONNECT_PERIOD_S
            try:
                stat = read_weight_stat()
            except ComError:                                    # wifi is off during the test, or the OS moved away
                rejoin_status = rejoin_ap()
                if rejoin_status is not None and rejoin_status != last_rejoin_status:
                    print(f"\n{rejoin_status}")
                last_rejoin_status = rejoin_status
            except ServerError as exc:
                print(f"\nCould not read weight_stat: {exc}")
                return 1
            else:
                if stat[0] != TEST_RUN:                         # server may still be stopping wifi at start of test
                    break
        if time.monotonic() - started >= WEIGHT_TEST_TIMEOUT_S:
            print("\nWeight test timeout: server did not come back")
            return 1
        time.sleep(1)

    show_elapsed()
    print()
    print(f"weight_stat[0] test state = {stat[0]} (0=stop, 1=run)")
    print(f"weight_stat[1] test time  = {stat[1]} ds")
    print(f"weight_stat[2] samples    = {stat[2]}")
    show(hold_ap())                                             # Windows stays on the server's wifi for get-samples
    return 0


def cmd_get_samples(args):
    saved = 0
    show(hold_ap())
    try:
        with ServerConnection() as server:
            state, _, count = server.read_words(WEIGHT_STAT_ADDRESS, WEIGHT_STAT_SIZE)
            if state == TEST_RUN:                               # existing file is kept in these two cases
                print("Weight test is running, samples are read after it completes")
                return 1
            if count == 0:
                print("Server has no samples")
                return 1
            with open(args.file, "w", newline="", encoding="utf-8") as csv_file:
                writer = csv.writer(csv_file)
                for pointer in range(0, count, REG_SAMPLES):
                    server.write_word(SAMPLES_POINTER_ADDRESS, pointer)
                    words = server.read_words(SAMPLES_ADDRESS, REG_SAMPLES)
                    for word in words[:count - pointer]:
                        writer.writerow([word - 65536 if word >= 32768 else word])      # signed int16 weight in 10 mg
                        saved += 1
                    print(f"\rReading samples: {saved} / {count}", end="", flush=True)
    except (ComError, ServerError, OSError) as exc:
        print(f"\nSamples not read: {exc}")
        if saved > 0:
            print(f"{args.file} is incomplete, it holds {saved} samples")
        return 1
    print(f"\nSaved {saved} samples to {args.file}")
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


# Windows: after success these commands leave automatic configuration as they set it,
# off after set-target and wait so the PC stays on the server's wifi for the next command.
# Every other command, and every failure, turns it on again. wifi-release has already tried.
KEEPS_WIFI_SETTING = {"set-target", "wait"}


def main():
    parser = argparse.ArgumentParser(description="adc_stability_2 weight test client")
    commands = parser.add_subparsers(dest="command", required=True)
    set_target = commands.add_parser("set-target", help="write target weight increment")
    set_target.add_argument("grams", type=float, help=f"target in g, 0-{MAX_TARGET_G}")
    set_target.set_defaults(func=cmd_set_target)
    commands.add_parser("start", help="start weight test").set_defaults(func=cmd_start)
    commands.add_parser("wait", help="wait for test end and show weight_stat").set_defaults(func=cmd_wait)
    get_samples = commands.add_parser("get-samples", help="save samples of completed test to csv file")
    get_samples.add_argument("file", help="csv file name, existing file is overwritten")
    get_samples.set_defaults(func=cmd_get_samples)
    commands.add_parser("wifi-release", help="Windows: turn wifi automatic configuration on again"
                        ).set_defaults(func=cmd_wifi_release)
    args = parser.parse_args()
    result = 1
    try:
        result = args.func(args)
    except KeyboardInterrupt:
        print("\nInterrupted")
        result = 130
    finally:                                                    # also runs on an unexpected exception
        if args.command != "wifi-release" and not (result == 0 and args.command in KEEPS_WIFI_SETTING):
            show(set_wifi_autoconfig(True))
    return result


if __name__ == "__main__":
    sys.exit(main())
