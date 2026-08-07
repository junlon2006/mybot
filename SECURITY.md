# Security policy

> [English](SECURITY.md) | [简体中文](SECURITY.zh-CN.md)

## Supported versions

Only the latest release-candidate tag is evaluated for security fixes. No version currently has a
stable production-support commitment.

## Reporting a vulnerability

Use the repository Security tab and GitHub private vulnerability reporting when available. Do not
open a public issue for an undisclosed vulnerability. Include affected versions, platform, impact,
reproduction steps, and suggested mitigations. Never include live credentials or user data.

This community project has no response SLA. Disclosure should be coordinated after a fix or
mitigation is available.

## Transport security

- HTTPS is enabled by default. Platform TLS backends must validate the certificate chain and
  hostname, send SNI, and use a maintained trust store.
- Plain HTTP is rejected unless `MYBOT_ALLOW_INSECURE_HTTP=ON` is explicitly compiled for an
  isolated development environment. It must not be enabled in device or release builds.

## Known security limitations

- The Linux file backend is not a production secret store.
- Third-party security advisories and licenses must be reviewed separately.
