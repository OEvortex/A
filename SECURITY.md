# Security policy

The A toolchain executes local source files and is not a sandbox. Programs can
read and write files and consume CPU and memory through the standard library.

## Reporting

Please report suspected vulnerabilities privately rather than opening a public
issue. Use GitHub's [private vulnerability reporting](https://github.com/OEvortex/A/security/advisories/new)
on this repository; the maintainers receive it as a private advisory. Include a
minimal reproducer, the A version (`a --version`), and the host platform.

## Scope

- The compiler, interpreter, and standard library in this repository.
- Malformed input that crashes the compiler instead of producing a diagnostic.

Out of scope: denial of service through intentionally expensive programs and
misuse of the file or process APIs on a host the attacker already controls.
