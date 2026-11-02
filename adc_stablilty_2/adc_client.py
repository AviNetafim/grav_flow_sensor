"""Command line client for the adc_stability_2 server. Runs on Linux and Windows.

python adc_client.py set-target <grams>     write target weight
python adc_client.py start                  start weight test, wifi turns off
python adc_client.py wait                   wait for test end, show weight_stat
python adc_client.py get-samples <file>     save samples of completed test to csv file

Each command connects, does its work and disconnects. Standard library only.
While waiting, the PC is switched back to the server's wifi if the OS moved to
another network, using nmcli on Linux and netsh on Windows.
"""

import argparse
import csv
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
JOIN_TIMEOUT_S = 10                         # nmcli gives up joining after this time
NETWORK_COMMAND_TIMEOUT_S = 15

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


def ap_network_state():
    """Return (visible, joined) for AP_SSID. Reads the OS network list, does not wait for a scan."""
    ssid = re.escape(AP_SSID)
    if sys.platform == "win32":                                 # netsh has no rescan, Windows refreshes by itself
        networks = run_network_command("netsh", "wlan", "show", "networks")
        interfaces = run_network_command("netsh", "wlan", "show", "interfaces")
        return (re.search(rf"^SSID \d+\s*: {ssid}\s*$", networks, re.MULTILINE) is not None,
                re.search(rf"^\s*SSID\s*: {ssid}\s*$", interfaces, re.MULTILINE) is not None)

    try:
        run_network_command("nmcli", "device", "wifi", "rescan")
    except NetworkToolError:
        pass                                                    # refused while a scan is running, list is still usable
    networks = run_network_command("nmcli", "-t", "-f", "IN-USE,SSID", "device", "wifi", "list", "--rescan", "no")
    return (re.search(rf"^.:{ssid}$", networks, re.MULTILINE) is not None,
            re.search(rf"^\*:{ssid}$", networks, re.MULTILINE) is not None)


def rejoin_ap():
    """Switch to AP_SSID if it is visible and not joined. Return text to show, None if nothing was done.

    Checking first matters: joining a network that is off can drop the current one.
    """
    try:
        visible, joined = ap_network_state()
        if not visible or joined:
            return None
        if sys.platform == "win32":
            run_network_command("netsh", "wlan", "connect", f"name={AP_SSID}")
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
    return 0


def cmd_get_samples(args):
    saved = 0
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
    args = parser.parse_args()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("\nInterrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
