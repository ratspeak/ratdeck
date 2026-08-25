"""Apply microReticulum resource-transfer fixes needed for LXMF voice memos
(and any other oversized link payloads).

Fixes (idempotent):
  1) Link.cpp RESOURCE complete → deliver assembled bytes via set_packet_callback
  2) Resource.cpp get_initial_request → request ALL parts (window was 4 only;
     no continuation path exists, so >4 parts stalled forever)
  3) Link.cpp RESOURCE_PRF → erase outbound resource on proof success
  4) Link.cpp start_resource_transfer → drop COMPLETE (and force-clear stale)
     outbound slots so ready_for_new_resource() is not permanently poisoned

PlatformIO pre-script and/or standalone:
  python3 scripts/patch_micror_resource_callback.py
"""
from __future__ import annotations

import pathlib
import sys

# ---------------------------------------------------------------------------
# 1) Inbound resource complete → packet callback
# ---------------------------------------------------------------------------
MARKER_CB = "resource packet callback"

OLD_CB = r"""				case Type::Packet::RESOURCE:
				{
					// Resource data part — NOT link-encrypted (encrypted at resource level)
					for (auto& resource : _object->_inbound_resources) {
						if (resource->receive_part(packet.data())) {
							if (resource->is_complete()) {
								// Assemble the resource
								Bytes assembled = resource->assemble(*this);
								if (assembled.size() > 0) {
									// Send proof back
									Bytes proof = resource->generate_proof();
									if (proof.size() > 0) {
										Bytes encrypted_proof = encrypt(proof);
										if (encrypted_proof) {
											Packet proof_packet(*this, encrypted_proof, Type::Packet::PROOF, Type::Packet::RESOURCE_PRF);
											proof_packet.send();
											had_outbound(true);
										}
									}
									// Store assembled data for later retrieval
									_object->_last_resource_data = assembled;
									DEBUGF("Inbound resource complete, %d bytes assembled", (int)assembled.size());
								}
							}
							break;
						}
					}
					break;
				}"""

NEW_CB = r"""				case Type::Packet::RESOURCE:
				{
					// Resource data part — NOT link-encrypted (encrypted at resource level)
					for (auto it = _object->_inbound_resources.begin();
						 it != _object->_inbound_resources.end(); ++it) {
						auto& resource = *it;
						if (resource->receive_part(packet.data())) {
							if (resource->is_complete()) {
								// Assemble the resource
								Bytes assembled = resource->assemble(*this);
								if (assembled.size() > 0) {
									// Send proof back
									Bytes proof = resource->generate_proof();
									if (proof.size() > 0) {
										Bytes encrypted_proof = encrypt(proof);
										if (encrypted_proof) {
											Packet proof_packet(*this, encrypted_proof, Type::Packet::PROOF, Type::Packet::RESOURCE_PRF);
											proof_packet.send();
											had_outbound(true);
										}
									}
									// Store assembled data for later retrieval
									_object->_last_resource_data = assembled;
									DEBUGF("Inbound resource complete, %d bytes assembled", (int)assembled.size());
									// Deliver to app the same way as normal link DATA (LXMF etc.).
									// Without this, oversized link payloads (resource path) never
									// reach set_packet_callback consumers.
									if (_object->_callbacks._packet) {
										try {
											Serial.printf("[LINK-RX] → resource packet callback, %d bytes\n",
												(int)assembled.size());
											_object->_callbacks._packet(assembled, packet);
											Serial.println("[LINK-RX] ← resource packet callback done");
										}
										catch (std::exception& e) {
											ERRORF("Error while executing packet callback from resource on %s. The contained exception was: %s",
												toString().c_str(), e.what());
										}
									}
								}
								_object->_inbound_resources.erase(it);
							}
							break;
						}
					}
					break;
				}"""

# ---------------------------------------------------------------------------
# 2) Request ALL parts up front (no window continuation in this port)
# ---------------------------------------------------------------------------
MARKER_REQ = "request ALL map hashes (no windowing"

OLD_REQ = r"""    // Request all map hashes (initial window)
    size_t count = std::min(_window, _map_hashes.size());
    for (size_t i = 0; i < count; i++) {
        request.append(_map_hashes[i].data(), 4);
    }"""

NEW_REQ = r"""    // Request ALL map hashes (no windowing continuation exists in this port;
    // min(_window=4, N) permanently stalled any resource >4 parts).
    // Voice memos are small (≤~12 parts); request payload stays tiny.
    size_t count = _map_hashes.size();  // request ALL map hashes (no windowing
    for (size_t i = 0; i < count; i++) {
        request.append(_map_hashes[i].data(), 4);
    }"""

# ---------------------------------------------------------------------------
# 3) Erase outbound resource when proof validates
# ---------------------------------------------------------------------------
MARKER_PRF = "Outbound resource proof validated — slot freed"

OLD_PRF = r"""				if (packet.context() == Type::Packet::RESOURCE_PRF) {
					const Bytes plaintext = decrypt(packet.data());
					if (plaintext) {
						for (auto& resource : _object->_outbound_resources) {
							if (resource->handle_proof(plaintext)) {
								DEBUG("Outbound resource proof validated");
								break;
							}
						}
					}
				}"""

NEW_PRF = r"""				if (packet.context() == Type::Packet::RESOURCE_PRF) {
					const Bytes plaintext = decrypt(packet.data());
					if (plaintext) {
						for (auto it = _object->_outbound_resources.begin();
							 it != _object->_outbound_resources.end(); ++it) {
							if ((*it)->handle_proof(plaintext)) {
								// Outbound resource proof validated — slot freed
								DEBUG("Outbound resource proof validated");
								_object->_outbound_resources.erase(it);
								break;
							}
						}
					}
				}"""

# ---------------------------------------------------------------------------
# 4) start_resource_transfer: clear COMPLETE / stale outbound slots
# ---------------------------------------------------------------------------
MARKER_START = "RES-TX] cleared"

OLD_START = r"""bool Link::start_resource_transfer(const Bytes& data) {
	assert(_object);
	if (!ready_for_new_resource()) return false;"""

NEW_START = r"""bool Link::start_resource_transfer(const Bytes& data) {
	assert(_object);
	// Drop completed outbound resources so a new transfer can start.
	// Without erase-on-proof (and this sweep), one transfer permanently
	// poisons ready_for_new_resource() for the life of the link.
	{
		auto& outs = _object->_outbound_resources;
		outs.erase(std::remove_if(outs.begin(), outs.end(),
			[](const std::shared_ptr<OutboundResource>& r) {
				return r && r->status() == ResourceStatus::COMPLETE;
			}), outs.end());
		// Stale TRANSFERRING (lost proof / stalled window) still blocks —
		// force-clear so voice retries work without reboot.
		if (!outs.empty()) {
			Serial.printf("[RES-TX] cleared %d stale outbound resource(s)\n",
				(int)outs.size());
			outs.clear();
		}
	}
	if (!ready_for_new_resource()) return false;"""


def find_files(project_dir: pathlib.Path, name: str) -> list[pathlib.Path]:
    libdeps = project_dir / ".pio" / "libdeps"
    if not libdeps.is_dir():
        return []
    return list(libdeps.glob(f"*/microReticulum/src/{name}"))


def apply_one(path: pathlib.Path, marker: str, old: str, new: str) -> str:
    text = path.read_text(encoding="utf-8")
    if marker in text:
        return f"skip (already): {path.name} [{marker[:32]}…]"
    if old not in text:
        return f"FAIL (pattern not found): {path} [{marker[:32]}…]"
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return f"patched: {path.name} [{marker[:40]}]"


def apply(project_dir: pathlib.Path) -> int:
    rc = 0
    jobs: list[tuple[str, str, str, str]] = [
        ("Link.cpp", MARKER_CB, OLD_CB, NEW_CB),
        ("Link.cpp", MARKER_PRF, OLD_PRF, NEW_PRF),
        ("Link.cpp", MARKER_START, OLD_START, NEW_START),
        ("Resource.cpp", MARKER_REQ, OLD_REQ, NEW_REQ),
    ]
    any_paths = False
    for fname, marker, old, new in jobs:
        paths = find_files(project_dir, fname)
        if not paths:
            continue
        any_paths = True
        for p in paths:
            msg = apply_one(p, marker, old, new)
            print(f"[patch_micror] {msg}")
            if msg.startswith("FAIL"):
                rc = 1
    if not any_paths:
        print("[patch_micror] no microReticulum under .pio/libdeps yet")
    return rc


# PlatformIO loads this file with Import available.
try:
    Import("env")  # type: ignore[name-defined]
    _project = pathlib.Path(env["PROJECT_DIR"])  # type: ignore[name-defined]

    def _before_build(source, target, env):  # noqa: ARG001
        apply(pathlib.Path(env["PROJECT_DIR"]))

    apply(_project)
    env.AddPreAction("buildprog", _before_build)  # type: ignore[name-defined]
except NameError:
    pass


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parents[1]
    if len(sys.argv) > 1:
        root = pathlib.Path(sys.argv[1]).resolve()
    sys.exit(apply(root))
