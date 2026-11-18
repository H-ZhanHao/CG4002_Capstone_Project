# Ultra96 deployment notes

Use [Network setup](../docs/NETWORK_SETUP.md) for the route and certificates, and
[Ultra96 services](README.md) for the selected server/module layout. Paths below
are examples or dated observations; this refresh did not inspect the live board.

## Persistent layout

Choose actual writable paths and retain a complete, consistent repository
revision. A suitable layout, when the account owns its home, is:

| Path | Purpose |
| --- | --- |
| `$HOME/CG4002_Capstone_Project/` | Deployment checkout, retaining `server/` and `communications/`. |
| `$HOME/.venvs/cg4002-visualizer/` | Isolated gateway environment. |
| `$HOME/.config/cg4002-visualizer/` | Gateway public certificate and private key. |
| Writable explicit `--log-file` directory | Persistent rotating server logs. |
| Board's working AI environment | Matching PYNQ/VART/XIR and model dependencies. |

Create a gateway-only environment from the deployment root:

```bash
install -d -m 700 "$HOME/.venvs"
python3 -m venv "$HOME/.venvs/cg4002-visualizer"
. "$HOME/.venvs/cg4002-visualizer/bin/activate"
python -m pip install -r communications/ultra96/requirements.txt
python -c "import websockets; print(websockets.__version__)"
```

The pinned gateway version is 12.0. This environment isolates the gateway from
PYNQ's system environment; do not use `sudo pip` or assume it supplies the
board-specific AI runtime. Run the integrated server in its compatible working
AI environment, with NumPy/SciPy, compiled xmodels and the matching overlay files.

Provision the gateway certificate/key using
[the certificate procedure](../docs/NETWORK_SETUP.md#3-certificates-two-separate-endpoints).
Protect its directory with mode 700 and key with mode 600. Transfer only the
public certificate to the Unity build machine and rebuild for a changed pin.