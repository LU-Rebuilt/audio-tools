# audio-tools

LEGO Universe FMOD audio tools. Extract and convert FEV event files and FSB sound banks.

> **Note:** This project was developed with significant AI assistance (Claude by Anthropic). All code has been reviewed and validated by the project maintainer, but AI-generated code may contain subtle issues. Contributions and reviews are welcome.

Part of the [LU-Rebuilt](https://github.com/LU-Rebuilt) project.

## Tools

### fsb_extract

Extract audio samples from encrypted FMOD FSB4 sound banks.

```
fsb_extract <input.fsb> [output_dir]
```

Decrypts the FSB using the FMOD project password, then extracts each audio sample with the appropriate file extension based on codec (MP3, ADPCM, XMA, GSM, PCM).

### fev_to_fdp

Convert a single FEV file to FMOD Designer project (FDP) XML format.

```
fev_to_fdp <input.fev> [input1.fsb input2.fsb ...] [output.fdp]
```

Parses the FEV for event/group/category/sounddef structure. Optionally reads FSB files for per-sample metadata (frequency, channels). Generates deterministic GUIDs from object names using FNV-1a.

### fev_project_setup

Set up FMOD Designer project structure from a directory of FEV files.

```
fev_project_setup <input_dir> <output_dir>
```

**Status:** Work in progress. Event and sound definition conversion works. Music system FDP generation is not yet fully functional.

## Building

```bash
cmake -B build
cmake --build build -j$(nproc)
```

For local development:

```bash
cmake -B build -DFETCHCONTENT_SOURCE_DIR_LU_ASSETS=/path/to/local/lu-assets
```

## Acknowledgments

Format parsers built from:
- **[lcdr/lu_formats](https://github.com/lcdr/lu_formats)** — Kaitai Struct FEV/FSB format definitions
- **Ghidra reverse engineering** of the original LEGO Universe client binary
- **FMOD Designer 4.44.64** — FDP XML schema reference from example projects

## License

[GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html) (AGPLv3)

