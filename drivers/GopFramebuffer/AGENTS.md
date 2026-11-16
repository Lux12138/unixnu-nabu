# Agent instructions

## Responsibility

Maintain UEFI GOP framebuffer and MobileFramebuffer integration.

## Required reading

1. Read this repository's README and [component documentation](https://unixnu.com/drivers/gop-framebuffer).
2. Read the [component catalog](https://unixnu.com/reference/components) and selected SoC and OS profiles in UniXNU.
3. Read the relevant implementation and its build entry point before editing.
4. For ports, read the [porting overview](https://unixnu.com/porting/), [repository specification](https://unixnu.com/porting/device-repository), and [device-tree specification](https://unixnu.com/porting/device-tree).
5. For kernel work, read XNU sources, the selected patch series, and [patch reference](https://unixnu.com/xnu/patch-reference).
6. For image assembly, read [prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), and [UEFI firmware](https://unixnu.com/porting/uefi-firmware) pages on the UniXNU site.
7. For migration, read the [migration guide](https://unixnu.com/porting/migration).

## Component selection

Search the catalog and existing implementations before adding a capability.
Select dependencies by interface, hardware scope, OS ABI, and board inputs.
Keep shared controller operations in their component or SoC repository.
Keep device configuration in the device repository.

Use `GopFramebuffer` for the repository, catalog ID, checkout, and build references.
Preserve source filenames, runtime classes, bundle names, exported symbols, and
protocol identifiers recorded in the catalog.

## Build and verification

Run the build command of the consuming component in the resolved build tree.
Read [building](https://unixnu.com/building) and [bring-up](https://unixnu.com/porting/bring-up).
Run applicable existing checks and report the commands, results, source
revisions, and required inputs. Report hardware observations separately from
build results, with the device and OS profile.

## Documentation

Maintain this README and the relevant English and Chinese pages in UniXNU.
Use concise language describing current interfaces and behavior.
