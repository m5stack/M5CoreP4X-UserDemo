# Vendored third-party sources

The following Git-hosted dependencies are stored in this repository as
ordinary source files. A checkout does not need to initialize Git submodules
for these paths.

| Path | Upstream | Commit | License |
| --- | --- | --- | --- |
| `components/otool_esp_component` | <https://github.com/onexs-xsi/otool_esp_component.git> | `82406f282adfe54e7bee5f46f9b54ba935e953b4` | MIT |
| `components/M5IOE1` | <https://github.com/m5stack/M5IOE1.git> | `846eec7d05e25c09013be2acdb8804487f48a62e` | MIT |
| `components/M5PM1` | <https://github.com/m5stack/M5PM1.git> | `be9a5456c007c333e7ac963f33bfde1ffa5d82ee` | MIT |
| `components/otool_esp_component/otool_bmi270/BMI270_SensorAPI` | <https://github.com/boschsensortec/BMI270_SensorAPI.git> | `41129fcfe39c583ee5462d79195741945d51c1fe` | BSD-3-Clause |
| `components/otool_esp_component/otool_bmi270/BMM150_SensorAPI` | <https://github.com/boschsensortec/BMM150_SensorAPI.git> | `0dce0617873cda1f6d51f6b7b961fdc2641e0c7c` | BSD-3-Clause |
| `components/m5_py32bin_tools` | <https://gitlab.m5stack.com/moyongming/m5_py32bin_tools> | `779b99e7e094d5f254f040291a2498b0e369014e` | MIT (declared in `idf_component.yml`) |

The snapshots were exported from the commits above with
`git -c core.autocrlf=false archive` on 2026-07-28. Their upstream license
files are retained in each source tree. The `m5_py32bin_tools` commit does
not contain a standalone license file; its component manifest declares MIT.
The vendored paths are marked `-text` in the root `.gitattributes` so Git
preserves the upstream byte content, including mixed LF and CRLF line
endings.

The upstream-tracked `gui/build` and `gui/dist` directories in
`m5_py32bin_tools` are generated PyInstaller artifacts (about 190 MB) and
are intentionally excluded. The GUI source, build script, specification,
firmware data, examples, and component source are retained.

To update a snapshot, export the new fixed commit into a clean temporary
directory, overlay any nested vendored trees, replace only the corresponding
tracked source directory, update this table, and run the offline dependency
contract plus a full firmware build. Do not copy ignored files, generated
build artifacts, or a submodule's `.git` metadata into the repository.

No active Git submodules remain. Upstream links in component manifests and
documentation are informational metadata, not fetch configuration. ESP-IDF
Component Manager dependencies remain unchanged.
