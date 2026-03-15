# SPU/VAG Playback Plugin

Self-contained PlayStation SPU ADPCM audio decoder. No external library
is used -- the decoder (~50 lines of C) is embedded directly in the plugin.

## Supported Formats

| Format | Extension | Description |
|--------|-----------|-------------|
| VAG | .vag | PS-ADPCM with 48-byte header (magic "VAGp") |
| VB | .vb | Raw PS-ADPCM blocks (no header) |

## Audio Format

PS-ADPCM encodes audio as 16-byte blocks, each producing 28 signed 16-bit
samples. The block format is:

- Byte 0: shift (bits 0-3) + filter index (bits 4-6)
- Byte 1: flags (loop markers: 1=end, 2=loop region, 4=loop start)
- Bytes 2-15: packed 4-bit nibbles (28 samples, low nibble first)

Five IIR filter coefficient sets are used for adaptive prediction.

## VAG Header

Files with the "VAGp" magic contain a 48-byte header with:

- Offset 16: sample rate (4 bytes, big-endian)
- Offset 32: name (16 bytes, null-terminated)

Raw .vb files assume a default sample rate of 44100 Hz.

## Source

All decoder code is original, written based on the public PS-ADPCM
specification. No external library or third-party code is used.
