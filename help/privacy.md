# Privacy and security

## Your data stays on your computer

- TAREEK-Vis reads your simulation files on your computer. It does not send
  them anywhere.
- TAREEK-Vis does not collect usage data and does not check for updates.
- TAREEK-Vis writes its cache and its log file in the `.simvis_cache` folder,
  in the folder of the network file. To remove them, delete that folder.

## Internet connections

TAREEK-Vis connects to the internet only while the background map is on
(**View > Background Map**). It then downloads map tiles over HTTPS from the
provider that you selected: OpenStreetMap, Esri or OpenTopoMap.

- The provider receives your IP address and the map areas that you look at.
  It does not receive your simulation data.
- Each provider has its own privacy policy and terms of use.
- To work offline, keep the background map off.

The downloaded tiles are kept in
`%LOCALAPPDATA%\TAREEK-Vis\TAREEK-Vis\cache\tiles` (at most 300 MB).

## Uninstall

The uninstaller removes the tile cache and the settings of TAREEK-Vis, which
include the list of recent folders. It does not remove the `.simvis_cache`
folders of your runs. To remove them, delete them.

## Links that open your browser

**Help > Report a Problem** and the links in **Help > About** open pages on
GitHub and on the website of the maintainer. TAREEK-Vis sends nothing to
these pages.

## Before you share a log file or a screenshot

- The log file, `simvis.log`, contains the full paths of the files that you
  opened. A path can contain your user name or the name of a project. Read
  the log before you attach it to a public issue, and remove what you do not
  want to share.
- An exported image, chart, video or scenario file can show the name of the
  run folder and IDs from your simulation. Make sure that you can share them.

## Download TAREEK-Vis safely

- Download TAREEK-Vis only from the
  [Releases page](https://github.com/jalal1/TAREEK-Vis/releases) on GitHub.
- The installer is not code-signed, so Windows shows a warning. Click
  **Run anyway** only for a file that you downloaded from the Releases page.
- To report a security problem, do not open a public issue. Contact the
  maintainer through [jalal1.github.io](https://jalal1.github.io/).

## License and warranty

TAREEK-Vis is free software under the GNU General Public License v3.0. It
comes without warranty. Check important results with other methods.
