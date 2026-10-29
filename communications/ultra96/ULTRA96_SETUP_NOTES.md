# Ultra96 visualizer deployment notes

## Current home-directory ownership issue

Observed on 22 September 2026 while logged in as `xilinx`:

```text
$ id
uid=1000(xilinx) gid=1000(xilinx) groups=1000(xilinx),4(adm),27(sudo)

$ getent passwd xilinx
xilinx:x:1000:1000:Xilinx User,,,:/home/xilinx:/bin/bash

$ ls -ld /home/xilinx
drwxr-x--- 3 127 135 ... /home/xilinx
```

`getent passwd 127` and `getent group 135` produced no output. Therefore,
`/home/xilinx` is owned by orphaned numeric IDs rather than the current
`xilinx` account. This prevents UID 1000 from entering its configured home
directory. Activating or deactivating `pynq-venv` does not cause or fix this;
Python environments do not change filesystem identity or permissions.

Do not run `sudo pip`, store the TLS private key in the inaccessible home
directory, or recursively change ownership without approval from the Ultra96
administrator. The likely permanent repair, after the administrator reviews
the directory contents, is:

```sh
sudo find /home/xilinx -maxdepth 2 -printf '%u:%g %m %p\n'
sudo chown -R xilinx:xilinx /home/xilinx
```

The recursive ownership command is recorded for the administrator; it is not
part of the temporary setup below.

## Temporary setup currently used

The gateway files are staged in the writable directory `/tmp/comms`:

```text
/tmp/comms/
├── server.py
├── visualizer_gateway.py
├── visualizer_protocol.py
├── requirements.txt
└── logs/
    ├── server.log
    └── server.log.1 ... server.log.5
```

Create an isolated environment there so PYNQ's system-managed environment and
its `websockets==10.3` installation remain unchanged:

```sh
cd /tmp/comms
deactivate 2>/dev/null || true
python3 -m venv .visualizer-venv
. .visualizer-venv/bin/activate
python -m pip install --no-cache-dir -r requirements.txt
python -c "import websockets; print(websockets.__version__)"
```

The expected WebSocket version is `12.0`. Do not use `sudo` for these commands.

`/tmp` may be cleared whenever the Ultra96 reboots. After a reboot, check first:

```sh
ls -la /tmp/comms
```

If it is missing, copy the three gateway files from the laptop again, recreate
`.visualizer-venv`, and reinstall `requirements.txt`. A certificate or private
key stored under `/tmp` must also be recreated. Do not commit either one.

The sensor server creates `/tmp/comms/logs` automatically and rotates
`server.log` at 10 MiB while retaining five older segments. These logs are also
temporary; copy any required assessment evidence off the board before reboot.

## Temporary certificate and gateway test

Until `/home/xilinx` ownership is repaired, keep test-only TLS material in a
private temporary directory:

```sh
cd /tmp/comms
install -d -m 700 /tmp/comms/tls
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 1 \
  -keyout /tmp/comms/tls/server-key.pem \
  -out /tmp/comms/tls/server-cert.pem \
  -subj "/CN=192.168.0.2" \
  -addext "subjectAltName=IP:192.168.0.2" \
  -addext "keyUsage=digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth"
chmod 600 /tmp/comms/tls/server-key.pem
```

Start the gateway from the isolated environment:

```sh
cd /tmp/comms
. .visualizer-venv/bin/activate
python visualizer_gateway.py \
  --tls-cert /tmp/comms/tls/server-cert.pem \
  --tls-key /tmp/comms/tls/server-key.pem \
  --demo-source
```

Expected listeners:

```text
WSS:       127.0.0.1:9000
Publisher: 127.0.0.1:9100
```

Only port 9000 is forwarded to the laptop. Port 9100 remains Ultra96-loopback
only. The private key remains on the Ultra96 and is never copied to the laptop.

## After the home directory is repaired

Move to a persistent user-owned layout:

```text
/home/xilinx/.venvs/cg4002-visualizer/
/home/xilinx/.config/cg4002-visualizer/server-cert.pem
/home/xilinx/.config/cg4002-visualizer/server-key.pem
/home/xilinx/cg4002-communications/ultra96/
```

Recreate the environment and certificate rather than copying the temporary
virtual environment. Keep the private-key directory mode at `700` and the key
mode at `600`.
