# Security policy

## Supported versions

Only the latest release-candidate tag is evaluated for security fixes. No version currently has a
stable production-support commitment.

## Reporting a vulnerability

Use the repository Security tab and GitHub private vulnerability reporting when available. Do not
open a public issue for an undisclosed vulnerability. Include affected versions, platform, impact,
reproduction steps, and suggested mitigations. Never include live credentials or user data.

This community project has no response SLA. Disclosure should be coordinated after a fix or
mitigation is available.

## Known security limitations

- The built-in device-service client supports plain HTTP only.
- The Linux file backend is not a production secret store.
- Third-party security advisories and licenses must be reviewed separately.

