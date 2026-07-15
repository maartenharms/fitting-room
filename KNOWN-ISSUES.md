# Known issues

- **Anniversary Edition support is new.** The AE build is verified offline against the 1.6.1170 binary (every hook byte-checked and its call target confirmed against the AE Address Library), but in-game AE testing is still ongoing. On a different next-gen AE build, if an engine offset moved, the affected hook fails safe: it does not install, there is no crash, and the reason is written to `FittingRoom.log`. Please report it if you see that line. Pre-next-gen AE (1.6.640 and similar) and VR are not supported.
- **Controller right-stick rotation in the editor is experimental.** Rotating the preview with the right stick may feel rough or overlap with camera input on some setups; the rest of the editor is controller-first and stable.
