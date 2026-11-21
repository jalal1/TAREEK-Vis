# Security policy

## Report a security problem

Do not report a security problem in a public issue, pull request, or
discussion. Report it privately with
[GitHub private vulnerability reporting](https://github.com/jalal1/TAREEK-Vis/security/advisories/new).
If you cannot use GitHub, contact the maintainer through
[jalal1.github.io](https://jalal1.github.io/).

Include this data in your report:

- What you did, and what occurred.
- The TAREEK-Vis version (**Help > About**) and your Windows version.
- A small file or run that shows the problem, if you have one.

Do not send real passwords, keys, or private data. A made-up example is
sufficient.

The maintainer replies to each report. We try to fix a confirmed problem in
the next release. Please give us time to release a fix before you tell other
people about the problem.

## Supported versions

Only the [latest release](https://github.com/jalal1/TAREEK-Vis/releases/latest)
gets fixes. Before you report a problem, make sure that it also occurs in the
latest release.

## What is a security problem

- A run, network, or events file that causes TAREEK-Vis to run code.
- A file that causes TAREEK-Vis to read or write files that you did not
  select, other than its cache folder and its log.
- TAREEK-Vis sends data to the network, other than the data that
  [Privacy and security](help/privacy.md) lists.
- A release or an installer that contains code that is not from this
  repository or from the libraries in
  [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## What is not a security problem

Report these items as usual
[issues](https://github.com/jalal1/TAREEK-Vis/issues):

- A crash, a hang, or too much memory use from a bad input file, if it does
  not cause one of the problems above.
- Data that goes to a map tile service when you turn on a background map. See
  [Privacy and security](help/privacy.md).
- The Windows warning for the installer. The installer is not code-signed.
