# Contributing

Contributions are welcome — bug reports, build fixes, and improvements to the
device tree.

## Start here

1. Read the **[README](README.md)** for the big picture and current feature
   status.
2. Build the tree (see the README's *Building* section) and test on a `degas`
   unit.

## Reporting issues

When filing a bug or build issue, include:

- what you did and what happened (vs. what you expected);
- the build command and any error output;
- for runtime issues, a `/tmp/recovery.log` from the device (`adb pull
  /tmp/recovery.log`).

## Pull requests

- Keep changes focused and described clearly.
- Match the surrounding style of the file you're editing.
- Note what you tested (which device, which TWRP function).

## FBE decryption research

`/data` decryption is not achievable from recovery on this device (the metadata
key is bound to the TEE Root‑of‑Trust). The full investigation — including the
`LD_PRELOAD` shim approach used to bring the keystore stack up by hand inside
recovery — lives in the [`fbe-decryption-research`](../../tree/fbe-decryption-research)
branch, not on `main`.

## License

By contributing you agree your contributions are licensed under Apache‑2.0 (with
TWRP‑inherited pieces under their upstream GPL licenses), matching the rest of
the repository.
