# Known issues

- **Next-gen AE compatibility is intentionally narrow.** AE 1.6.1170 is field-tested and every hook is also verified against its Address Library target. On another supported 1.6.1130+ runtime, a moved hook fails safe: it does not install and writes the reason to `FittingRoom.log`. Pre-next-gen AE (1.6.640 and similar) and VR are not supported.
- **Controller right-stick rotation in the editor is experimental.** Rotating the preview with the right stick may feel rough or overlap with camera input on some setups; the rest of the editor is controller-first and stable.
